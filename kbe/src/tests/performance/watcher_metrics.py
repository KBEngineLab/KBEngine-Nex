"""External Watcher sampler. / 外部 Watcher 采样器。"""

from __future__ import annotations

import math
import json
import re
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any


@dataclass(frozen=True, slots=True)
class WatcherTarget:
    component_type: str
    host: str
    port: int
    path: str
    component_name: str = ""


def parse_target(value: str) -> WatcherTarget:
    """Parse TYPE=HOST:PORT:PATH or TYPE=@COMPONENT:PATH.
    解析 TYPE=HOST:PORT:PATH 或 TYPE=@COMPONENT:PATH。
    """
    try:
        component_type, endpoint = value.split("=", 1)
        if endpoint.startswith("@"):
            component_name, path = endpoint[1:].split(":", 1)
            if not component_name or not path:
                raise ValueError
            return WatcherTarget(component_type, "", 0, path, component_name)
        host, port, path = endpoint.split(":", 2)
        return WatcherTarget(component_type, host, int(port), path)
    except (ValueError, TypeError) as exc:
        raise ValueError("watcher target must be TYPE=HOST:PORT:PATH or TYPE=@COMPONENT:PATH") from exc


def resolve_target(target: WatcherTarget, log_roots: list[Path]) -> WatcherTarget:
    """Resolve a component's ephemeral internal endpoint from owned logs.
    从本轮自有日志解析组件的临时内部端点。
    """
    if not target.component_name:
        return target
    pattern = re.compile(
        rf"componentType:{re.escape(target.component_name)}(?:,|\s).*?intaddr:([^,]+), intport:(\d+)",
        re.IGNORECASE,
    )
    for root in log_roots:
        if not root.exists():
            continue
        for path in root.rglob("*.log"):
            try:
                match = pattern.search(path.read_text(encoding="utf-8", errors="replace"))
            except OSError:
                continue
            if match:
                return WatcherTarget(
                    target.component_type,
                    match.group(1).strip(),
                    int(match.group(2)),
                    target.path,
                    target.component_name,
                )
    raise LookupError(f"component endpoint is not available yet: {target.component_name}")


def watcher_target_key(target: WatcherTarget) -> str:
    """Return the stable scenario key for a target, independent of its endpoint.
    返回与临时端点无关的稳定场景键，供采样周期配置使用。
    """
    return f"{target.component_type}:{target.path}"


class WatcherSchedule:
    """Schedule independent steady-state sampling periods for Watcher targets.
    为不同 Watcher 目标维护独立的稳态采样周期。
    """

    def __init__(
        self,
        targets: list[WatcherTarget],
        default_interval_seconds: float,
        interval_overrides: dict[str, Any] | None = None,
    ):
        default_interval = _validate_interval(default_interval_seconds, "default")
        overrides = dict(interval_overrides or {})
        known_keys = {watcher_target_key(target) for target in targets}
        unknown_keys = sorted(set(overrides) - known_keys)
        if unknown_keys:
            raise ValueError(f"watcher interval has unknown target(s): {', '.join(unknown_keys)}")
        self._intervals = {
            _watcher_target_identity(target): _validate_interval(
                overrides.get(watcher_target_key(target), default_interval),
                watcher_target_key(target),
            )
            for target in targets
        }
        self._next_due: dict[tuple[str, str, int, str], float] = {}

    def due(self, target: WatcherTarget, now: float | None = None) -> bool:
        """Return whether target may be sampled at the supplied monotonic time.
        判断目标在给定单调时钟时刻是否到达采样时间。
        """
        key = _watcher_target_identity(target)
        current = time.monotonic() if now is None else now
        tolerance = min(self._intervals[key] * 0.1, 0.1)
        return current + tolerance >= self._next_due.get(key, float("-inf"))

    def mark_sampled(self, target: WatcherTarget, now: float | None = None) -> None:
        """Set the next deadline after a completed query, including failures.
        查询完成后设置下一次截止时间，失败查询也不能立即重试形成控制面风暴。
        """
        key = _watcher_target_identity(target)
        current = time.monotonic() if now is None else now
        interval = self._intervals[key]
        deadline = self._next_due.get(key)
        if deadline is None:
            self._next_due[key] = current + interval
            return
        next_deadline = deadline + interval
        if next_deadline <= current:
            skipped = math.floor((current - next_deadline) / interval) + 1
            next_deadline += skipped * interval
        self._next_due[key] = next_deadline

    def interval(self, target: WatcherTarget) -> float:
        """Return the effective interval for one target. / 返回目标的实际采样周期。"""
        return self._intervals[_watcher_target_identity(target)]


