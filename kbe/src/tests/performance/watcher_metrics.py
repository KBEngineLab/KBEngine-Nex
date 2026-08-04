"""External Watcher sampler. / 外部 Watcher 采样器。"""

from __future__ import annotations

import math
import json
import re
import socket
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


def resolve_target(
    target: WatcherTarget,
    log_roots: list[Path],
    machine_resolver: MachineEndpointResolver | None = None,
) -> WatcherTarget:
    """Resolve a component's ephemeral internal endpoint from owned logs.
    从本轮自有日志解析组件的临时内部端点。
    """
    if not target.component_name:
        return target
    component_name, separator, component_id = target.component_name.partition("#")
    if separator and (not component_id or not component_id.isdigit()):
        raise ValueError(f"invalid component selector: {target.component_name}")
    component_id_pattern = (
        rf"componentID:{re.escape(component_id)}(?:,|\s)" if separator else ""
    )

    # The cluster manifest is structured and remains available when INFO logs
    # are disabled. It owns server endpoints but not separately started Bots.
    # 集群清单不受 INFO 日志级别影响，优先用于服务端组件；独立 Bots 再走
    # 日志或 Machine 发现。
    for root in log_roots:
        manifest_path = root.parent / "components.json"
        if not manifest_path.is_file():
            continue
        try:
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
        for item in manifest:
            if str(item.get("name", "")).lower() != component_name.lower():
                continue
            if separator and int(item.get("component_id", 0)) != int(component_id):
                continue
            host = str(item.get("intaddr", ""))
            port = int(item.get("intport", 0))
            if host and port > 0:
                return WatcherTarget(
                    target.component_type, host, port, target.path, target.component_name,
                )
    pattern = re.compile(
        rf"componentType:{re.escape(component_name)}(?:,|\s).*?"
        rf"{component_id_pattern}.*?intaddr:([^,]+), intport:(\d+)",
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
    if machine_resolver is not None:
        return machine_resolver.resolve(target)
    raise LookupError(f"component endpoint is not available yet: {target.component_name}")


class MachineEndpointResolver:
    """Cache KBMachine discovery on one stable UDP reply port.
    在固定 UDP 回复端口上缓存 KBMachine 组件发现结果。

    Machine refreshes component health synchronously for a new request key. Reusing
    one socket lets its duplicate-request cache serve retries without repeatedly
    scanning every process and turning readiness polling into server load.
    Machine 会为新的请求键同步刷新组件健康状态。复用同一 socket 可命中其重复
    请求缓存，避免 readiness 轮询反复扫描全部进程并制造额外服务端负载。
    """

    def __init__(self, tools_root: Path):
        self.tools_root = tools_root
        self._machines: Any | None = None
        self._define: Any | None = None
        self._machines_module: Any | None = None
        self._message_stream: Any | None = None
        self._cache: dict[tuple[str, int | None], tuple[str, int]] = {}

    def _initialize(self) -> None:
        if self._machines is not None:
            return
        sys.path.insert(0, str(self.tools_root))
        try:
            from pycommon import Define, Machines, MessageStream

            self._define = Define
            self._machines_module = Machines
            self._message_stream = MessageStream
            self._machines = Machines.Machines()
        finally:
            sys.path.pop(0)

    def _index_discovered_components(self) -> None:
        assert self._machines is not None and self._define is not None
        for component_type, infos in self._machines.interfaces.items():
            name = str(self._define.COMPONENT_NAME[int(component_type)]).lower()
            for info in infos:
                endpoint = (str(info.intaddr), int(info.intport))
                self._cache[(name, int(info.componentID))] = endpoint
                self._cache.setdefault((name, None), endpoint)

    def _refresh(
        self,
        desired: set[tuple[str, int | None]],
        deadline: float | None = None,
    ) -> None:
        self._initialize()
        assert self._machines is not None
        assert self._machines_module is not None and self._message_stream is not None

        message = self._message_stream.MessageStreamWriter(
            self._machines_module.MachineInterface_onQueryAllInterfaceInfos
        )
        message.writeInt32(self._machines.uid)
        message.writeString(self._machines.username)
        message.writeUint16(socket.htons(self._machines.replyPort))
        def on_response(data: bytes, _address: tuple[str, int]) -> bool:
            self._machines.parseQueryData(data)
            self._index_discovered_components()
            return desired.issubset(self._cache) or (
                deadline is not None and time.monotonic() >= deadline
            )

        # Stop as soon as the desired component arrives. Machines.queryAllInterfaces
        # waits for UDP silence, which is unbounded while a large response stream is
        # active and can defeat the runner's outer readiness timeout.
        # 找到目标组件后立即停止；Machines.queryAllInterfaces 会等待 UDP 静默，
        # 大响应流下时长无上限，会使运行器外层 readiness 超时失效。
        remaining = 0.5 if deadline is None else max(deadline - time.monotonic(), 0.01)
        self._machines.sendAndReceive(
            message.build(), "<broadcast>", trycount=0,
            timeout=min(0.5, remaining), callback=on_response,
        )

    @staticmethod
    def _target_key(target: WatcherTarget) -> tuple[str, int | None]:
        component_name, separator, component_id = target.component_name.partition("#")
        return component_name.lower(), int(component_id) if separator else None

    def resolve_many(
        self,
        targets: list[WatcherTarget],
        deadline: float | None = None,
    ) -> list[WatcherTarget]:
        """Resolve all dynamic endpoints with at most one Machine broadcast.
        最多通过一次 Machine 广播解析本批全部动态端点。

        A high-process-count run may contain dozens of Bots component IDs. Querying
        each ID separately multiplies Machine work and can consume the entire
        readiness window before the first Watcher request is sent.
        高进程数压测会包含数十个 Bots 组件 ID；逐 ID 广播会放大 Machine 工作量，
        甚至在首次 Watcher 请求前耗尽整个 readiness 窗口。
        """
        keys = [self._target_key(target) for target in targets]
        missing = {key for key in keys if key not in self._cache}
        if missing:
            if deadline is not None and time.monotonic() >= deadline:
                raise TimeoutError("component endpoint discovery deadline exhausted")
            self._refresh(missing, deadline)

        resolved: list[WatcherTarget] = []
        for target, key in zip(targets, keys):
            endpoint = self._cache.get(key)
            if endpoint is None:
                raise LookupError(
                    f"component endpoint is not registered with Machine: {target.component_name}"
                )
            resolved.append(WatcherTarget(
                target.component_type,
                endpoint[0],
                endpoint[1],
                target.path,
                target.component_name,
            ))
        return resolved

    def resolve(self, target: WatcherTarget) -> WatcherTarget:
        return self.resolve_many([target])[0]

    def close(self) -> None:
        if self._machines is not None:
            self._machines.stopListen()
            self._machines = None
        self._cache = {}


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
        self._phase_offsets: dict[tuple[str, str, int, str], float] = {}
        targets_by_interval: dict[float, list[WatcherTarget]] = {}
        for target in targets:
            targets_by_interval.setdefault(
                self._intervals[_watcher_target_identity(target)], []
            ).append(target)

        for interval, interval_targets in targets_by_interval.items():
            # The runner can only dispatch on its base sampling cadence. Distribute
            # slower targets over those observable slots so a five-second target set
            # does not synchronously interrupt every server on the same tick.
            # Runner 只能在基础采样周期上调度。将低频目标分散到可观测秒槽，避免
            # 五秒目录在同一 Tick 同步打断全部服务进程。
            slot_count = max(1, int(math.floor(interval / default_interval + 1e-9)))
            for index, target in enumerate(interval_targets):
                self._phase_offsets[_watcher_target_identity(target)] = (
                    index % slot_count
                ) * default_interval
        self._next_due: dict[tuple[str, str, int, str], float] = {}

    def start(self, now: float | None = None) -> None:
        """Anchor deterministic phase offsets at measurement start.
        在测量开始时锚定确定性的错峰偏移。

        Readiness queries deliberately do not use this schedule. Keeping phase
        initialization explicit prevents startup duration from changing which slot a
        target receives and makes repeated A/B runs comparable.
        Readiness 查询不使用该调度器。显式初始化可避免启动耗时改变目标槽位，
        保证多轮 A/B 压测使用相同采样相位。
        """
        current = time.monotonic() if now is None else now
        for key, offset in self._phase_offsets.items():
            self._next_due.setdefault(key, current + offset)

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

    def phase_offset(self, target: WatcherTarget) -> float:
        """Return the measurement phase offset. / 返回测量阶段的错峰偏移。"""
        return self._phase_offsets[_watcher_target_identity(target)]


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
        self._watchers: dict[tuple[str, str, int, str], Any] = {}
        self._last_stats: dict[tuple[str, str, int, str], WatcherQueryStats] = {}
        self._machine_resolver = MachineEndpointResolver(tools_root)

    def resolve_target(self, target: WatcherTarget, log_roots: list[Path]) -> WatcherTarget:
        """Resolve through manifest/logs, then one cached Machine session.
        依次通过清单、日志和单个缓存 Machine 会话解析端点。
        """
        return resolve_target(target, log_roots, self._machine_resolver)

    def resolve_targets(
        self,
        targets: list[WatcherTarget],
        log_roots: list[Path],
        deadline: float | None = None,
    ) -> list[WatcherTarget]:
        """Resolve manifests/logs first, then batch unresolved Machine targets.
        优先解析清单和日志，再批量发现剩余的 Machine 目标。
        """
        endpoints: dict[tuple[str, int | None], tuple[str, int]] = {}
        manifest_paths = {
            root.parent / "components.json" for root in log_roots
            if (root.parent / "components.json").is_file()
        }
        for path in manifest_paths:
            if deadline is not None and time.monotonic() >= deadline:
                raise TimeoutError("component endpoint discovery deadline exhausted")
            try:
                manifest = json.loads(path.read_text(encoding="utf-8"))
            except (OSError, ValueError):
                continue
            for item in manifest:
                name = str(item.get("name", "")).lower()
                host = str(item.get("intaddr", ""))
                port = int(item.get("intport", 0))
                component_id = int(item.get("component_id", 0))
                if name and host and port > 0:
                    endpoints[(name, component_id)] = (host, port)
                    endpoints.setdefault((name, None), (host, port))

        # Runtime logs are indexed once for the whole target set. Bots commonly
        # run with WARN logging, so missing entries intentionally fall through to
        # the single Machine broadcast below.
        # 本轮日志只建立一次索引；Bots 常以 WARN 级别运行，没有端点日志时统一
        # 交给下方的一次 Machine 广播发现。
        endpoint_pattern = re.compile(
            r"componentType:([^,\s]+)(?:,|\s).*?componentID:(\d+)(?:,|\s).*?"
            r"intaddr:([^,]+), intport:(\d+)",
            re.IGNORECASE,
        )
        scanned_logs: set[Path] = set()
        for root in log_roots:
            if not root.exists():
                continue
            for path in root.rglob("*.log"):
                if path in scanned_logs:
                    continue
                scanned_logs.add(path)
                if deadline is not None and time.monotonic() >= deadline:
                    raise TimeoutError("component endpoint discovery deadline exhausted")
                try:
                    content = path.read_text(encoding="utf-8", errors="replace")
                except OSError:
                    continue
                for match in endpoint_pattern.finditer(content):
                    name = match.group(1).lower()
                    endpoint = (match.group(3).strip(), int(match.group(4)))
                    endpoints[(name, int(match.group(2)))] = endpoint
                    endpoints.setdefault((name, None), endpoint)

        resolved: list[WatcherTarget | None] = [None] * len(targets)
        unresolved: list[WatcherTarget] = []
        unresolved_indexes: list[int] = []
        for index, target in enumerate(targets):
            if not target.component_name:
                resolved[index] = target
                continue
            component_name, separator, component_id = target.component_name.partition("#")
            if separator and (not component_id or not component_id.isdigit()):
                raise ValueError(f"invalid component selector: {target.component_name}")
            key = (
                component_name.lower(),
                int(component_id) if separator and component_id.isdigit() else None,
            )
            endpoint = endpoints.get(key)
            if endpoint is not None:
                resolved[index] = WatcherTarget(
                    target.component_type, endpoint[0], endpoint[1], target.path,
                    target.component_name,
                )
            else:
                unresolved.append(target)
                unresolved_indexes.append(index)

        if unresolved:
            dynamic = self._machine_resolver.resolve_many(unresolved, deadline)
            for index, target in zip(unresolved_indexes, dynamic):
                resolved[index] = target
        if any(target is None for target in resolved):
            raise LookupError("one or more component endpoints could not be resolved")
        return [target for target in resolved if target is not None]

    def _get_watcher(self, target: WatcherTarget) -> tuple[Any, bool]:
        """Return a connected Watcher for one endpoint, creating it lazily.
        按端点复用控制连接，减少每次采样的 TCP 建连和调度抖动。
        """
        sys.path.insert(0, str(self.tools_root))
        try:
            from pycommon import Define, Watcher

            key = (target.component_type, target.host, target.port, target.path)
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

    def query(
        self,
        target: WatcherTarget,
        timeout_seconds: float | None = None,
    ) -> dict[str, Any]:
        """Query one snapshot without exceeding an optional caller deadline.
        查询一次快照，并允许调用方用剩余全局预算收紧单次超时。
        """
        key = (target.component_type, target.host, target.port, target.path)
        watcher, connection_reused = self._get_watcher(target)
        try:
            if hasattr(watcher, "clearWatchData"):
                watcher.clearWatchData()
            watcher.requireQueryWatcher(target.path)
            effective_timeout = self.timeout_seconds
            if timeout_seconds is not None:
                effective_timeout = min(effective_timeout, max(float(timeout_seconds), 0.01))
            deadline = time.monotonic() + effective_timeout
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
        return self._last_stats.get((target.component_type, target.host, target.port, target.path))

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
        self._machine_resolver.close()


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

