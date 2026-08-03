#!/usr/bin/env python3

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys
import time
import xml.etree.ElementTree as ET


COMPONENT_COUNT = 9
ERROR_PATTERN = re.compile(
    r"\[ERROR\]|\[FATAL\]|^\s*(?:ERROR|FATAL)\s|"
    r"Traceback \(most recent call last\)|Unhandled exception",
    re.IGNORECASE | re.MULTILINE,
)


def parse_arguments():
    parser = argparse.ArgumentParser(description="Run an isolated KBEngine server cluster smoke test.")
    parser.add_argument("--components", required=True)
    parser.add_argument("--expected-component-count", type=int, default=COMPONENT_COUNT)
    parser.add_argument("--repository-root", type=Path, required=True)
    parser.add_argument("--assets-root", type=Path, required=True)
    parser.add_argument("--config-overlay", type=Path)
    parser.add_argument(
        "--resource-overlay",
        action="append",
        default=[],
        type=Path,
        help="Prepend a read-only resource root before the game assets",
    )
    parser.add_argument("--binary-root", type=Path, required=True)
    parser.add_argument("--run-root", type=Path, required=True)
    parser.add_argument("--database-type", choices=("mysql", "mongodb", "postgresql"))
    parser.add_argument("--database-host", default="127.0.0.1")
    parser.add_argument("--database-port", type=int)
    parser.add_argument("--database-name")
    parser.add_argument("--database-username")
    parser.add_argument("--database-password", default="")
    parser.add_argument("--startup-timeout-seconds", type=int, default=90)
    parser.add_argument(
        "--external-readiness",
        action="store_true",
        help="Publish live child processes and delegate semantic readiness to an external controller",
    )
    parser.add_argument(
        "--hold-until-file",
        type=Path,
        help="Keep the ready cluster alive until this file exists; used by external test controllers",
    )
    args = parser.parse_args()

    if not 10 <= args.startup_timeout_seconds <= 150:
        parser.error("--startup-timeout-seconds must be between 10 and 150")
    if args.expected_component_count < COMPONENT_COUNT:
        parser.error(f"--expected-component-count must be at least {COMPONENT_COUNT}")
    if args.database_type:
        if not args.database_port or not 1 <= args.database_port <= 65535:
            parser.error("--database-port must be between 1 and 65535")
        if not args.database_name or not args.database_username:
            parser.error("database tests require --database-name and --database-username")
    if args.database_type and args.config_overlay:
        parser.error("--database-type and --config-overlay cannot be combined")
    return args


def normalized(path):
    return path.expanduser().resolve()


def require_owned_run_root(run_root, binary_root):
    # 清理目录必须严格位于 CMake 构建树内，任何盘符或前缀异常都在删除前失败。
    # The cleanup directory must be strictly inside the CMake build tree; drive or prefix mismatches fail before deletion.
    try:
        shared = Path(os.path.commonpath((str(run_root), str(binary_root))))
    except ValueError as exc:
        raise RuntimeError(f"run root and binary root are on different filesystems: {exc}") from exc
    if os.path.normcase(str(shared)) != os.path.normcase(str(binary_root)) or run_root == binary_root:
        raise RuntimeError(f"run root must stay below the CMake binary tree: {run_root}")


def remove_run_root(run_root, timeout_seconds=5):
    """Remove a prior owned run after Windows releases recently closed log handles.
    等待 Windows 释放刚关闭的日志句柄后，删除上一次自有运行目录。
    """
    deadline = time.monotonic() + timeout_seconds
    while run_root.exists():
        try:
            shutil.rmtree(run_root)
            return
        except OSError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.1)


def parse_components(encoded, expected_count=COMPONENT_COUNT):
    components = []
    for item in filter(None, encoded.split("|")):
        fields = item.split("::")
        if len(fields) != 4:
            raise RuntimeError(f"invalid component specification: {item}")
        executable = normalized(Path(fields[1]))
        if not executable.is_file():
            raise RuntimeError(f"component executable is missing: {executable}")
        components.append(
            {
                "name": fields[0],
                "executable": executable,
                "component_id": int(fields[2]),
                "global_order": int(fields[3]),
            }
        )
    if len(components) != expected_count:
        raise RuntimeError(
            f"server cluster requires {expected_count} components, received {len(components)}"
        )
    return components


def set_xml_value(parent, name, value):
    element = parent.find(name)
    if element is None:
        element = ET.SubElement(parent, name)
    element.text = value


