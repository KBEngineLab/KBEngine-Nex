"""验证 demo assets 的登录、跨 Cell 显式传送、客户端退出和服务端正常退出。"""

from __future__ import annotations

import argparse
from contextlib import ExitStack
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import uuid
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from performance.assets import build_environment, create_config_overlay, find_server_config, set_xml_value
from performance.log_metrics import DEFAULT_PATTERNS
from performance.run import resolve_management_admin_token, wait_for_cluster_ready_for_login
from performance.topology import build_local_cluster_components, platform_executable_name
from runtime_console import RuntimeConsole
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


def verify_demo(entries, run_root, environment, args, output, password):
    sys.path.insert(0, str(args.repository_root / "kbe/tools/server"))
    from pycommon import Define, Machines

    token = resolve_management_admin_token(environment)
    wait_for_cluster_ready_for_login(
        args.repository_root / "kbe/tools/server", [run_root / "logs"],
        1, 2, 150, 5, None, token,
    )
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
                        "port": info.consolePort, "type": info.componentType,
                    }
                    for infos in machines.interfaces.values()
                    for info in infos
                    if info.componentID in {*owned, 10000}
                }

            infos = wait_until(
                "本次组件控制台发现", discover,
                lambda found: 10000 in found and all(
                    cid in found and found[cid]["port"] > 0 for cid in (7001, 8001, 8002)
                ),
            )
            require(infos[10000]["pid"] == bots.pid, "发现结果中的 Bots 不属于本次测试")
            for cid in (7001, 8001, 8002):
                expected_pid = owned[cid]["process"].pid
                require(infos[cid]["pid"] == expected_pid, "发现结果包含其他进程，停止检查")
                console = RuntimeConsole(
                    infos[cid]["host"], infos[cid]["port"], password, cid, expected_pid
                )
                resources.callback(console.close)
                consoles[cid] = console
                print(f"DEMO_CONSOLE_READY component={cid} pid={expected_pid}", flush=True)

            cells = {cid: consoles[cid] for cid in (8001, 8002)}

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

            # 使用真实管理请求让 Bots 断开连接，不能用终止进程代替正常退出验证。
            require(bots.poll() is None and discover()[10000]["pid"] == bots.pid,
                    "退出前 Bots 进程身份已改变")
            machines.stopServer(Define.BOTS_TYPE, 10000, targetIP="127.0.0.1")
            bots_exit = bots.wait(timeout=30)
            require(bots_exit == 0, f"Bots 没有正常退出，退出码为 {bots_exit}")
            for cid in (7001, 8001, 8002):
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
                "target_cellapp": target["cid"], "target_space": target["space"],
                "client_preserved": route["client"] == original_route["client"],
                "database_id_preserved": route["dbid"] == original_route["dbid"],
                "avatars_after_logout": 0,
                "normal_server_exits": len(entries) - len(INFRASTRUCTURE_COMPONENTS),
                "infrastructure_cleanup": list(INFRASTRUCTURE_COMPONENTS),
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
    args = parser.parse_args()
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
    components, _ = build_local_cluster_components(args.repository_root / "kbe/bin/server", 1, 2)
    cluster_args = argparse.Namespace(
        repository_root=args.repository_root, assets_root=args.assets_root,
        config_overlay=config_path.parents[1], resource_overlay=[],
        binary_root=args.binary_root, run_root=output / "server",
        components=components, expected_component_count=10,
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
