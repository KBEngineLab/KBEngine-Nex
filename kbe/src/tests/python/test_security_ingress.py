#!/usr/bin/env python3

import argparse
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import sys
import time


COMPONENT_IDS = {
    "baseappmgr": 5000,
    "cellappmgr": 6000,
    "baseapp": 7001,
    "cellapp": 8001,
}

PROBES = (
    (
        "baseappmgr-zero-forward",
        "baseappmgr",
        "BASEAPPMGR_TYPE",
        "Baseappmgr::forwardMessage",
        r"Baseappmgr::forwardMessage: rejected unbound senderComponent\(0\)",
    ),
    (
        "baseappmgr-spoofed-update",
        "baseappmgr",
        "BASEAPPMGR_TYPE",
        "Baseappmgr::updateBaseapp",
        r"Baseappmgr::updateBaseapp: rejected componentID\(7001\)",
    ),
    (
        "cellappmgr-zero-forward",
        "cellappmgr",
        "CELLAPPMGR_TYPE",
        "Cellappmgr::forwardMessage",
        r"Cellappmgr::forwardMessage: rejected unbound senderComponent\(0\)",
    ),
    (
        "cellappmgr-spoofed-update",
        "cellappmgr",
        "CELLAPPMGR_TYPE",
        "Cellappmgr::updateCellapp",
        r"Cellappmgr::updateCellapp: rejected componentID\(8001\)",
    ),
    (
        "baseapp-spoofed-callback",
        "baseapp",
        "BASEAPP_TYPE",
        "Baseapp::onCreateEntityAnywhereCallback",
        r"onCreateEntityAnywhereCallback: rejected unbound sourceComponent\(7001\)",
    ),
    (
        "cellapp-spoofed-teleport",
        "cellapp",
        "CELLAPP_TYPE",
        "Cellapp::reqTeleportToCellAppCB",
        r"reqTeleportToCellAppCB: rejected unbound targetCellappID=8001",
    ),
)


def parse_arguments():
    parser = argparse.ArgumentParser(
        description="Start a real KBEngine cluster and verify fail-closed internal ingress paths."
    )
    parser.add_argument("--cluster-script", type=Path, required=True)
    parser.add_argument("--components", required=True)
    parser.add_argument("--repository-root", type=Path, required=True)
    parser.add_argument("--assets-root", type=Path, required=True)
    parser.add_argument("--binary-root", type=Path, required=True)
    parser.add_argument("--run-root", type=Path, required=True)
    parser.add_argument("--startup-timeout-seconds", type=int, default=90)
    args = parser.parse_args()
    if not 10 <= args.startup_timeout_seconds <= 150:
        parser.error("--startup-timeout-seconds must be between 10 and 150")
    return args


def normalized(path):
    return path.expanduser().resolve()


def read_text(path):
    if not path.is_file():
        return ""
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        # 子进程可能正在以独占共享模式刷新日志；下一轮轮询会重试。
        # A child process may be flushing a log with exclusive sharing; retry on the next poll.
        return ""


def all_logs(run_root, controller_stdout, controller_stderr):
    paths = [controller_stdout, controller_stderr]
    if run_root.is_dir():
        paths.extend(sorted(run_root.rglob("*.log")))
    return "\n".join(read_text(path) for path in paths)


def wait_for_text(process, run_root, controller_stdout, controller_stderr, pattern, timeout, description):
    deadline = time.monotonic() + timeout
    expression = re.compile(pattern, re.IGNORECASE)
    while time.monotonic() < deadline:
        output = all_logs(run_root, controller_stdout, controller_stderr)
        if expression.search(output):
            return output
        result = process.poll()
        if result is not None:
            raise RuntimeError(
                f"cluster controller exited with code {result} while waiting for {description}"
            )
        time.sleep(0.1)
    raise RuntimeError(f"timed out waiting for {description}")