def create_database_overlay(args, run_root):
    candidates = (
        args.assets_root / "res" / "server" / "kbengine.xml",
        args.assets_root / "kbengine.xml",
    )
    source = next((path for path in candidates if path.is_file()), None)
    if source is None:
        raise RuntimeError(f"assets do not contain a kbengine.xml database configuration: {args.assets_root}")

    tree = ET.parse(source)
    database = tree.getroot().find("./dbmgr/databaseInterfaces/default")
    if database is None:
        raise RuntimeError(f"database interface 'default' is missing from {source}")

    set_xml_value(database, "type", args.database_type)
    set_xml_value(database, "host", args.database_host)
    set_xml_value(database, "port", str(args.database_port))
    set_xml_value(database, "databaseName", args.database_name)
    auth = database.find("auth")
    if auth is None:
        auth = ET.SubElement(database, "auth")
    set_xml_value(auth, "username", args.database_username)
    set_xml_value(auth, "password", args.database_password)
    set_xml_value(auth, "encrypt", "false")
    set_xml_value(auth, "authSource", "admin" if args.database_type == "mongodb" else "")

    destination = run_root / "config-overlay" / "res" / "server" / "kbengine.xml"
    destination.parent.mkdir(parents=True, exist_ok=True)
    tree.write(destination, encoding="utf-8", xml_declaration=True)
    return destination.parents[1]


def read_text(path):
    if not path.is_file():
        return ""
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        # Windows may deny a concurrent log read while the logger rotates or flushes.
        # Windows 下 logger 轮转或刷新期间可能暂时拒绝并发读取。
        return ""


def component_output(entry, run_root):
    output = [read_text(entry["stdout"]), read_text(entry["stderr"])]
    log_root = run_root / "logs"
    if log_root.is_dir():
        paths = set(log_root.glob(f"{entry['spec']['name']}*.log"))
        paths.update(log_root.glob(f"logger_{entry['spec']['name']}.log"))
        output.extend(read_text(path) for path in sorted(paths))
    return "\n".join(output)


def centralized_discovery_marker(log_text, component_name, component_id):
    """Match one component's discovery marker in the central Logger stream.
    在中央 Logger 流中精确匹配一个组件的发现完成标记。

    Performance runs use a rolling logger file whose name does not follow the
    legacy ``logger_<component>.log`` convention.  Requiring both component
    name and CID prevents another component's marker from satisfying this one.
    性能压测使用不遵循旧 ``logger_<component>.log`` 命名的滚动日志；同时要求
    组件名和 CID，可以避免用其他组件的完成行误判当前组件已经完成发现。
    """
    marker = re.compile(
        rf"\b{re.escape(component_name)}\b[^\r\n]*"
        rf"\b{int(component_id)}\b[^\r\n]*Found all the components!",
        re.IGNORECASE,
    )
    return marker.search(log_text) is not None


def component_discovery_complete(entry, run_root):
    if "Found all the components!" in component_output(entry, run_root):
        return True

    log_root = run_root / "logs"
    if not log_root.is_dir():
        return False
    component_name = entry["spec"]["name"]
    component_id = entry["spec"]["component_id"]
    return any(
        centralized_discovery_marker(read_text(path), component_name, component_id)
        for path in sorted(log_root.glob("logger*.log"))
    )


def all_run_logs(entries, run_root):
    output = []
    for entry in entries:
        output.extend((read_text(entry["stdout"]), read_text(entry["stderr"])))
    log_root = run_root / "logs"
    if log_root.is_dir():
        output.extend(read_text(path) for path in sorted(log_root.glob("*.log")))
    return "\n".join(output)


def wait_for_running(entries, run_root, timeout):
    deadline = time.monotonic() + timeout
    pending = []
    while time.monotonic() < deadline:
        pending.clear()
        for entry in entries:
            result = entry["process"].poll()
            if result is not None:
                raise RuntimeError(
                    f"{entry['spec']['name']} exited during startup with code {result}"
                )
            marker = re.compile(
                rf"----\s+{re.escape(entry['spec']['name'])}\s+is running\s+----",
                re.IGNORECASE,
            )
            if not marker.search(component_output(entry, run_root)):
                pending.append(entry["spec"]["name"])
        if not pending:
            return
        time.sleep(0.25)
    raise RuntimeError(f"components did not reach running state before timeout: {', '.join(pending)}")


def wait_for_process_stability(entries, seconds=2.0):
    """Verify that every spawned child survives a short ownership handoff window.
    确认全部子进程在控制权交接窗口内保持存活，不把日志文本冒充语义就绪。
    """
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        for entry in entries:
            result = entry["process"].poll()
            if result is not None:
                raise RuntimeError(
                    f"{entry['spec']['name']} exited during startup with code {result}"
                )
        time.sleep(min(0.1, max(deadline - time.monotonic(), 0.0)))


