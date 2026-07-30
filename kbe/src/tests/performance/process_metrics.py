"""Portable process sampling with standard-library fallbacks. / 使用标准库实现可移植进程采样。"""

from __future__ import annotations

import json
import os
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path


@dataclass(slots=True)
class ProcessSample:
    cpu_percent: float
    working_set_bytes: int
    thread_count: int


class ProcessCollector:
    def __init__(self, pid: int):
        self.pid = pid
        self._previous_cpu: float | None = None
        self._previous_time = time.monotonic()

    def sample(self) -> ProcessSample | None:
        if os.name == "nt":
            return self._sample_windows()
        return self._sample_procfs()

    def _sample_windows(self) -> ProcessSample | None:
        command = (
            "Get-Process -Id "
            + str(self.pid)
            + " | Select-Object CPU,WorkingSet64,Threads | ConvertTo-Json -Compress"
        )
        try:
            result = subprocess.run(
                ["powershell", "-NoProfile", "-NonInteractive", "-Command", command],
                capture_output=True,
                text=True,
                timeout=2,
                check=True,
            )
            data = json.loads(result.stdout)
            cpu_seconds = float(data.get("CPU") or 0.0)
            threads = data.get("Threads") or []
            return self._finish(cpu_seconds, int(data.get("WorkingSet64") or 0), len(threads))
        except (OSError, ValueError, subprocess.SubprocessError, json.JSONDecodeError):
            return None

    def _sample_procfs(self) -> ProcessSample | None:
        stat_path = Path(f"/proc/{self.pid}/stat")
        status_path = Path(f"/proc/{self.pid}/status")
        try:
            fields = stat_path.read_text(encoding="utf-8").split()
            cpu_ticks = float(fields[13]) + float(fields[14])
            page_size = os.sysconf("SC_PAGE_SIZE")
            rss_bytes = int(fields[23]) * page_size
            thread_count = int(fields[19])
            ticks_per_second = os.sysconf("SC_CLK_TCK")
            return self._finish(cpu_ticks / ticks_per_second, rss_bytes, thread_count)
        except (OSError, ValueError, IndexError):
            if not status_path.exists():
                return None
            return None

    def _finish(self, cpu_seconds: float, working_set: int, threads: int) -> ProcessSample:
        now = time.monotonic()
        elapsed = max(now - self._previous_time, 1e-6)
        cpu_delta = 0.0 if self._previous_cpu is None else max(cpu_seconds - self._previous_cpu, 0.0)
        cpu_percent = cpu_delta / elapsed / max(os.cpu_count() or 1, 1) * 100.0
        self._previous_cpu = cpu_seconds
        self._previous_time = now
        return ProcessSample(cpu_percent, working_set, threads)


class ProcessGroupCollector:
    """Sample multiple owned processes with one OS query per interval.
    每个采样周期只执行一次系统查询，避免进程数放大控制器开销。
    """

    def __init__(self, processes: dict[str, int]):
        self._collectors = {name: ProcessCollector(pid) for name, pid in processes.items()}

    def sample(self) -> dict[str, ProcessSample]:
        if os.name != "nt":
            return {
                name: sample
                for name, collector in self._collectors.items()
                if (sample := collector.sample()) is not None
            }
        if not self._collectors:
            return {}
        pids = ",".join(str(collector.pid) for collector in self._collectors.values())
        command = (
            f"Get-Process -Id {pids} | "
            "Select-Object Id,CPU,WorkingSet64,@{Name='ThreadCount';Expression={$_.Threads.Count}} | "
            "ConvertTo-Json -Compress"
        )
        try:
            result = subprocess.run(
                ["powershell", "-NoProfile", "-NonInteractive", "-Command", command],
                capture_output=True,
                text=True,
                timeout=3,
                check=True,
            )
            rows = json.loads(result.stdout)
            if isinstance(rows, dict):
                rows = [rows]
            by_pid = {int(row["Id"]): row for row in rows}
        except (OSError, ValueError, subprocess.SubprocessError, json.JSONDecodeError, KeyError):
            return {}
        samples: dict[str, ProcessSample] = {}
        for name, collector in self._collectors.items():
            row = by_pid.get(collector.pid)
            if row is None:
                continue
            samples[name] = collector._finish(
                float(row.get("CPU") or 0.0),
                int(row.get("WorkingSet64") or 0),
                int(row.get("ThreadCount") or 0),
            )
        return samples

