#!/usr/bin/env python3

import argparse
import json
from pathlib import Path
import os
import shutil
import subprocess
import sys
import time

from test_security_ingress import find_external_endpoint, normalized, read_text, wait_for_text


def parse_arguments():
    parser = argparse.ArgumentParser(
        description="Verify authenticated Cell RPC target relationships with real Avatars and Spaces."
    )
    parser.add_argument("--cluster-script", type=Path, required=True)
    parser.add_argument("--probe-script", type=Path, required=True)
    parser.add_argument("--resource-overlay", type=Path, required=True)
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


def read_state(path):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None


def wait_for_state(processes, state_path, predicate, timeout, description):
    deadline = time.monotonic() + timeout
    last_state = None
    while time.monotonic() < deadline:
        for role, process in processes.items():
            if process.poll() is not None:
                raise RuntimeError(f"client {role} exited with code {process.returncode}")
        last_state = read_state(state_path)
        if last_state is not None and predicate(last_state):
            return last_state
        time.sleep(0.05)
    raise RuntimeError(f"timed out waiting for {description}: last_state={last_state}")


def write_command(path, sequence, action, **values):
    temporary_path = path.with_suffix(f".{sequence}.tmp")
    temporary_path.write_text(
        json.dumps({"sequence": sequence, "action": action, **values}) + "\n",
        encoding="utf-8",
    )
    temporary_path.replace(path)


def assert_cluster_alive(process):
    if process.poll() is not None:
        raise RuntimeError(f"cluster exited during relationship probe with code {process.returncode}")


def materialize_resource_overlay(assets_root, fixture_root, destination, binary_root):
    # Python selects one script root from the first matching entities.xml; it
    # does not merge base/cell/common directories across KBE_RES_PATH entries.
    # Build an isolated script tree so fixture overrides remain test-only while
    # unchanged modules still come from the real course assets.
    # Python 会从首个 entities.xml 选择单一脚本根，不会跨 KBE_RES_PATH 合并
    # base/cell/common。这里物化隔离脚本树，使覆盖仅用于测试，未修改模块仍来自课程资产。
    shared = Path(os.path.commonpath((str(destination), str(binary_root))))
    if os.path.normcase(str(shared)) != os.path.normcase(str(binary_root)):
        raise RuntimeError(f"materialized resource overlay must stay in the build tree: {destination}")
    if destination.exists():
        shutil.rmtree(destination)
    destination.mkdir(parents=True)

    for name in (
        "base",
        "cell",
        "common",
        "data",
        "db",
        "entity_defs",
        "interface",
        "login",
        "logger",
        "server_common",
        "user_type",
    ):
        source = assets_root / name
        if source.is_dir():
            shutil.copytree(source, destination / name)
    for name in ("entities.xml", "plugins.xml"):
        source = assets_root / name
        if source.is_file():
            shutil.copy2(source, destination / name)

    shutil.copytree(fixture_root, destination, dirs_exist_ok=True)
    return destination


