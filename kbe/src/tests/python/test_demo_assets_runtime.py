"""验证 demo assets 的登录、跨 Cell 显式传送、客户端退出和服务端正常退出。"""

from __future__ import annotations

import argparse
from contextlib import ExitStack
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
import uuid
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from cell_handoff_runtime import verify_handoff_destroy_timer
from performance.assets import build_environment, create_config_overlay, find_server_config, set_xml_value
from performance.log_metrics import DEFAULT_PATTERNS
from performance.run import resolve_management_admin_token, wait_for_cluster_ready_for_login
from performance.topology import build_local_cluster_components, platform_executable_name
from runtime_console import RuntimeConsole
from space_boundary_runtime import verify_boundary_move
from space_partition_load import stop_demo_bot_actions, verify_partition_load
from space_boundary_compare import verify_boundary_compare
from space_split_runtime import LiveSpaceAdjustment, verify_space_merge, verify_space_split
from space_layout_runtime import (
    NO_NAVIGATION_MAPPING, verify_boundary_ghosts, verify_fixed_layout, verify_ghost_source_change,
    verify_layout_removal, verify_rejected_layout_removal,
)
from test_server_cluster import ERROR_PATTERN, all_run_logs, read_text, run_cluster
from test_server_cluster import select_interfaces_port as select_tcp_port


SHUTDOWN_GROUPS = (
    ("loginapp", "baseapp", "cellapp", "baseappmgr", "cellappmgr", "dbmgr"),
)
INFRASTRUCTURE_COMPONENTS = ("machine", "logger", "interfaces")


def require(condition: bool, message: str):
    if not condition:
        raise RuntimeError(message)


def validate_test_database(database: ET.Element, original_name: str, expected_name: str) -> str:
    database_name = database.findtext("databaseName", "").strip()
    # Windows 上数据库名可能不区分大小写，不能只靠大小写变化避开业务库检查。
    normalized_name = database_name.casefold()
    require(database_name == expected_name and bool(database_name)
            and normalized_name != original_name.casefold()
            and normalized_name not in {"kbe", "mysql", "sys", "information_schema", "performance_schema"},
            "必须显式提供不同于原 assets 的独立测试数据库")
    require(database.findtext("./auth/encrypt", "false").strip().lower() in ("false", "0"),
            "本次运行配置必须使用明文测试凭据，不能把加密内容改为明文标记后使用")
    return database_name


def wait_until(description, action, ready, timeout=60):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        last = action()
        if ready(last):
            return last
        time.sleep(0.2)
    raise TimeoutError(f"{description}超时，最后结果：{last!r}")


def cell_entities(console):
    return console.evaluate(
        "[{'id': e.id, 'type': type(e).__name__, 'space': e.spaceID, "
        "'utype': e.spaceUType, 'real': e.isReal(), "
        "'position': list(e.position), 'direction': list(e.direction)} "
        "for e in KBEngine.entities.values() "
        "if type(e).__name__ in ('Avatar', 'Space')]"
    )