def find_endpoint(log_text, component_name):
    component_id = COMPONENT_IDS[component_name]
    pattern = re.compile(
        rf"componentID:{component_id}\b[^\r\n]*?intaddr:([^,\s]+),\s*intport:(\d+)",
        re.IGNORECASE,
    )
    match = pattern.search(log_text)
    if not match:
        raise RuntimeError(f"internal endpoint for {component_name}({component_id}) was not published")
    host = match.group(1)
    port = int(match.group(2))
    if not 1 <= port <= 65535:
        raise RuntimeError(f"invalid internal port for {component_name}: {port}")
    return host, port


def flatten_values(values):
    result = {}
    for key, value in values.items():
        if isinstance(value, dict):
            result.update(flatten_values(value))
        else:
            result[str(key)] = value
    return result


def query_watcher_snapshot(watcher, path, timeout=5):
    watcher.clearWatchData()
    watcher.requireQueryWatcher(path)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline and len(watcher.watchData) < 2:
        watcher.processOne(min(0.25, max(deadline - time.monotonic(), 0.01)))
    if len(watcher.watchData) < 2:
        raise TimeoutError(f"Watcher query timed out: {path}")
    return watcher.watchData


def query_message_contract(repository_root, component_type_name, endpoint, message_name):
    # Query the live target's registry instead of duplicating auto-assigned IDs.
    # 从目标进程的实时注册表读取协议，不在测试中复制自动分配的消息 ID。
    tools_root = repository_root / "kbe" / "tools" / "server"
    sys.path.insert(0, str(tools_root))
    watcher = None
    try:
        from pycommon import Define, Watcher

        component_type = getattr(Define, component_type_name)
        watcher = Watcher.Watcher(component_type)
        watcher.connect(*endpoint)
        # Watcher readWatchers resolves directory paths below its ``root`` node;
        # querying a leaf such as ``.../id`` returns an empty snapshot. Query the message directory once,
        # then read both protocol leaves from that snapshot.
        # Watcher 的 readWatchers 从 ``root`` 节点解析目录路径；直接查询 ``.../id`` 会得到空快照。
        # 一次查询消息目录，再从同一快照读取两个协议叶子。
        snapshot = query_watcher_snapshot(
            watcher, f"root/network/messages/{message_name}"
        )
        values = next((item["values"] for item in snapshot if item["type"] == 0), {})
        contract = flatten_values(values)
        if "id" not in contract or "len" not in contract:
            raise RuntimeError(
                f"Watcher did not return id/len for {message_name}: {contract}"
            )
        contract = {field: int(contract[field]) for field in ("id", "len")}
        return contract["id"], contract["len"]
    finally:
        if watcher is not None:
            watcher.close()
        sys.path.pop(0)


def probe_body(probe_case):
    if probe_case == "baseappmgr-zero-forward":
        return struct.pack("=QQ", 0, 7001)
    if probe_case == "baseappmgr-spoofed-update":
        return struct.pack("=QiifI", 7001, 0, 0, float("nan"), 0)
    if probe_case == "cellappmgr-zero-forward":
        return struct.pack("=QQ", 0, 8001)
    if probe_case == "cellappmgr-spoofed-update":
        return struct.pack("=QifI", 8001, 0, float("nan"), 0)
    if probe_case == "baseapp-spoofed-callback":
        return struct.pack("=I", 1) + b"Account\0" + struct.pack("=iQ", 1, 7001)
    if probe_case == "cellapp-spoofed-teleport":
        return struct.pack("=QQQib", 8001, 8001, 7001, 1, 0)
    raise RuntimeError(f"unsupported probe case: {probe_case}")


def run_probe(args, probe_case, component_type_name, message_name, endpoint):
    message_id, message_length = query_message_contract(
        args.repository_root, component_type_name, endpoint, message_name
    )
    body = probe_body(probe_case)
    frame = struct.pack("=H", message_id)
    if message_length == -1:
        if len(body) > 65534:
            raise RuntimeError(f"probe body is too large: {probe_case}")
        frame += struct.pack("=H", len(body))
    elif message_length != len(body):
        raise RuntimeError(
            f"probe contract mismatch for {message_name}: target={message_length} body={len(body)}"
        )
    frame += body

    with socket.create_connection(endpoint, timeout=5) as connection:
        connection.sendall(frame)
        connection.shutdown(socket.SHUT_WR)
        time.sleep(0.05)