def _watcher_target_identity(target: WatcherTarget) -> tuple[str, str, int, str]:
    if target.component_name:
        # Discovered ports are ephemeral; the component name is the stable identity
        # across pre-start configuration and post-readiness resolution.
        # 动态端口每轮都会变化；组件名才是启动前配置与就绪后解析之间的稳定身份。
        return target.component_type, target.component_name, 0, target.path
    return target.component_type, target.host, target.port, target.path


def _validate_interval(value: Any, name: str) -> float:
    try:
        interval = float(value)
    except (TypeError, ValueError) as exc:
        raise ValueError(f"watcher interval must be a finite number: {name}") from exc
    if not math.isfinite(interval) or interval < 0.1:
        raise ValueError(f"watcher interval must be >= 0.1 seconds: {name}")
    return interval


@dataclass(frozen=True, slots=True)
class WatcherQueryStats:
    """Metadata about the last response, separate from Watcher values.
    保存最近一次响应的元数据，与业务 Watcher 值分离，避免污染指标树。
    """

    value_count: int
    response_bytes_estimated: int
    connection_reused: bool


class WatcherCollector:
    def __init__(self, tools_root: Path, timeout_seconds: float = 5.0):
        self.tools_root = tools_root
        self.timeout_seconds = timeout_seconds
        self._watchers: dict[tuple[str, str, int], Any] = {}
        self._last_stats: dict[tuple[str, str, int], WatcherQueryStats] = {}

    def _get_watcher(self, target: WatcherTarget) -> tuple[Any, bool]:
        """Return a connected Watcher for one endpoint, creating it lazily.
        按端点复用控制连接，减少每次采样的 TCP 建连和调度抖动。
        """
        sys.path.insert(0, str(self.tools_root))
        try:
            from pycommon import Define, Watcher

            key = (target.component_type, target.host, target.port)
            watcher = self._watchers.get(key)
            if watcher is None:
                component_type = getattr(Define, target.component_type)
                watcher = Watcher.Watcher(component_type)
                watcher.connect(target.host, target.port)
                self._watchers[key] = watcher
                return watcher, False
            return watcher, True
        finally:
            sys.path.pop(0)

    def query(self, target: WatcherTarget) -> dict[str, Any]:
        """Query one snapshot outside the server process. / 在服务进程外查询一次快照。"""
        key = (target.component_type, target.host, target.port)
        watcher, connection_reused = self._get_watcher(target)
        try:
            if hasattr(watcher, "clearWatchData"):
                watcher.clearWatchData()
            watcher.requireQueryWatcher(target.path)
            deadline = time.monotonic() + self.timeout_seconds
            while time.monotonic() < deadline and not watcher.watchData:
                watcher.processOne(min(0.25, max(deadline - time.monotonic(), 0.01)))
            if not watcher.watchData:
                raise TimeoutError(f"watcher query timed out: {target.path}")
            values = flatten_values(watcher.watchData[0].get("values", {}))
            self._last_stats[key] = WatcherQueryStats(
                len(values),
                estimate_response_bytes(watcher.watchData[0]),
                connection_reused,
            )
            return values
        except Exception:
            # A broken socket must not poison later samples; the next query reconnects.
            # 失效连接立即淘汰，下一次查询自动重连，避免错误状态持续污染采样。
            self._watchers.pop(key, None)
            try:
                watcher.close()
            except (AttributeError, OSError):
                pass
            raise

    def last_query_stats(self, target: WatcherTarget) -> WatcherQueryStats | None:
        """Return metadata for the last successful target query.
        返回目标最近一次成功查询的响应元数据。
        """
        return self._last_stats.get((target.component_type, target.host, target.port))

    def close(self) -> None:
        """Close all cached control connections owned by this collector.
        统一释放本轮采集器持有的控制连接。
        """
        watchers, self._watchers = self._watchers, {}
        for watcher in watchers.values():
            try:
                watcher.close()
            except (AttributeError, OSError):
                pass


def estimate_response_bytes(value: Any) -> int:
    """Estimate JSON-equivalent response size for a protocol-neutral metric.
    估算 JSON 等价响应大小，仅用于趋势比较，不宣称为线协议字节数。
    """
    try:
        return len(json.dumps(value, ensure_ascii=True, separators=(",", ":")).encode("utf-8"))
    except (TypeError, ValueError):
        return len(repr(value).encode("utf-8", errors="replace"))


def flatten_values(values: dict[str, Any], prefix: str = "") -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in values.items():
        name = f"{prefix}/{key}" if prefix else str(key)
        if isinstance(value, dict):
            result.update(flatten_values(value, name))
        else:
            result[name] = value
    return result