def verify_partitioning_rejection(entries, run_root, environment, args, output):
    extra_root = output / "rejected-cell"
    config_path = extra_root / "res/server/kbengine.xml"
    config_path.parent.mkdir(parents=True, exist_ok=False)
    source_root = Path(environment["KBE_RES_PATH"].split(os.pathsep)[0])
    tree = ET.parse(source_root / "server/kbengine.xml")
    root = tree.getroot()
    manager = root.find("cellappmgr")
    partitioning = manager.find("spacePartitioning")
    if partitioning is None:
        partitioning = ET.SubElement(manager, "spacePartitioning")

    enabled = args.space_partitioning == "true"
    expected_error = "Cellapp::onSpacePartitioningMode: incompatible manager"
    if args.partition_rejection == "mode":
        set_xml_value(partitioning, "enabled", str(not enabled).lower())
    elif args.partition_rejection == "invalid":
        set_xml_value(partitioning, "enabled", "not-a-boolean")
        expected_error = "cellappmgr/spacePartitioning/enabled 'not-a-boolean' is invalid"
    else:
        set_xml_value(partitioning, "enabled", "true")
        cell = root.find("cellapp")
        coordinates = cell.find("coordinate_system")
        if coordinates is None:
            coordinates = ET.SubElement(cell, "coordinate_system")
        if args.partition_rejection == "coordinates":
            set_xml_value(coordinates, "enable", "false")
            expected_error = "space partitioning requires cellapp/coordinate_system/enable=true"
        else:
            require(enabled, "坐标轴不一致检查必须在分割开启模式下执行")
            defaults = ET.parse(args.repository_root / "kbe/res/server/kbengine_defaults.xml").getroot()
            default_y = defaults.findtext("./cellapp/coordinate_system/rangemgr_y").strip().lower()
            has_y = coordinates.findtext("rangemgr_y", default_y).strip().lower() == "true"
            set_xml_value(coordinates, "rangemgr_y", str(not has_y).lower())
    tree.write(config_path, encoding="utf-8", xml_declaration=True)

    extra_environment = dict(environment)
    extra_environment["KBE_RES_PATH"] = os.pathsep.join(
        (str(config_path.parents[1]), environment["KBE_RES_PATH"])
    )
    process = None
    with (extra_root / "stdout.log").open("wb") as stdout, \
            (extra_root / "stderr.log").open("wb") as stderr:
        try:
            process = subprocess.Popen(
                [str(args.repository_root / "kbe/bin/server" / platform_executable_name("cellapp")),
                 "--cid=8010", "--gus=20", "--hide=1"],
                cwd=extra_root, env=extra_environment, stdin=subprocess.DEVNULL,
                stdout=stdout, stderr=stderr,
                creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
            )
            exit_code = process.wait(timeout=60)
            require(exit_code in (1, 255, 4294967295),
                    f"不兼容节点没有按启动失败退出，退出码为 {exit_code}")
        finally:
            # 只清理本次启动且仍在运行的进程，不能按组件名或旧 PID 查找。
            if process is not None and process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)

    logs = wait_until(
        "不兼容节点拒绝日志",
        lambda: read_text(extra_root / "stdout.log") + read_text(extra_root / "stderr.log")
        + all_run_logs(entries, run_root)
        + "".join(read_text(path) for path in (extra_root / "logs").glob("*.log")),
        lambda text: expected_error in text, timeout=10,
    )
    require("confirmed CellApp(8010)" not in logs,
            "不兼容节点被管理器确认为可用节点")
    require("onCellappInitProgress: cid=8010" not in logs,
            "不兼容节点仍上报了初始化完成")
    require("Traceback (most recent call last)" not in logs and "KBE_ASSERT" not in logs,
            "不兼容节点因脚本异常或断言退出，不能当作模式拒绝通过")
    require(all(entry["process"].poll() is None for entry in entries),
            "拒绝新增节点时原有服务进程退出")
    result = {"status": "PASS", "rejection": args.partition_rejection,
              "space_partitioning_configuration": args.space_partitioning,
              "rejected_component": 8010, "exit_code": exit_code}
    (output / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"SPACE_PARTITIONING_REJECTION_PASS result={output / 'result.json'}", flush=True)


def demo_bot_count(args):
    if args.boundary_compare_window_ms is not None:
        return args.partition_load_bots
    if args.check_adjustment_aoi:
        # 显式传送移走一名玩家，调区的同一 Space 仍需要两名 Bot 互相观察。
        return 3
    return args.partition_load_bots + 1 if args.partition_load_bots else 2


def verify_demo_teleport(base, snapshot, initial, source, original_route, avatar_count):
    eid = source["id"]
    original_space_ids = {
        e["id"] for entities in initial.values() for e in entities if e["type"] == "Space"
    }
    # 通过原游戏的分配器建立另一 CellApp 上的 Space，不用同进程移动代替传送。
    created = base.evaluate(
        f"[e.getSpaceAllocs()[{source['utype']}].createSpace(0, {{}}) "
        "for e in tuple(KBEngine.entities.values()) if type(e).__name__ == 'Spaces']"
    )
    require(created == [None], "业务 Space 分配器不是唯一实例")
    target_state = wait_until(
        "第二个 Space 实例创建", snapshot,
        lambda state: any(
            e["type"] == "Space" and e["real"] and e["id"] not in original_space_ids
            for entities in state.values() for e in entities
        ),
    )
    target = next(
        e for entities in target_state.values() for e in entities
        if e["type"] == "Space" and e["real"] and e["id"] not in original_space_ids
    )
    require(target["cid"] != source["cid"], "新 Space 未分配到另一 CellApp，不执行同进程替代测试")
    base.evaluate(
        f"KBEngine.entities[{target['id']}].teleportSpace(KBEngine.entities[{eid}], "
        f"{tuple(source['position'])!r}, {tuple(source['direction'])!r}, {{}})"
    )

    def migrated(state):
        reals = [e for entities in state.values() for e in entities if e["id"] == eid and e["real"]]
        return len(reals) == 1 and reals[0]["cid"] == target["cid"] and reals[0]["space"] == target["space"]

    final = wait_until("角色跨 Cell 传送", snapshot, migrated)
    route = wait_until(
        "Base 更新角色路由",
        lambda: base.evaluate(
            f"{{'cell': str(KBEngine.entities[{eid}].cell), "
            f"'client': str(KBEngine.entities[{eid}].client), "
            f"'connected': KBEngine.entities[{eid}].client is not None, "
            f"'dbid': KBEngine.entities[{eid}].databaseID}}"
        ),
        lambda value: f"cellapp[{target['cid']}]" in value["cell"],
    )
    require(route["connected"] and route["client"] == original_route["client"] and
            route["dbid"] == original_route["dbid"], "传送改变了客户端连接或角色数据库 ID")
    require(
        sum(e["type"] == "Avatar" and e["real"] for entities in final.values() for e in entities) == avatar_count,
        "传送后 Real 角色数量改变",
    )
    print(f"DEMO_TELEPORT_PASS entity={eid} source={source['cid']} target={target['cid']}", flush=True)
    return final, target, route


