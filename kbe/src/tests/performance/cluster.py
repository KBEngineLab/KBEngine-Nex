"""Lifecycle adapter for the existing server-cluster controller.
现有服务端集群控制器的生命周期适配层。
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import time
from collections import Counter
import re
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True, slots=True)
class ClusterProcess:
    name: str
    pid: int


class PerformanceCluster:
    """Start and stop only the isolated cluster owned by one performance run.
    只启动和停止单次性能运行所拥有的隔离集群。
    """

    def __init__(
        self,
        repository_root: Path,
        assets_root: Path,
        binary_root: Path,
        run_root: Path,
        components: str,
        config_overlay_root: Path,
        fixture_root: Path | None = None,
        startup_timeout_seconds: int = 90,
    ):
        self.repository_root = repository_root.resolve()
        self.assets_root = assets_root.resolve()
        self.binary_root = binary_root.resolve()
        self.run_root = run_root.resolve()
        self.components = components
        self.config_overlay_root = config_overlay_root.resolve()
        self.fixture_root = fixture_root.resolve() if fixture_root is not None else None
        self.startup_timeout_seconds = startup_timeout_seconds
        self.stop_file = self.run_root / "stop.requested"
        self.stdout_path = self.run_root / "cluster-controller.stdout.log"
        self.stderr_path = self.run_root / "cluster-controller.stderr.log"
        self.process: subprocess.Popen[bytes] | None = None
        self._stdout_handle = None
        self._stderr_handle = None

    def start(self) -> list[ClusterProcess]:
        if self.process is not None:
            raise RuntimeError("performance cluster is already started")
        if not self.components.strip():
            raise ValueError("--cluster-components cannot be empty")
        self.run_root.mkdir(parents=True, exist_ok=False)
        self._stdout_handle = self.stdout_path.open("wb")
        self._stderr_handle = self.stderr_path.open("wb")
        script = self.repository_root / "kbe/src/tests/python/test_server_cluster.py"
        command = [
            sys.executable,
            "-B",
            str(script),
            "--components",
            self.components,
            "--expected-component-count",
            str(len(tuple(filter(None, self.components.split("|"))))),
            "--repository-root",
            str(self.repository_root),
            "--assets-root",
            str(self.assets_root),
            "--config-overlay",
            str(self.config_overlay_root),
            "--binary-root",
            str(self.binary_root),
            "--run-root",
            str(self.run_root / "server"),
            "--startup-timeout-seconds",
            str(self.startup_timeout_seconds),
            "--hold-until-file",
            str(self.stop_file),
        ]
        if self.fixture_root is not None:
            command.extend(("--resource-overlay", str(self.fixture_root)))
        creation_flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
        self.process = subprocess.Popen(
            tuple(command),
            cwd=self.run_root,
            stdin=subprocess.DEVNULL,
            stdout=self._stdout_handle,
            stderr=self._stderr_handle,
            creationflags=creation_flags,
        )
        try:
            self._wait_until_ready()
            return self.component_processes()
        except Exception:
            self.stop()
            raise

    def component_processes(self) -> list[ClusterProcess]:
        manifest_path = self.run_root / "server/components.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        counts = Counter(str(item["name"]) for item in manifest)
        return [
            ClusterProcess(
                (
                    f'{item["name"]}:{item["component_id"]}'
                    if counts[str(item["name"])] > 1
                    else str(item["name"])
                ),
                int(item["pid"]),
            )
            for item in manifest
        ]

    def stop(self) -> None:
        if self.process is None:
            self._close_logs()
            return
        if self.process.poll() is None:
            self.stop_file.touch(exist_ok=True)
            try:
                self.process.wait(timeout=20)
            except subprocess.TimeoutExpired:
                self.process.terminate()
                try:
                    self.process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.process.wait()
        self._close_logs()

    def wait_for_log_readiness(
        self,
        pattern: str,
        minimum_count: int,
        log_glob: str,
        timeout_seconds: float,
    ) -> int:
        """Wait for an asset-owned log milestone before starting workload clients.
        在启动压测客户端前等待业务资产定义的日志里程碑。
        """
        if minimum_count < 1 or timeout_seconds <= 0:
            raise ValueError("server log readiness count and timeout must be positive")
        expression = re.compile(pattern)
        deadline = time.monotonic() + timeout_seconds
        log_root = self.run_root / "server/logs"
        observed = 0
        while time.monotonic() < deadline:
            if self.process is not None and self.process.poll() is not None:
                raise RuntimeError(self._failure_message("cluster exited before server log readiness"))
            observed = sum(
                len(expression.findall(self._read_text(path)))
                for path in log_root.glob(log_glob)
                if path.is_file()
            )
            if observed >= minimum_count:
                return observed
            time.sleep(0.25)
        raise TimeoutError(
            f"server log readiness timed out after {timeout_seconds:.1f}s: "
            f"pattern={pattern!r}, observed={observed}, expected>={minimum_count}"
        )

    def _wait_until_ready(self) -> None:
        deadline = time.monotonic() + self.startup_timeout_seconds + 15
        while time.monotonic() < deadline:
            if self.process is not None and self.process.poll() is not None:
                raise RuntimeError(self._failure_message("cluster controller exited before readiness"))
            if "SERVER_CLUSTER_READY" in self._read_text(self.stdout_path):
                return
            time.sleep(0.25)
        raise TimeoutError(self._failure_message("cluster controller readiness timed out"))

    def _failure_message(self, message: str) -> str:
        stderr = self._read_text(self.stderr_path).strip()
        return f"{message}: {stderr or self.stderr_path}"

    def _close_logs(self) -> None:
        for handle_name in ("_stdout_handle", "_stderr_handle"):
            handle = getattr(self, handle_name)
            if handle is not None:
                handle.close()
                setattr(self, handle_name, None)

    @staticmethod
    def _read_text(path: Path) -> str:
        if not path.is_file():
            return ""
        return path.read_text(encoding="utf-8", errors="replace")