def run_test(args):
    args.cluster_script = normalized(args.cluster_script)
    args.repository_root = normalized(args.repository_root)
    args.assets_root = normalized(args.assets_root)
    args.binary_root = normalized(args.binary_root)
    args.run_root = normalized(args.run_root)
    if not args.cluster_script.is_file():
        raise RuntimeError(f"required cluster script is missing: {args.cluster_script}")

    stop_file = args.run_root / "security-probes-complete.stop"
    controller_stdout = args.run_root.parent / f"{args.run_root.name}.controller.stdout.log"
    controller_stderr = args.run_root.parent / f"{args.run_root.name}.controller.stderr.log"
    for path in (controller_stdout, controller_stderr):
        if path.exists():
            path.unlink()

    command = (
        sys.executable,
        "-B",
        str(args.cluster_script),
        "--components",
        args.components,
        "--repository-root",
        str(args.repository_root),
        "--assets-root",
        str(args.assets_root),
        "--binary-root",
        str(args.binary_root),
        "--run-root",
        str(args.run_root),
        "--startup-timeout-seconds",
        str(args.startup_timeout_seconds),
        "--hold-until-file",
        str(stop_file),
    )

    process = None
    stdout_handle = controller_stdout.open("wb")
    stderr_handle = controller_stderr.open("wb")
    try:
        process = subprocess.Popen(
            command,
            cwd=args.repository_root,
            stdin=subprocess.DEVNULL,
            stdout=stdout_handle,
            stderr=stderr_handle,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
        )
        ready_logs = wait_for_text(
            process,
            args.run_root,
            controller_stdout,
            controller_stderr,
            r"SERVER_CLUSTER_READY",
            args.startup_timeout_seconds + 30,
            "cluster readiness",
        )

        endpoints = {
            component_name: find_endpoint(ready_logs, component_name)
            for component_name in COMPONENT_IDS
        }
        for probe_case, component_name, component_type, message_name, rejection_pattern in PROBES:
            run_probe(
                args,
                probe_case,
                component_type,
                message_name,
                endpoints[component_name],
            )
            wait_for_text(
                process,
                args.run_root,
                controller_stdout,
                controller_stderr,
                rejection_pattern,
                10,
                f"rejection marker for {probe_case}",
            )

        # The cluster controller checks every owned component while held. Keeping
        # it alive across a scheduling interval proves the malformed frames did
        # not terminate a target process. 集群控制器在 hold 状态持续检查全部子进程；
        # 跨过一个调度周期仍存活，证明畸形帧没有终止目标组件。
        time.sleep(0.5)
        if process.poll() is not None:
            raise RuntimeError(f"cluster exited after probes with code {process.returncode}")

        stop_file.touch()
        result = process.wait(timeout=30)
        if result != 0:
            raise RuntimeError(f"cluster cleanup failed with code {result}")
        print(f"SECURITY_MULTIPROCESS_INGRESS_PASS probes={len(PROBES)}", flush=True)
    finally:
        if process is not None and process.poll() is None:
            try:
                stop_file.parent.mkdir(parents=True, exist_ok=True)
                stop_file.touch()
                process.wait(timeout=10)
            except (OSError, subprocess.TimeoutExpired):
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=10)
        stdout_handle.close()
        stderr_handle.close()


def main():
    args = parse_arguments()
    try:
        run_test(args)
        return 0
    except Exception as exception:
        print(
            f"SECURITY_MULTIPROCESS_INGRESS_FAIL error={exception} run_root={args.run_root}",
            file=sys.stderr,
            flush=True,
        )
        return 1


if __name__ == "__main__":
    sys.exit(main())