def wait_for_discovery(entries, run_root):
    deadline = time.monotonic() + 30
    managers = [entry for entry in entries if entry["spec"]["name"] in ("baseappmgr", "cellappmgr")]
    while time.monotonic() < deadline:
        if all(component_discovery_complete(entry, run_root) for entry in managers):
            return
        time.sleep(0.25)
    raise RuntimeError("BaseAppMgr and CellAppMgr did not complete component discovery before timeout")


def stop_processes(entries):
    # 仅按逆序停止本次创建的进程对象，不按名称扫描或终止开发机上的其他实例。
    # Stop only process objects created by this run, in reverse order, without scanning for other instances by name.
    for entry in reversed(entries):
        process = entry["process"]
        if process.poll() is None:
            try:
                process.terminate()
            except OSError:
                pass
    for entry in entries:
        process = entry["process"]
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            try:
                process.kill()
                process.wait(timeout=5)
            except OSError:
                pass
        finally:
            entry["stdout_handle"].close()
            entry["stderr_handle"].close()


def write_component_manifest(entries, run_root):
    """Publish owned child PIDs so an external controller can sample only this cluster.
    发布本轮子进程 PID，使外部控制器只采样自己启动的集群。
    """
    manifest = [
        {
            "name": entry["spec"]["name"],
            "component_id": entry["spec"]["component_id"],
            "pid": entry["process"].pid,
        }
        for entry in entries
    ]
    (run_root / "components.json").write_text(
        json.dumps(manifest, indent=2) + "\n",
        encoding="utf-8",
    )


def hold_ready_cluster(entries, stop_file):
    stop_file = normalized(stop_file)
    while not stop_file.exists():
        for entry in entries:
            result = entry["process"].poll()
            if result is not None:
                # 子进程的 stderr 包含 KBE_ASSERT 表达式，但控制器清理后调用方可能只剩聚合日志。
                # Preserve a bounded output tail in the controller failure so KBE_ASSERT evidence
                # survives child-log cleanup without copying potentially huge runtime logs.
                output_tail = component_output(entry, stop_file.parent / "server")[-4096:].strip()
                raise RuntimeError(
                    f"{entry['spec']['name']} exited while cluster was held with code {result}; "
                    f"output_tail={output_tail or '<empty>'}"
                )
        time.sleep(0.25)


