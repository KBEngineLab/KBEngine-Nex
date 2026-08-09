"""Portable process sampling with standard-library fallbacks. / 使用标准库实现可移植进程采样。"""

from __future__ import annotations

import os
import time
import ctypes
from dataclasses import dataclass
from pathlib import Path


@dataclass
class ProcessSample:
    cpu_percent: float
    working_set_bytes: int
    private_bytes: int | None
    peak_working_set_bytes: int | None
    thread_count: int
    handle_count: int | None


if os.name == "nt":
    from ctypes import wintypes

    class _ProcessMemoryCountersEx(ctypes.Structure):
        _fields_ = [
            ("cb", wintypes.DWORD),
            ("PageFaultCount", wintypes.DWORD),
            ("PeakWorkingSetSize", ctypes.c_size_t),
            ("WorkingSetSize", ctypes.c_size_t),
            ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
            ("QuotaPagedPoolUsage", ctypes.c_size_t),
            ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
            ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
            ("PagefileUsage", ctypes.c_size_t),
            ("PeakPagefileUsage", ctypes.c_size_t),
            ("PrivateUsage", ctypes.c_size_t),
        ]


    class _ThreadEntry32(ctypes.Structure):
        _fields_ = [
            ("dwSize", wintypes.DWORD),
            ("cntUsage", wintypes.DWORD),
            ("th32ThreadID", wintypes.DWORD),
            ("th32OwnerProcessID", wintypes.DWORD),
            ("tpBasePri", wintypes.LONG),
            ("tpDeltaPri", wintypes.LONG),
            ("dwFlags", wintypes.DWORD),
        ]


    _kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    _psapi = ctypes.WinDLL("psapi", use_last_error=True)
    _kernel32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    _kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    _kernel32.Thread32First.argtypes = [wintypes.HANDLE, ctypes.POINTER(_ThreadEntry32)]
    _kernel32.Thread32First.restype = wintypes.BOOL
    _kernel32.Thread32Next.argtypes = [wintypes.HANDLE, ctypes.POINTER(_ThreadEntry32)]
    _kernel32.Thread32Next.restype = wintypes.BOOL
    _kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    _kernel32.OpenProcess.restype = wintypes.HANDLE
    _kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    _kernel32.CloseHandle.restype = wintypes.BOOL
    _kernel32.GetProcessTimes.argtypes = [
        wintypes.HANDLE,
        ctypes.POINTER(wintypes.FILETIME), ctypes.POINTER(wintypes.FILETIME),
        ctypes.POINTER(wintypes.FILETIME), ctypes.POINTER(wintypes.FILETIME),
    ]
    _kernel32.GetProcessTimes.restype = wintypes.BOOL
    _kernel32.GetProcessHandleCount.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
    _kernel32.GetProcessHandleCount.restype = wintypes.BOOL
    _psapi.GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD]
    _psapi.GetProcessMemoryInfo.restype = wintypes.BOOL


def _filetime_seconds(value: wintypes.FILETIME) -> float:
    ticks = (int(value.dwHighDateTime) << 32) | int(value.dwLowDateTime)
    return ticks / 10_000_000.0


def _windows_thread_counts(pids: set[int]) -> dict[int, int]:
    """Count owned threads with one system snapshot for the whole process group.
    通过一次系统线程快照统计整组进程，避免逐进程调用 PowerShell。
    """
    if os.name != "nt" or not pids:
        return {}
    invalid_handle = ctypes.c_void_p(-1).value
    snapshot = _kernel32.CreateToolhelp32Snapshot(0x00000004, 0)
    if snapshot == invalid_handle:
        return {}
    counts = {pid: 0 for pid in pids}
    entry = _ThreadEntry32()
    entry.dwSize = ctypes.sizeof(entry)
    try:
        has_entry = bool(_kernel32.Thread32First(snapshot, ctypes.byref(entry)))
        while has_entry:
            owner = int(entry.th32OwnerProcessID)
            if owner in counts:
                counts[owner] += 1
            has_entry = bool(_kernel32.Thread32Next(snapshot, ctypes.byref(entry)))
    finally:
        _kernel32.CloseHandle(snapshot)
    return counts


