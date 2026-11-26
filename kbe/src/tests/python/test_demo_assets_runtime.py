"""验证 demo assets 的登录、跨 Cell 显式传送、客户端退出和服务端正常退出。"""

from __future__ import annotations

import argparse
from contextlib import ExitStack
import json
import os
from pathlib import Path
import re
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
from space_split_runtime import verify_space_merge, verify_space_split
from space_layout_runtime import (
    NO_NAVIGATION_MAPPING, verify_boundary_ghosts, verify_fixed_layout,
    verify_layout_removal, verify_rejected_layout_removal,
)
from test_server_cluster import ERROR_PATTERN, all_run_logs, read_text, run_cluster


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


def verify_demo(entries, run_root, environment, args, output, password):
    sys.path.insert(0, str(args.repository_root / "kbe/tools/server"))
    from pycommon import Define, Machines

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
    bots = None
    with ExitStack() as resources:
        resources.callback(machines.stopListen)
        try:
            bots_environment = build_environment(args.repository_root, args.assets_root, output)
            bots_environment["KBE_TEST_MODE"] = "1"
            bots_exe = args.repository_root / "kbe/bin/server" / platform_executable_name("bots")
            stdout = resources.enter_context((output / "bots.stdout.log").open("wb"))
            stderr = resources.enter_context((output / "bots.stderr.log").open("wb"))
            bots = subprocess.Popen(
                [str(bots_exe), "--cid=10000", "--gus=40", "--hide=1",
                 "--publish-component", "--bots-transport=tcp", "--bots-allow-tcp-fallback=false"],
                cwd=output, env=bots_environment, stdout=stdout, stderr=stderr,
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
                    if info.componentID in {*owned, 10000}
                }

            infos = wait_until(
                "本次组件控制台发现", discover,
                lambda found: 10000 in found and all(
                    cid in found and found[cid]["port"] > 0 for cid in (7001, *cell_ids)
                ),
            )
            require(infos[10000]["pid"] == bots.pid, "发现结果中的 Bots 不属于本次测试")
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

            def snapshot():
                return {
                    cid: [dict(entity, cid=cid) for entity in cell_entities(console)]
                    for cid, console in cells.items()
                }

            initial = wait_until(
                "两个角色进入场景", snapshot,
                lambda state: sum(e["type"] == "Avatar" and e["real"]
                                  for entities in state.values() for e in entities) == 2,
                timeout=90,
            )
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
            original_space_ids = {
                e["id"] for entities in initial.values() for e in entities if e["type"] == "Space"
            }
            # 指定 assets 只配置一个地图；通过其现有分配器建立第二个真实实例，
            # 不替换业务脚本，也不把一个 Space 内的本地移动当作跨 Cell 传送。
            created = consoles[7001].evaluate(
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
            require(target["cid"] != source["cid"],
                    "新 Space 未分配到另一 CellApp，不执行同进程替代测试")
            consoles[7001].evaluate(
                f"KBEngine.entities[{target['id']}].teleportSpace(KBEngine.entities[{eid}], "
                f"{tuple(source['position'])!r}, {tuple(source['direction'])!r}, {{}})"
            )

            def migrated(state):
                reals = [e for entities in state.values() for e in entities
                         if e["id"] == eid and e["real"]]
                return len(reals) == 1 and reals[0]["cid"] == target["cid"] and \
                    reals[0]["space"] == target["space"]

            final = wait_until("角色跨 Cell 传送", snapshot, migrated)
            route = wait_until(
                "Base 更新角色路由",
                lambda: consoles[7001].evaluate(
                    f"{{'cell': str(KBEngine.entities[{eid}].cell), "
                    f"'client': str(KBEngine.entities[{eid}].client), "
                    f"'connected': KBEngine.entities[{eid}].client is not None, "
                    f"'dbid': KBEngine.entities[{eid}].databaseID}}"
                ),
                lambda value: f"cellapp[{target['cid']}]" in value["cell"],
            )
            require(route["connected"] and route["client"] == original_route["client"] and
                    route["dbid"] == original_route["dbid"],
                    "传送改变了客户端连接或角色数据库 ID")
            require(
                sum(e["type"] == "Avatar" and e["real"]
                    for entities in final.values() for e in entities) == 2,
                "传送后 Real 角色数量不等于两个",
            )
            print(f"DEMO_TELEPORT_PASS entity={eid} source={source['cid']} target={target['cid']}",
                  flush=True)
            layout_result = None
            verify_after_logout = None
            if args.fixed_space_layout:
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
                    layout_result["split"] = verify_space_split(
                        cells, layout_result, consoles[7001], args.repository_root,
                        f"{manager['host']}:{manager['internal_port']}", environment, output,
                    )
                    if args.merge_space:
                        layout_result["merge"] = verify_space_merge(
                            cells, layout_result, layout_result["split"], consoles[7001], args.repository_root,
                            f"{manager['host']}:{manager['internal_port']}", environment, output,
                        )
                    verify_after_logout = None
                elif args.handoff_destroy_timer:
                    layout_result["handoff_destroy_timer"] = verify_handoff_destroy_timer(
                        cells, layout_result, output
                    )
                elif layout_result["enabled"]:
                    def read_bots_log():
                        return read_text(output / "logs/bots.10000.log")

                    port = wait_until(
                        "Bots 控制台启动",
                        lambda: re.search(r"TelnetServer server is running on port (\d+)", read_bots_log()),
                        bool,
                    )
                    bots_console = RuntimeConsole("127.0.0.1", int(port.group(1)), password, 10000, bots.pid)
                    resources.callback(bots_console.close)
                    if args.move_space_boundary:
                        layout_result["boundary_move"] = verify_boundary_move(
                            cells, layout_result, consoles[7001], bots_console, output
                        )
                    else:
                        layout_result["boundary_ghosts"] = verify_boundary_ghosts(
                            cells, layout_result, bots_console, read_bots_log,
                            lambda: read_text(run_root / "logs/logger_cellapp.log"),
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
            require(bots.poll() is None and discover()[10000]["pid"] == bots.pid,
                    "退出前 Bots 进程身份已改变")
            machines.stopServer(Define.BOTS_TYPE, 10000, targetIP="127.0.0.1")
            bots_exit = bots.wait(timeout=30)
            require(bots_exit == 0, f"Bots 没有正常退出，退出码为 {bots_exit}")
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
            require(not ERROR_PATTERN.search(all_run_logs(entries, run_root)),
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
            logs = all_run_logs(entries, run_root)
            logs += read_text(output / "bots.stdout.log") + read_text(output / "bots.stderr.log")
            logs += "".join(read_text(path) for path in (output / "logs").glob("*.log"))
            require(not ERROR_PATTERN.search(logs) and not DEFAULT_PATTERNS["error"].search(logs),
                    "运行或退出过程中产生错误，请检查本次日志")
            require(
                all(entry["process"].poll() is None for entry in entries
                    if entry["spec"]["name"] in INFRASTRUCTURE_COMPONENTS),
                "辅助组件在测试控制器清理前已经退出",
            )
            result = {
                "status": "PASS", "entity": eid, "source_cellapp": source["cid"],
                "space_partitioning_configuration": args.space_partitioning,
                "space_partitioning_enabled": args.space_partitioning == "true",
                "target_cellapp": target["cid"], "target_space": target["space"],
                "client_preserved": route["client"] == original_route["client"],
                "database_id_preserved": route["dbid"] == original_route["dbid"],
                "avatars_after_logout": 0,
                "normal_server_exits": len(entries) - len(INFRASTRUCTURE_COMPONENTS),
                "infrastructure_cleanup": list(INFRASTRUCTURE_COMPONENTS),
                "fixed_space_layout": layout_result,
            }
            (output / "result.json").write_text(
                json.dumps(result, indent=2) + "\n", encoding="utf-8"
            )
            print(f"DEMO_ASSETS_RUNTIME_PASS result={output / 'result.json'}", flush=True)
        finally:
            # 失败时仍只处理本次持有的子进程；清理不计入正常退出的通过结果。
            if bots is not None and bots.poll() is None:
                bots.terminate()
                try:
                    bots.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    bots.kill()
                    bots.wait(timeout=5)


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
    parser.add_argument("--handoff-destroy-timer", action="store_true",
                        help="验证一次性定时器在自动交接期间请求销毁")
    parser.add_argument("--move-space-boundary", action="store_true",
                        help="验证双 Cell 边界移动、静止实体迁移和在途交接")
    parser.add_argument("--split-space", action="store_true",
                        help="验证运维新增 Cell、准备期间的资源更新和实体迁移")
    parser.add_argument("--merge-space", action="store_true",
                        help="扩容后验证合并、排空和再次扩容")
    parser.add_argument("--cellapp-count", type=int, choices=(2, 4), default=2)
    parser.add_argument("--keep-space-layout-on-shutdown", action="store_true",
                        help="保留已启用的区域直接正常关服，检查 Cell 资源不会阻止退出")
    args = parser.parse_args()
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
        args.assets_root, output, 2,
        database_host=database.findtext("host", "").strip(),
        database_port=int(database.findtext("port", "3306")),
        database_name=database_name,
        database_username=database.findtext("./auth/username", "").strip(),
        database_password=database.findtext("./auth/password", "").strip(),
        runtime_log_level="info", server_runtime_log_level="info",
        bots_transport="tcp", bots_allow_tcp_fallback=False,
    )
    runtime = ET.parse(config_path)
    if args.split_space:
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
        if args.handoff_destroy_timer or args.move_space_boundary or args.split_space:
            set_xml_value(partitioning, "autoHandoff", "true")
            set_xml_value(partitioning, "handoffMargin", "2")
    password = uuid.uuid4().hex
    for component in ("baseapp", "cellapp", "bots"):
        parent = runtime.getroot().find(component)
        if parent is None:
            parent = ET.SubElement(runtime.getroot(), component)
        telnet = parent.find("telnet_service")
        if telnet is None:
            telnet = ET.SubElement(parent, "telnet_service")
        set_xml_value(telnet, "password", password)
        set_xml_value(telnet, "default_layer", "python")
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