def run_cluster(args):
    args.repository_root = normalized(args.repository_root)
    args.assets_root = normalized(args.assets_root)
    if args.config_overlay:
        args.config_overlay = normalized(args.config_overlay)
        if not (args.config_overlay / "server/kbengine.xml").is_file():
            raise RuntimeError(f"config overlay does not contain server/kbengine.xml: {args.config_overlay}")
    args.resource_overlay = [normalized(path) for path in args.resource_overlay]
    for path in args.resource_overlay:
        if not path.is_dir():
            raise RuntimeError(f"resource overlay does not exist: {path}")
    args.binary_root = normalized(args.binary_root)
    args.run_root = normalized(args.run_root)
    require_owned_run_root(args.run_root, args.binary_root)
    components = parse_components(args.components, args.expected_component_count)

    remove_run_root(args.run_root)
    args.run_root.mkdir(parents=True)

    resource_roots = []
    if args.config_overlay:
        resource_roots.append(args.config_overlay)
        if args.resource_overlay:
            # 1.x derives Python script imports from the first server/kbengine.xml,
            # which is the per-run config overlay. Stage fixture component scripts
            # beside that overlay so Base/Cell/Login components execute the same
            # callbacks as Bots while entity definitions still come from the overlay.
            script_root = args.config_overlay.parent.parent / "scripts"
            for component in ("common", "data", "user_type", "base", "cell", "bots", "db", "interface", "login", "logger"):
                source = args.resource_overlay[0] / component
                if source.is_dir():
                    shutil.copytree(
                        source,
                        script_root / component,
                        dirs_exist_ok=True,
                        ignore=shutil.ignore_patterns("__pycache__", "*.pyc", "*.pyo"),
                    )
    resource_roots.extend(args.resource_overlay)
    if args.database_type:
        # 数据库覆盖只写构建树并排在原 assets 前面，保证源资产只读且覆盖项优先命中。
        # The database overlay stays in the build tree and precedes source assets, keeping them read-only while giving overrides priority.
        resource_roots.append(create_database_overlay(args, args.run_root))
    resource_roots.extend(
        (
            args.repository_root / "kbe" / "res",
            args.assets_root,
            args.assets_root / "scripts",
            args.assets_root / "res",
        )
    )
    environment = os.environ.copy()
    environment.update(
        {
            "KBE_ROOT": str(args.repository_root),
            "KBE_RES_PATH": os.pathsep.join(str(path) for path in resource_roots),
            "KBE_BIN_PATH": str(components[0]["executable"].parent),
            # 禁止 Debug CRT 弹窗；abort 仍然保留并由控制器检查退出码。
            # Disable Debug CRT dialogs; abort remains enabled and is checked by the controller.
            "KBE_TEST_MODE": "1",
        }
    )

    entries = []
    try:
        for spec in components:
            stdout_path = args.run_root / f"{spec['name']}.stdout.log"
            stderr_path = args.run_root / f"{spec['name']}.stderr.log"
            stdout_handle = stdout_path.open("wb")
            stderr_handle = stderr_path.open("wb")
            creation_flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
            try:
                process = subprocess.Popen(
                    (
                        str(spec["executable"]),
                        f"--cid={spec['component_id']}",
                        f"--gus={spec['global_order']}",
                        "--hide=1",
                    ),
                    cwd=args.run_root,
                    env=environment,
                    stdin=subprocess.DEVNULL,
                    stdout=stdout_handle,
                    stderr=stderr_handle,
                    creationflags=creation_flags,
                )
            except Exception:
                stdout_handle.close()
                stderr_handle.close()
                raise
            entries.append(
                {
                    "spec": spec,
                    "process": process,
                    "stdout": stdout_path,
                    "stderr": stderr_path,
                    "stdout_handle": stdout_handle,
                    "stderr_handle": stderr_handle,
                }
            )
            # Machine 先建立发现端点，其余组件仍通过真实网络发现流程完成注册。
            # Machine establishes discovery first; the remaining components still register through the real network discovery flow.
            time.sleep(1.0 if spec["name"] == "machine" else 0.15)

        if args.external_readiness:
            # PerformanceCluster performs Manager onReadyForLogin checks through
            # Watcher. This stage only proves ownership and early process health.
            # 性能控制器会通过 Watcher 校验 Manager onReadyForLogin；本阶段
            # 只确认进程归属和早期存活，避免依赖可被日志级别过滤的 INFO。
            wait_for_process_stability(entries)
        else:
            wait_for_running(entries, args.run_root, args.startup_timeout_seconds)
            wait_for_discovery(entries, args.run_root)
        for entry in entries:
            result = entry["process"].poll()
            if result is not None:
                raise RuntimeError(
                    f"{entry['spec']['name']} exited after reporting ready with code {result}"
                )

        combined_log = all_run_logs(entries, args.run_root)
        if ERROR_PATTERN.search(combined_log):
            raise RuntimeError(f"server cluster emitted an error; inspect logs under {args.run_root}")

        if args.database_type:
            dbmgr = next(entry for entry in entries if entry["spec"]["name"] == "dbmgr")
            database_pattern = re.compile(
                rf"(?:type|dbtype)={re.escape(args.database_type)}.*"
                rf"(?:db|currdatabase)={re.escape(args.database_name)}.*connected=(?:yes|true)",
                re.IGNORECASE | re.DOTALL,
            )
            if not database_pattern.search(component_output(dbmgr, args.run_root)):
                raise RuntimeError(
                    f"DBMgr did not confirm {args.database_type} database '{args.database_name}' as connected"
                )

        database_result = f" database={args.database_type}" if args.database_type else ""
        print(f"SERVER_CLUSTER_PASS components={len(entries)}{database_result}", flush=True)
        write_component_manifest(entries, args.run_root)
        if args.hold_until_file:
            print(f"SERVER_CLUSTER_READY stop_file={normalized(args.hold_until_file)}", flush=True)
            hold_ready_cluster(entries, args.hold_until_file)
    finally:
        stop_processes(entries)


def database_is_reachable(host, port):
    # 端口预检只判断测试依赖是否启动；认证和数据库语义仍由 DBMgr 验证，不能被预检掩盖。
    # The port probe only checks whether the test dependency is running; DBMgr still validates authentication and database semantics.
    try:
        with socket.create_connection((host, port), timeout=1.0):
            return True
    except OSError:
        return False


def main():
    args = parse_arguments()
    if args.database_type and not database_is_reachable(args.database_host, args.database_port):
        print(
            f"SERVER_CLUSTER_SKIP database={args.database_type} "
            f"endpoint={args.database_host}:{args.database_port} reason=unreachable",
            flush=True,
        )
        return 77
    try:
        run_cluster(args)
        return 0
    except Exception as exc:
        print(f"SERVER_CLUSTER_FAIL error={exc} run_root={args.run_root}", file=sys.stderr, flush=True)
        return 1


if __name__ == "__main__":
    sys.exit(main())
