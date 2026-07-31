#!/usr/bin/env python3

import argparse
from pathlib import Path
import re
import subprocess
import sys

from test_security_ingress import find_external_endpoint, normalized, wait_for_text


def parse_arguments():
    parser = argparse.ArgumentParser(
        description="Verify an authenticated BaseApp Channel rejects another entity ID."
    )
    parser.add_argument("--cluster-script", type=Path, required=True)
    parser.add_argument("--probe-script", type=Path, required=True)
    parser.add_argument("--node-executable", type=Path, required=True)
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


def require_file(path, description):
    path = normalized(path)
    if not path.is_file():
        raise RuntimeError(f"{description} is missing: {path}")
    return path


def run_test(args):
    args.cluster_script = require_file(args.cluster_script, "cluster script")
    args.probe_script = require_file(args.probe_script, "authenticated client probe")
    args.node_executable = require_file(args.node_executable, "Node.js executable")
    args.repository_root = normalized(args.repository_root)
    args.assets_root = normalized(args.assets_root)
    args.binary_root = normalized(args.binary_root)
    args.run_root = normalized(args.run_root)

    client_root = args.assets_root / "kbeclient" / "typescript-component-e2e"
    require_file(
        client_root / "dist" / "typescript-e2e-generated-v3" / "KBEngine.js",
        "generated TypeScript SDK",
    )
    require_file(client_root / "node_modules" / "ws" / "index.js", "WebSocket runtime")

    stop_file = args.run_root / "authenticated-client-probe-complete.stop"
    controller_stdout = args.run_root.parent / f"{args.run_root.name}.controller.stdout.log"
    controller_stderr = args.run_root.parent / f"{args.run_root.name}.controller.stderr.log"
    client_stdout = args.run_root.parent / f"{args.run_root.name}.client.stdout.log"
    client_stderr = args.run_root.parent / f"{args.run_root.name}.client.stderr.log"
    for path in (controller_stdout, controller_stderr, client_stdout, client_stderr):
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
            creationflags=subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0,
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
        # Process readiness precedes DBMgr's initialization fan-out. LoginApp
        # correctly rejects logins until this marker advances initProgress.
        # 进程就绪早于 DBMgr 初始化广播；等待该标记后 LoginApp 才会开放登录。
        ready_logs = wait_for_text(
            process,
            args.run_root,
            controller_stdout,
            controller_stderr,
            r"Loginapp::onDbmgrInitCompleted:",
            30,
            "LoginApp database initialization",
        )
        ready_logs = wait_for_text(
            process,
            args.run_root,
            controller_stdout,
            controller_stderr,
            r"Baseappmgr::onBaseappInitProgress: all completed!",
            30,
            "BaseApp manager initialization",
        )
        ready_logs = wait_for_text(
            process,
            args.run_root,
            controller_stdout,
            controller_stderr,
            r"Cellappmgr::onCellappInitProgress: cid=\d+, progress=1",
            30,
            "CellApp manager initialization",
        )
        login_host, login_port = find_external_endpoint(ready_logs, "loginapp")

        client_result = subprocess.run(
            (
                str(args.node_executable),
                str(args.probe_script),
                str(client_root),
                login_host,
                str(login_port),
            ),
            cwd=args.run_root,
            stdin=subprocess.DEVNULL,
            capture_output=True,
            timeout=45,
            creationflags=subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0,
        )
        client_stdout.write_bytes(client_result.stdout)
        client_stderr.write_bytes(client_result.stderr)
        client_output = (client_result.stdout + b"\n" + client_result.stderr).decode(
            "utf-8", errors="replace"
        )
        if client_result.returncode != 0:
            raise RuntimeError(
                f"authenticated client exited with code {client_result.returncode}: {client_output}"
            )

        marker = re.search(
            r"AUTHENTICATED_CLIENT_PROBE_SENT requested=(\d+) proxyID=(\d+)",
            client_output,
        )
        if marker is None:
            raise RuntimeError(f"authenticated client did not report its principal: {client_output}")
        requested_id, proxy_id = (int(value) for value in marker.groups())
        if requested_id == proxy_id or proxy_id <= 0:
            raise RuntimeError(
                f"authenticated client produced invalid IDs requested={requested_id}, proxyID={proxy_id}"
            )

        wait_for_text(
            process,
            args.run_root,
            controller_stdout,
            controller_stderr,
            rf"Baseapp::reqAccountBindEmail: rejected unbound entity, "
            rf"requested={requested_id}, proxyID={proxy_id}",
            10,
            "authenticated entity-binding rejection",
        )

        if process.poll() is not None:
            raise RuntimeError(f"cluster exited after authenticated probe with code {process.returncode}")
        stop_file.touch()
        result = process.wait(timeout=30)
        if result != 0:
            raise RuntimeError(f"cluster cleanup failed with code {result}")
        print(
            f"SECURITY_AUTHENTICATED_CLIENT_INGRESS_PASS requested={requested_id} proxyID={proxy_id}",
            flush=True,
        )
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
            f"SECURITY_AUTHENTICATED_CLIENT_INGRESS_FAIL error={exception} run_root={args.run_root}",
            file=sys.stderr,
            flush=True,
        )
        return 1


if __name__ == "__main__":
    sys.exit(main())