def run_test(args):
    args.cluster_script = require_file(args.cluster_script, "cluster script")
    args.probe_script = require_file(args.probe_script, "authenticated Cell RPC probe")
    args.node_executable = require_file(args.node_executable, "Node.js executable")
    args.repository_root = normalized(args.repository_root)
    args.assets_root = normalized(args.assets_root)
    args.resource_overlay = normalized(args.resource_overlay)
    args.binary_root = normalized(args.binary_root)
    args.run_root = normalized(args.run_root)
    if not args.resource_overlay.is_dir():
        raise RuntimeError(f"resource overlay is missing: {args.resource_overlay}")

    materialized_overlay = args.run_root.parent / f"{args.run_root.name}-resources"
    materialize_resource_overlay(
        args.assets_root,
        args.resource_overlay,
        materialized_overlay,
        args.binary_root,
    )

    client_root = args.assets_root / "kbeclient" / "typescript-component-e2e"
    require_file(
        client_root / "dist" / "typescript-e2e-generated-v3" / "KBEngine.js",
        "generated TypeScript SDK",
    )
    require_file(client_root / "node_modules" / "ws" / "index.js", "WebSocket runtime")

    stop_file = args.run_root / "authenticated-cell-rpc-complete.stop"
    controller_stdout = args.run_root.parent / f"{args.run_root.name}.controller.stdout.log"
    controller_stderr = args.run_root.parent / f"{args.run_root.name}.controller.stderr.log"
    for path in (controller_stdout, controller_stderr):
        if path.exists():
            path.unlink()

    cluster_command = (
        sys.executable,
        "-B",
        str(args.cluster_script),
        "--components",
        args.components,
        "--repository-root",
        str(args.repository_root),
        "--assets-root",
        str(args.assets_root),
        "--resource-overlay",
        str(materialized_overlay),
        "--binary-root",
        str(args.binary_root),
        "--run-root",
        str(args.run_root),
        "--startup-timeout-seconds",
        str(args.startup_timeout_seconds),
        "--hold-until-file",
        str(stop_file),
    )

    cluster_process = None
    client_processes = {}
    client_handles = []
    stdout_handle = controller_stdout.open("wb")
    stderr_handle = controller_stderr.open("wb")
    try:
        cluster_process = subprocess.Popen(
            cluster_command,
            cwd=args.repository_root,
            stdin=subprocess.DEVNULL,
            stdout=stdout_handle,
            stderr=stderr_handle,
            creationflags=subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0,
        )
        ready_logs = wait_for_text(
            cluster_process,
            args.run_root,
            controller_stdout,
            controller_stderr,
            r"SERVER_CLUSTER_READY",
            args.startup_timeout_seconds + 30,
            "cluster readiness",
        )
        for pattern, description in (
            (r"Loginapp::onDbmgrInitCompleted:", "LoginApp database initialization"),
            (r"Baseappmgr::onBaseappInitProgress: all completed!", "BaseApp manager initialization"),
            (r"Cellappmgr::onCellappInitProgress: cid=\d+, progress=1", "CellApp manager initialization"),
        ):
            ready_logs = wait_for_text(
                cluster_process,
                args.run_root,
                controller_stdout,
                controller_stderr,
                pattern,
                30,
                description,
            )
        login_host, login_port = find_external_endpoint(ready_logs, "loginapp")

        coordination_root = args.run_root / "cell-rpc-clients"
        coordination_root.mkdir()
        unique_suffix = f"{int(time.time() * 1000)}_{cluster_process.pid}"
        roles = {
            "source": f"security_near_source_{unique_suffix}",
            "near": f"security_near_target_{unique_suffix}",
            "cross": f"security_cross_target_{unique_suffix}",
        }
        state_paths = {}
        command_paths = {}
        for role, avatar_name in roles.items():
            state_path = coordination_root / f"{role}.state.json"
            command_path = coordination_root / f"{role}.command.json"
            client_stdout = coordination_root / f"{role}.stdout.log"
            client_stderr = coordination_root / f"{role}.stderr.log"
            out_handle = client_stdout.open("wb")
            err_handle = client_stderr.open("wb")
            client_handles.extend((out_handle, err_handle))
            process = subprocess.Popen(
                (
                    str(args.node_executable),
                    str(args.probe_script),
                    str(client_root),
                    login_host,
                    str(login_port),
                    f"security_cell_rpc_{role}_{unique_suffix}",
                    avatar_name,
                    str(state_path),
                    str(command_path),
                ),
                cwd=args.run_root,
                stdin=subprocess.DEVNULL,
                stdout=out_handle,
                stderr=err_handle,
                creationflags=subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0,
            )
            client_processes[role] = process
            state_paths[role] = state_path
            command_paths[role] = command_path

        states = {
            role: wait_for_state(
                client_processes,
                state_paths[role],
                lambda state: state.get("entityID", 0) > 0 and state.get("spaceID", 0) > 0,
                45,
                f"{role} Avatar world entry",
            )
            for role in roles
        }
        source_id = states["source"]["entityID"]
        near_id = states["near"]["entityID"]
        cross_id = states["cross"]["entityID"]
        source_space = states["source"]["spaceID"]
        near_space = states["near"]["spaceID"]
        cross_space = states["cross"]["spaceID"]
        if len({source_id, near_id, cross_id}) != 3:
            raise RuntimeError(f"clients did not receive distinct Avatar IDs: {states}")
        if source_space != near_space or cross_space == source_space:
            raise RuntimeError(f"test overlay did not create the required Space layout: {states}")

        wait_for_state(
            client_processes,
            state_paths["source"],
            lambda state: near_id in state.get("visibleAvatarIds", []),
            15,
            "near non-controlled Avatar entering the source Witness",
        )
        write_command(command_paths["source"], 1, "rpc", targetID=near_id)
        wait_for_text(
            cluster_process,
            args.run_root,
            controller_stdout,
            controller_stderr,
            rf"SECURITY_CELL_RPC_ACCEPTED caller={source_id} target={near_id} spaceID={source_space}",
            10,
            "visible non-controlled target acceptance",
        )
        assert_cluster_alive(cluster_process)

        write_command(command_paths["near"], 2, "rpc", targetID=near_id)
        wait_for_state(
            client_processes,
            state_paths["source"],
            lambda state: near_id not in state.get("visibleAvatarIds", []),
            15,
            "same-Space target leaving the source Witness",
        )
        write_command(command_paths["source"], 2, "rpc", targetID=near_id)
        wait_for_text(
            cluster_process,
            args.run_root,
            controller_stdout,
            controller_stderr,
            rf"rejected unauthorized target, srcEntityID={source_id}, targetID={near_id}, "
            rf"sourceSpaceID={source_space}, targetSpaceID={near_space}",
            10,
            "same-Space invisible target rejection",
        )
        assert_cluster_alive(cluster_process)

        write_command(command_paths["source"], 3, "rpc", targetID=cross_id)
        wait_for_text(
            cluster_process,
            args.run_root,
            controller_stdout,
            controller_stderr,
            rf"rejected unauthorized target, srcEntityID={source_id}, targetID={cross_id}, "
            rf"sourceSpaceID={source_space}, targetSpaceID={cross_space}",
            10,
            "cross-Space target rejection",
        )
        assert_cluster_alive(cluster_process)

        for role in roles:
            write_command(command_paths[role], 99, "stop")
        for role, process in client_processes.items():
            result = process.wait(timeout=10)
            if result != 0:
                output = read_text(coordination_root / f"{role}.stderr.log")
                raise RuntimeError(f"client {role} cleanup failed with code {result}: {output}")

        stop_file.touch()
        result = cluster_process.wait(timeout=30)
        if result != 0:
            raise RuntimeError(f"cluster cleanup failed with code {result}")
        print(
            "SECURITY_AUTHENTICATED_CELL_RPC_PASS "
            f"source={source_id} visible={near_id} cross={cross_id}",
            flush=True,
        )
    finally:
        for process in client_processes.values():
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
        for handle in client_handles:
            handle.close()
        if cluster_process is not None and cluster_process.poll() is None:
            try:
                stop_file.parent.mkdir(parents=True, exist_ok=True)
                stop_file.touch()
                cluster_process.wait(timeout=10)
            except (OSError, subprocess.TimeoutExpired):
                cluster_process.terminate()
                try:
                    cluster_process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    cluster_process.kill()
                    cluster_process.wait(timeout=10)
        stdout_handle.close()
        stderr_handle.close()
        if materialized_overlay.exists():
            shutil.rmtree(materialized_overlay)


def main():
    args = parse_arguments()
    try:
        run_test(args)
        return 0
    except Exception as exception:
        print(
            f"SECURITY_AUTHENTICATED_CELL_RPC_FAIL error={exception} run_root={args.run_root}",
            file=sys.stderr,
            flush=True,
        )
        return 1


if __name__ == "__main__":
    sys.exit(main())