def _windows_process_row(pid: int, thread_count: int) -> tuple[float, int, int, int, int, int] | None:
    """Read one process through Win32 APIs without spawning a helper process.
    直接通过 Win32 API 读取进程，避免高负载时创建辅助 PowerShell 进程。
    """
    if os.name != "nt":
        return None
    handle = _kernel32.OpenProcess(0x1000 | 0x0010, False, pid)
    if not handle:
        return None
    creation = wintypes.FILETIME()
    exit_time = wintypes.FILETIME()
    kernel = wintypes.FILETIME()
    user = wintypes.FILETIME()
    memory = _ProcessMemoryCountersEx()
    memory.cb = ctypes.sizeof(memory)
    handle_count = wintypes.DWORD()
    try:
        if not _kernel32.GetProcessTimes(
            handle, ctypes.byref(creation), ctypes.byref(exit_time),
            ctypes.byref(kernel), ctypes.byref(user),
        ):
            return None
        if not _psapi.GetProcessMemoryInfo(handle, ctypes.byref(memory), memory.cb):
            return None
        has_handle_count = _kernel32.GetProcessHandleCount(handle, ctypes.byref(handle_count))
        return (
            _filetime_seconds(kernel) + _filetime_seconds(user),
            int(memory.WorkingSetSize),
            int(memory.PrivateUsage),
            int(memory.PeakWorkingSetSize),
            thread_count,
            int(handle_count.value) if has_handle_count else 0,
        )
    finally:
        _kernel32.CloseHandle(handle)


def _parse_windows_process_row(row: dict[str, object]) -> tuple[float, int, int, int, int, int]:
    """Normalize the stable scalar fields emitted by Get-Process.
    统一 Get-Process 输出的标量字段，避免单进程与多进程 JSON 形态影响采样契约。
    """
    return (
        float(row.get("CPU") or 0.0),
        int(row.get("WorkingSet64") or 0),
        int(row.get("PrivateMemorySize64") or 0),
        int(row.get("PeakWorkingSet64") or 0),
        int(row.get("ThreadCount") or 0),
        int(row.get("Handles") or 0),
    )


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
        thread_counts = _windows_thread_counts({self.pid})
        row = _windows_process_row(self.pid, thread_counts.get(self.pid, 0))
        return self._finish(*row) if row is not None else None

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
            peak_working_set = None
            try:
                status = status_path.read_text(encoding="utf-8")
                for line in status.splitlines():
                    if line.startswith("VmHWM:"):
                        peak_working_set = int(line.split()[1]) * 1024
                        break
            except (OSError, ValueError, IndexError):
                pass

            handle_count = None
            try:
                handle_count = len(os.listdir(f"/proc/{self.pid}/fd"))
            except OSError:
                pass

            # /proc does not expose Windows-style private committed bytes cheaply. Leaving it
            # unavailable is more accurate than labelling RssAnon as an equivalent measurement.
            # /proc 无法低成本提供与 Windows 私有提交量等价的指标，缺省优于用 RssAnon 冒充同口径数据。
            return self._finish(
                cpu_ticks / ticks_per_second,
                rss_bytes,
                None,
                peak_working_set,
                thread_count,
                handle_count,
            )
        except (OSError, ValueError, IndexError):
            if not status_path.exists():
                return None
            return None

    def _finish(
        self,
        cpu_seconds: float,
        working_set: int,
        private_bytes: int | None,
        peak_working_set: int | None,
        threads: int,
        handles: int | None,
    ) -> ProcessSample:
        now = time.monotonic()
        elapsed = max(now - self._previous_time, 1e-6)
        cpu_delta = 0.0 if self._previous_cpu is None else max(cpu_seconds - self._previous_cpu, 0.0)
        cpu_percent = cpu_delta / elapsed / max(os.cpu_count() or 1, 1) * 100.0
        self._previous_cpu = cpu_seconds
        self._previous_time = now
        return ProcessSample(
            cpu_percent,
            working_set,
            private_bytes,
            peak_working_set,
            threads,
            handles,
        )


class ProcessGroupCollector:
    """Sample multiple owned processes with one OS query per interval.
    每个采样周期只执行一次系统查询，避免进程数放大控制器开销。
    """

    def __init__(self, processes: dict[str, int]):
        self._collectors = {name: ProcessCollector(pid) for name, pid in processes.items()}

    def sample(self) -> dict[str, ProcessSample]:
        if os.name != "nt":
            samples: dict[str, ProcessSample] = {}
            for name, collector in self._collectors.items():
                sample = collector.sample()
                if sample is not None:
                    samples[name] = sample
            return samples
        if not self._collectors:
            return {}
        pids = {collector.pid for collector in self._collectors.values()}
        thread_counts = _windows_thread_counts(pids)
        samples: dict[str, ProcessSample] = {}
        for name, collector in self._collectors.items():
            row = _windows_process_row(
                collector.pid, thread_counts.get(collector.pid, 0),
            )
            if row is None:
                continue
            samples[name] = collector._finish(*row)
        return samples