def verify_demo(entries, run_root, environment, args, output, password):
    sys.path.insert(0, str(args.repository_root / "kbe/tools/server"))
    from pycommon import Define, Machines

    expected_server_errors = set()

    def checked_server_logs():
        logs = all_run_logs(entries, run_root)
        if not expected_server_errors:
            return logs
        # 只排除缩容负例在指定源 Cell 实测到的那一条溢出诊断。
        return "\n".join(line for line in logs.splitlines() if line.strip() not in expected_server_errors)

    token = resolve_management_admin_token(environment)
    wait_for_cluster_ready_for_login(
        args.repository_root / "kbe/tools/server", [run_root / "logs"],
        1, args.cellapp_count, 150, 5, None, token,
    )
    cell_ids = tuple(range(8001, 8001 + args.cellapp_count))
    expected_mode = "true" if args.space_partitioning == "true" else "false"
    logs = all_run_logs(entries, run_root)
    for cid in cell_ids:
        require(
            f"confirmed CellApp({cid}), enabled={expected_mode}." in logs,
            f"CellApp {cid} 未完成预期的分割模式确认",
        )
    if args.partition_rejection:
        verify_partitioning_rejection(entries, run_root, environment, args, output)
        return
    machines = Machines.Machines(adminToken=token)
    consoles = {}
    bots_processes = {}
    bots_logs = []
    bots_ids = tuple(range(10000, 10000 + args.partition_bot_processes))
    with ExitStack() as resources:
        resources.callback(machines.stopListen)
        try:
            bots_exe = args.repository_root / "kbe/bin/server" / platform_executable_name("bots")
            for index, cid in enumerate(bots_ids):
                bots_environment = build_environment(args.repository_root, args.assets_root, output)
                bots_environment["KBE_TEST_MODE"] = "1"
                prefix = "bots"
                if len(bots_ids) > 1:
                    prefix = f"bots.{cid}"
                    base_config = output / "config-overlay/res/server"
                    config = ET.parse(base_config / "kbengine.xml")
                    bot_config = config.getroot().find("./bots")
                    set_xml_value(bot_config.find("telnet_service"), "port", args.bots_console_ports[cid])
                    # 显式传送会移出一名玩家，采样人数仍与参数一致。
                    count = args.partition_load_bots // len(bots_ids) + (index == 0)
                    set_xml_value(bot_config.find("defaultAddBots"), "totalCount", count)
                    override = output / prefix / "res"
                    config_dir = override / "server"
                    config_dir.mkdir(parents=True)
                    config.write(config_dir / "kbengine.xml", encoding="utf-8", xml_declaration=True)
                    # 引擎从当前 XML 所在目录读取日志配置，不能依赖下一层资源目录。
                    log_config = Path("log4cxx_properties/bots.properties")
                    (config_dir / log_config.parent).mkdir()
                    shutil.copyfile(base_config / log_config, config_dir / log_config)
                    bots_environment["KBE_RES_PATH"] = str(override) + os.pathsep + bots_environment["KBE_RES_PATH"]
                stdout_path, stderr_path = output / f"{prefix}.stdout.log", output / f"{prefix}.stderr.log"
                bots_logs.extend((stdout_path, stderr_path))
                bots_processes[cid] = subprocess.Popen(
                    [str(bots_exe), f"--cid={cid}", f"--gus={40 + index}", "--hide=1",
                     "--publish-component", "--bots-transport=tcp", "--bots-allow-tcp-fallback=false"],
                    cwd=output, env=bots_environment,
                    stdout=resources.enter_context(stdout_path.open("wb")),
                    stderr=resources.enter_context(stderr_path.open("wb")),
                    creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
                )
            owned = {entry["spec"]["component_id"]: entry for entry in entries}

            def discover():
                machines.queryAllInterfaces("127.0.0.1", timeout=1)
                return {
                    info.componentID: {
                        "pid": info.pid, "host": info.intaddr,
                        "port": info.consolePort, "internal_port": info.intport, "type": info.componentType,
                    }
                    for infos in machines.interfaces.values()
                    for info in infos
                    if info.componentID in {*owned, *bots_ids}
                }

            infos = wait_until(
                "本次组件控制台发现", discover,
                lambda found: all(cid in found for cid in bots_ids) and all(
                    cid in found and found[cid]["port"] > 0 for cid in (7001, *cell_ids)
                ),
            )
            require(all(infos[cid]["pid"] == process.pid for cid, process in bots_processes.items()),
                    "发现结果中的 Bots 不属于本次测试")
            for cid in (7001, *cell_ids):
                expected_pid = owned[cid]["process"].pid
                require(infos[cid]["pid"] == expected_pid, "发现结果包含其他进程，停止检查")
                console = RuntimeConsole(
                    infos[cid]["host"], infos[cid]["port"], password, cid, expected_pid
                )
                resources.callback(console.close)
                consoles[cid] = console
                print(f"DEMO_CONSOLE_READY component={cid} pid={expected_pid}", flush=True)

            cells = {cid: consoles[cid] for cid in cell_ids}

            def connect_bots_console(cid=10000):
                port = args.bots_console_ports.get(cid)
                if port is None:
                    marker = wait_until(
                        "Bots 控制台启动",
                        lambda: re.search(r"TelnetServer server is running on port (\d+)", read_bots_log()),
                        bool,
                    )
                    port = int(marker.group(1))
                console = RuntimeConsole("127.0.0.1", port, password, cid, bots_processes[cid].pid)
                resources.callback(console.close)
                return console

            def read_bots_log():
                return read_text(output / "logs/bots.10000.log")

            def snapshot():
                return {
                    cid: [dict(entity, cid=cid) for entity in cell_entities(console)]
                    for cid, console in cells.items()
                }

            avatar_count = demo_bot_count(args)
            initial = wait_until(
                "本轮角色进入场景", snapshot,
                lambda state: sum(e["type"] == "Avatar" and e["real"]
                                  for entities in state.values() for e in entities) == avatar_count,
                timeout=90,
            )
            adjustment_bots = None
            if args.check_adjustment_aoi:
                adjustment_bots = connect_bots_console()
                stop_demo_bot_actions(adjustment_bots)
            load_bots_consoles = []
            if args.partition_load_bots:
                for cid in bots_ids:
                    console = connect_bots_console(cid)
                    stop_demo_bot_actions(console)
                    load_bots_consoles.append(console)
            avatars = [e for entities in initial.values() for e in entities
                       if e["type"] == "Avatar" and e["real"]]
            source = avatars[0]
            eid = source["id"]
            original_route = consoles[7001].evaluate(
                f"{{'client': str(KBEngine.entities[{eid}].client), "
                f"'connected': KBEngine.entities[{eid}].client is not None, "
                f"'dbid': KBEngine.entities[{eid}].databaseID}}"
            )
            require(original_route["connected"] and original_route["dbid"] > 0,
                    "传送前角色缺少客户端连接或有效数据库 ID")
            if args.boundary_compare_window_ms is None:
                final, target, route = verify_demo_teleport(
                    consoles[7001], snapshot, initial, source, original_route, avatar_count,
                )
            else:
                # 不引入第 97 个客户端，也不提前通过显式传送复用跨 Cell 连接。
                final, target, route = initial, None, None
            layout_result = None
            verify_after_logout = None
            load_result = None
            load_passed = True
            adjustment_passed = True
            if args.partition_load_bots:
                players = {row["id"] for rows in final.values() for row in rows
                           if row["type"] == "Avatar" and row["real"] and row["space"] == source["space"]}
                require(len(players) == args.partition_load_bots, "性能场景的玩家未进入同一 Space")
                processes = {f"{entry['spec']['name']}#{cid}": entry["process"].pid
                             for cid, entry in owned.items()}
                processes.update({f"bots#{cid}": process.pid for cid, process in bots_processes.items()})
                processes["controller"] = os.getpid()
                if args.boundary_compare_window_ms is not None:
                    load_result, load_passed = verify_boundary_compare(
                        cells, consoles[7001], load_bots_consoles[0], source, players,
                        args.boundary_compare_window_ms, output,
                    )
                else:
                    load_result, load_passed = verify_partition_load(
                        cells, consoles[7001], load_bots_consoles, infos, processes, source, players, args, token, output,
                        base_info=infos[7001],
                    )
            elif args.fixed_space_layout:
                original_space = next(
                    entity for entity in initial[source["cid"]]
                    if entity["type"] == "Space" and entity["space"] == source["space"]
                )

                def rejection_logged(cid, operation, space, version, revision, key):
                    message = (f"Cellapp::spaceLayout: {operation} rejected, space={space}, "
                               f"version={version}, revision={revision}, key={key}: ")
                    # 服务启动后日志由 Logger 集中写入，按日志头中的组件编号区分提交方。
                    for line in read_text(run_root / "logs/logger_cellapp.log").splitlines():
                        header, _, body = line.partition(" - ")
                        fields = header.split()
                        if body.startswith(message) and len(fields) >= 4 and fields[3] == str(cid):
                            return True
                    return False

                fixed_cells = cells
                if args.split_space:
                    owner = original_space["cid"]
                    peer = next(cid for cid in cells if cid != owner)
                    fixed_cells = {cid: cells[cid] for cid in (owner, peer)}
                layout_result, verify_after_logout = verify_fixed_layout(
                    fixed_cells, original_space, args.space_partitioning == "true", rejection_logged
                )
                if args.split_space:
                    manager = infos[6000]
                    require(manager["pid"] == owned[6000]["process"].pid,
                            "发现的 CellAppMgr 不属于本次测试")
                    live_checks = {}
                    if adjustment_bots is not None:
                        live_checks["live"] = LiveSpaceAdjustment(
                            cells, consoles[7001], adjustment_bots, original_space["space"],
                        )
                    layout_result["split"] = verify_space_split(
                        cells, layout_result, consoles[7001], args.repository_root,
                        f"{manager['host']}:{manager['internal_port']}", environment, output,
                        **live_checks,
                    )
                    if args.merge_space:
                        layout_result["merge"] = verify_space_merge(
                            cells, layout_result, layout_result["split"], consoles[7001], args.repository_root,
                            f"{manager['host']}:{manager['internal_port']}", environment, output,
                            expected_server_errors=expected_server_errors, **live_checks,
                        )
                        if live_checks:
                            adjustment_passed = layout_result["merge"]["live_adjustment"]["mutual_aoi_objects_preserved"]
                    verify_after_logout = None
                elif args.handoff_destroy_timer:
                    layout_result["handoff_destroy_timer"] = verify_handoff_destroy_timer(
                        cells, layout_result, output
                    )
                elif layout_result["enabled"]:
                    bots_console = connect_bots_console()
                    if args.move_space_boundary:
                        layout_result["boundary_move"] = verify_boundary_move(
                            cells, layout_result, consoles[7001], bots_console, output
                        )
                    else:
                        layout_result["boundary_ghosts"] = verify_boundary_ghosts(
                            cells, layout_result, bots_console, read_bots_log,
                            lambda: read_text(run_root / "logs/logger_cellapp.log"),
                            args.coordinate_has_y,
                        )
                        if len(cells) == 4:
                            layout_result["ghost_source_change"] = verify_ghost_source_change(
                                cells, layout_result, bots_console, output, args.coordinate_has_y,
                            )
                    mapping_log = (
                        f"ClientObjectBase::addSpaceGeometryMapping: spaceID={original_space['space']}, "
                        f"respath={NO_NAVIGATION_MAPPING}!"
                    )
                    wait_until(
                        "Bots 收到无服务端导航的场景映射",
                        lambda: mapping_log in read_text(output / "logs/bots.10000.log"),
                        bool,
                    )
                    layout_result["client_received_mapping"] = True

            # 使用真实管理请求让 Bots 断开连接，不能用终止进程代替正常退出验证。
            current_infos = discover()
            for cid, process in bots_processes.items():
                require(process.poll() is None and current_infos.get(cid, {}).get("pid") == process.pid,
                        "退出前 Bots 进程身份已改变")
                machines.stopServer(Define.BOTS_TYPE, cid, targetIP="127.0.0.1")
            for cid, process in bots_processes.items():
                bots_exit = process.wait(timeout=30)
                require(bots_exit == 0, f"Bots {cid} 没有正常退出，退出码为 {bots_exit}")
            for cid in (7001, *cell_ids):
                require(owned[cid]["process"].poll() is None, "客户端退出时服务端已结束")
            wait_until(
                "Cell 清理退出角色", snapshot,
                lambda state: not any(e["type"] == "Avatar"
                                      for entities in state.values() for e in entities),
            )
            wait_until(
                "Base 清理退出角色",
                lambda: consoles[7001].evaluate(
                    "[e.id for e in KBEngine.entities.values() "
                    "if type(e).__name__ in ('Avatar', 'Account')]"
                ),
                lambda ids: not ids,
            )
            print("DEMO_LOGOUT_PASS avatars=0 accounts=0", flush=True)
            if verify_after_logout is not None:
                layout_result.update(verify_after_logout())
            if layout_result is not None and not args.keep_space_layout_on_shutdown:
                verify_layout_removal(cells, layout_result, consoles[7001], target, output)
                if layout_result["enabled"]:
                    verify_rejected_layout_removal(cells, target)
                    layout_result["rejected_layout_removed"] = True
            elif layout_result is not None:
                layout_result["active_at_shutdown"] = True
            require(not ERROR_PATTERN.search(checked_server_logs()),
                    "业务检查产生错误，请检查本次日志")

            for console in consoles.values():
                console.close()
            consoles.clear()
            # 整服关闭需要先通知全部核心组件，避免仍在运行的管理器把对端退出当作故障。
            # BaseApp 与 DBMgr 保留原有等待条件，所有请求发出后再等待进程结束。
            # 三个辅助组件与现有集群工具一致，由测试控制器按持有的进程对象清理。
            for group in SHUTDOWN_GROUPS:
                group_entries = [entry for name in group for entry in entries
                                 if entry["spec"]["name"] == name]
                current_infos = discover()
                for entry in group_entries:
                    name = entry["spec"]["name"]
                    require(entry["process"].poll() is None,
                            f"{name} 在收到退出请求前已经结束")
                    info = current_infos.get(entry["spec"]["component_id"])
                    require(info is not None and info["pid"] == entry["process"].pid,
                            f"{name} 缺少本次进程的发现结果")
                    machines.stopServer(
                        info["type"], entry["spec"]["component_id"], targetIP="127.0.0.1"
                    )
                for entry in sorted(group_entries, key=lambda item: item["spec"]["name"] == "baseapp"):
                    name = entry["spec"]["name"]
                    exit_code = entry["process"].wait(timeout=60)
                    require(exit_code == 0, f"{name} 没有正常退出，退出码为 {exit_code}")
                    print(f"DEMO_COMPONENT_EXIT name={name} code={exit_code}", flush=True)
            logs = checked_server_logs()
            logs += "".join(read_text(path) for path in bots_logs)
            logs += "".join(read_text(path) for path in (output / "logs").glob("*.log"))
            require(not ERROR_PATTERN.search(logs) and not DEFAULT_PATTERNS["error"].search(logs),
                    "运行或退出过程中产生错误，请检查本次日志")
            require(
                all(entry["process"].poll() is None for entry in entries
                    if entry["spec"]["name"] in INFRASTRUCTURE_COMPONENTS),
                "辅助组件在测试控制器清理前已经退出",
            )
            result = {
                "status": "PASS" if load_passed and adjustment_passed else "FAIL",
                "entity": eid, "source_cellapp": source["cid"],
                "space_partitioning_configuration": args.space_partitioning,
                "space_partitioning_enabled": args.space_partitioning == "true",
                "avatars_after_logout": 0,
                "normal_server_exits": len(entries) - len(INFRASTRUCTURE_COMPONENTS),
                "infrastructure_cleanup": list(INFRASTRUCTURE_COMPONENTS),
                "fixed_space_layout": layout_result,
            }
            if target is not None:
                result.update(
                    target_cellapp=target["cid"], target_space=target["space"],
                    client_preserved=route["client"] == original_route["client"],
                    database_id_preserved=route["dbid"] == original_route["dbid"],
                )
            if load_result is not None:
                result["partition_load"] = load_result
            (output / "result.json").write_text(
                json.dumps(result, indent=2) + "\n", encoding="utf-8"
            )
            if not load_passed:
                raise RuntimeError(f"客户端玩家 AOI 或 TCP 队列检查未通过，详见 {output / load_result}")
            if not adjustment_passed:
                raise RuntimeError(f"调区后客户端重建了仍应可见的玩家对象，详见 {output / 'result.json'}")
            print(f"DEMO_ASSETS_RUNTIME_PASS result={output / 'result.json'}", flush=True)
        finally:
            # 失败时仍只处理本次持有的子进程；清理不计入正常退出的通过结果。
            for process in bots_processes.values():
                if process.poll() is not None:
                    continue
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository-root", type=Path, required=True)
    parser.add_argument("--assets-root", type=Path, required=True)
    parser.add_argument("--binary-root", type=Path, required=True)
    parser.add_argument("--config-overlay", type=Path, required=True)
    parser.add_argument("--test-database-name", required=True,
                        help="必须与运行配置一致，且不同于原 assets 的数据库名")
    parser.add_argument("--space-partitioning", choices=("default", "false", "true"), default="default",
                        help="仅修改隔离运行配置；default 检查项目未填写新配置时的行为")
    parser.add_argument("--partition-rejection", choices=("mode", "coordinates", "axes", "invalid"),
                        help="启动真实额外 CellApp 检查拒绝行为，不运行正常业务流程")
    parser.add_argument("--fixed-space-layout", action="store_true",
                        help="验证固定区域、资源同步、空区域保留和逻辑 Space 删除")
    parser.add_argument("--coordinate-y", choices=("true", "false"),
                        help="覆盖本轮 CellApp 的 AOI 坐标轴配置")
    parser.add_argument("--handoff-destroy-timer", action="store_true",
                        help="验证一次性定时器在自动交接期间请求销毁")
    parser.add_argument("--move-space-boundary", action="store_true",
                        help="验证双 Cell 边界移动、静止实体迁移和在途交接")
    parser.add_argument("--split-space", action="store_true",
                        help="验证运维新增 Cell、准备期间的资源更新和实体迁移")
    parser.add_argument("--merge-space", action="store_true",
                        help="扩容后验证合并、排空和再次扩容")
    parser.add_argument("--check-adjustment-aoi", action="store_true",
                        help="新增、调界和合并时验证两个在线玩家的 AOI、Real 和路由")
    parser.add_argument("--partition-load-bots", type=int, default=0,
                        help="使用指定数量的真实 TCP Bots 采样均匀和热点分布")
    parser.add_argument("--partition-bot-processes", type=int, choices=(1, 2, 4), default=1,
                        help="压测客户端分配到几个 Bots 进程")
    parser.add_argument("--partition-load-seconds", type=int, default=30,
                        help="每种分布预热结束后的采样秒数")
    parser.add_argument("--partition-crossings", type=int, default=0,
                        help="采样后重复跨越四区交汇处的垂直切分线")
    parser.add_argument("--boundary-compare-window-ms", type=int, choices=(0, 1000),
                        help="96 个真实 TCP 客户端、两 Cell 的旁观者位置延迟对照")
    parser.add_argument("--cellapp-count", type=int, choices=(2, 4), default=2)
    parser.add_argument("--keep-space-layout-on-shutdown", action="store_true",
                        help="保留已启用的区域直接正常关服，检查 Cell 资源不会阻止退出")
    args = parser.parse_args()
    if args.boundary_compare_window_ms is not None:
        require(args.partition_load_bots == 96 and args.cellapp_count == 2 and args.coordinate_y == "false"
                and args.partition_bot_processes == 1 and args.space_partitioning == "true"
                and not (args.fixed_space_layout or args.partition_rejection or args.partition_crossings),
                "跨引擎对照需要 96 个 Bots、单 Bots 进程、两 Cell 和二维分割")
    else:
        require(args.partition_load_bots == 0 or (
            4 <= args.partition_load_bots <= 128 and args.partition_load_bots % 4 == 0
            and args.cellapp_count == 4 and args.space_partitioning in ("true", "false")
            and args.partition_load_seconds >= 10 and not (args.fixed_space_layout or args.partition_rejection)
        ), "性能采样需要四个 CellApp、明确的分割模式、四的倍数个玩家（4 至 128）及至少 10 秒的采样")
    require(args.partition_crossings == 0 or (
        args.partition_load_bots and args.space_partitioning == "true" and args.partition_crossings > 0
    ), "连续越界需要在启用分割的性能场景中执行")
    require(args.partition_bot_processes == 1 or args.partition_load_bots > 0,
            "多个 Bots 进程仅用于性能场景")
    require(not args.keep_space_layout_on_shutdown or (
        args.fixed_space_layout and args.space_partitioning == "true"
    ), "保留布局关服检查要求启用固定空间分区")
    require(not args.handoff_destroy_timer or (
        args.fixed_space_layout and args.space_partitioning == "true" and args.cellapp_count == 2
        and not args.partition_rejection
    ), "销毁定时器检查要求启用双 Cell 固定空间分区")
    require(not args.move_space_boundary or (
        args.fixed_space_layout and args.space_partitioning == "true" and args.cellapp_count == 2
        and not args.partition_rejection and not args.handoff_destroy_timer
    ), "调界检查要求单独启用双 Cell 固定空间分区")
    require(not args.split_space or (
        args.fixed_space_layout and args.space_partitioning == "true" and args.cellapp_count == 4
        and not (args.partition_rejection or args.move_space_boundary or args.handoff_destroy_timer)
    ), "扩容检查要求启动四个 CellApp，先启用双 Cell 固定布局")
    require(not args.merge_space or args.split_space, "缩容检查要求同时开启扩容检查")
    require(not args.check_adjustment_aoi or args.merge_space, "调区 AOI 检查要求同时开启缩容检查")
    for name in ("repository_root", "assets_root", "binary_root", "config_overlay"):
        setattr(args, name, getattr(args, name).resolve())
    source = ET.parse(args.config_overlay / "server/kbengine.xml").getroot()
    database = source.find("./dbmgr/databaseInterfaces/default")
    original_database = ET.parse(find_server_config(args.assets_root)).getroot().findtext(
        "./dbmgr/databaseInterfaces/default/databaseName", ""
    ).strip()
    require(database is not None, "运行配置缺少默认数据库")
    database_name = validate_test_database(database, original_database, args.test_database_name)
    output = args.binary_root / "Testing" / f"demo-assets-runtime-{uuid.uuid4().hex[:12]}"
    output.mkdir(parents=True, exist_ok=False)
    config_path = create_config_overlay(
        args.assets_root, output, demo_bot_count(args),
        database_host=database.findtext("host", "").strip(),
        database_port=int(database.findtext("port", "3306")),
        database_name=database_name,
        database_username=database.findtext("./auth/username", "").strip(),
        database_password=database.findtext("./auth/password", "").strip(),
        runtime_log_level="warn" if args.partition_load_bots else "info", server_runtime_log_level="info",
        performance_probes_enabled=bool(args.partition_load_bots and args.boundary_compare_window_ms is None),
        bots_transport="tcp", bots_allow_tcp_fallback=False,
    )
    runtime = ET.parse(config_path)
    if args.boundary_compare_window_ms is not None:
        set_xml_value(runtime.getroot(), "asyncioRepeatOffset", 0.01)
    if args.check_adjustment_aoi or args.coordinate_y is not None:
        cell = runtime.getroot().find("cellapp")
        if cell is None:
            cell = ET.SubElement(runtime.getroot(), "cellapp")
    if args.check_adjustment_aoi:
        set_xml_value(cell, "ghostingMaxPerCheck", 4)
        # 缩小测试连接的内核缓冲，让暂停读取的观察 Cell 确实在发送方产生用户态积压。
        channel_common = runtime.getroot().find("channelCommon")
        if channel_common is None:
            channel_common = ET.SubElement(runtime.getroot(), "channelCommon")
        for key in ("readBufferSize", "writeBufferSize"):
            buffers = channel_common.find(key)
            if buffers is None:
                buffers = ET.SubElement(channel_common, key)
            set_xml_value(buffers, "internal", 256 * 1024)
    if args.coordinate_y is not None:
        coordinates = cell.find("coordinate_system")
        if coordinates is None:
            coordinates = ET.SubElement(cell, "coordinate_system")
        set_xml_value(coordinates, "rangemgr_y", args.coordinate_y)
    axes_path = "./cellapp/coordinate_system/rangemgr_y"
    axes = runtime.getroot().findtext(axes_path)
    if axes is None:
        defaults = ET.parse(args.repository_root / "kbe/res/server/kbengine_defaults.xml")
        axes = defaults.getroot().findtext(axes_path)
    args.coordinate_has_y = axes.strip().lower() == "true"
    if (args.fixed_space_layout and args.space_partitioning == "true" and args.cellapp_count == 4
            and not (args.coordinate_has_y or args.split_space or args.partition_rejection)):
        # 暂停脚本会耗尽 completion 的时间预算；本用例保留数量上限，
        # 但允许恢复后在 Witness Tick 前消费一起排队的快照和增量。
        channel_common = runtime.getroot().find("channelCommon")
        if channel_common is None:
            channel_common = ET.SubElement(runtime.getroot(), "channelCommon")
        completion_budget = channel_common.find("completionBudget")
        if completion_budget is None:
            completion_budget = ET.SubElement(channel_common, "completionBudget")
        set_xml_value(completion_budget, "maxProcessingTimeMS", 0)
    if args.split_space or args.partition_load_bots:
        management = runtime.getroot().find("management")
        if management is None:
            management = ET.SubElement(runtime.getroot(), "management")
        set_xml_value(management, "adminToken", uuid.uuid4().hex + uuid.uuid4().hex)
    manager = runtime.getroot().find("cellappmgr")
    if manager is None:
        manager = ET.SubElement(runtime.getroot(), "cellappmgr")
    partitioning = manager.find("spacePartitioning")
    if args.space_partitioning == "default":
        if partitioning is not None:
            manager.remove(partitioning)
    else:
        if partitioning is None:
            partitioning = ET.SubElement(manager, "spacePartitioning")
        set_xml_value(partitioning, "enabled", args.space_partitioning)
        if (
            args.handoff_destroy_timer or args.move_space_boundary or args.split_space
            or (args.fixed_space_layout and args.cellapp_count == 4)
            or (args.partition_load_bots and args.space_partitioning == "true")
        ):
            set_xml_value(partitioning, "autoHandoff", "true")
            set_xml_value(partitioning, "handoffMargin", "2")
    password = uuid.uuid4().hex
    # 压测过滤实体创建日志，直接使用本轮配置的 Bots 控制台端口。
    args.bots_console_ports = {}
    if args.partition_load_bots:
        for cid in range(10000, 10000 + args.partition_bot_processes):
            port = select_tcp_port()
            while port in args.bots_console_ports.values():
                port = select_tcp_port()
            args.bots_console_ports[cid] = port
    for component in ("baseapp", "cellapp", "bots"):
        parent = runtime.getroot().find(component)
        if parent is None:
            parent = ET.SubElement(runtime.getroot(), component)
        telnet = parent.find("telnet_service")
        if telnet is None:
            telnet = ET.SubElement(parent, "telnet_service")
        set_xml_value(telnet, "password", password)
        set_xml_value(telnet, "default_layer", "python")
        if component == "bots" and args.bots_console_ports:
            set_xml_value(telnet, "port", args.bots_console_ports[10000])
    runtime.write(config_path, encoding="utf-8", xml_declaration=True)
    if args.fixed_space_layout and args.space_partitioning == "true":
        # 只在这次运行的资源目录中增加无导航映射，不改原 assets。
        (config_path.parents[1] / NO_NAVIGATION_MAPPING).mkdir()
    components, _ = build_local_cluster_components(
        args.repository_root / "kbe/bin/server", 1, args.cellapp_count
    )
    cluster_args = argparse.Namespace(
        repository_root=args.repository_root, assets_root=args.assets_root,
        config_overlay=config_path.parents[1], resource_overlay=[],
        binary_root=args.binary_root, run_root=output / "server",
        components=components, expected_component_count=8 + args.cellapp_count,
        database_type=None, external_readiness=False,
        startup_timeout_seconds=150, hold_until_file=None,
    )
    print(f"DEMO_ASSETS_RUNTIME_START output={output}", flush=True)
    run_cluster(
        cluster_args,
        on_ready=lambda entries, root, env: verify_demo(
            entries, root, env, args, output, password
        ),
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        print(f"DEMO_ASSETS_RUNTIME_FAIL error={exc}", file=sys.stderr, flush=True)
        sys.exit(1)
