"""External Watcher sampler. / 外部 Watcher 采样器。"""

from __future__ import annotations

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


def parse_target(value: str) -> WatcherTarget:
    """Parse TYPE=HOST:PORT:PATH. / 解析 TYPE=HOST:PORT:PATH。"""
    try:
        component_type, endpoint = value.split("=", 1)
        host, port, path = endpoint.split(":", 2)
        return WatcherTarget(component_type, host, int(port), path)
    except (ValueError, TypeError) as exc:
        raise ValueError("watcher target must be TYPE=HOST:PORT:PATH") from exc


class WatcherCollector:
    def __init__(self, tools_root: Path, timeout_seconds: float = 5.0):
        self.tools_root = tools_root
        self.timeout_seconds = timeout_seconds

    def query(self, target: WatcherTarget) -> dict[str, Any]:
        """Query one snapshot outside the server process. / 在服务进程外查询一次快照。"""
        sys.path.insert(0, str(self.tools_root))
        watcher = None
        try:
            from pycommon import Define, Watcher

            component_type = getattr(Define, target.component_type)
            watcher = Watcher.Watcher(component_type)
            watcher.connect(target.host, target.port)
            watcher.requireQueryWatcher(target.path)
            deadline = time.monotonic() + self.timeout_seconds
            while time.monotonic() < deadline and not watcher.watchData:
                watcher.processOne(min(0.25, max(deadline - time.monotonic(), 0.01)))
            if not watcher.watchData:
                raise TimeoutError(f"watcher query timed out: {target.path}")
            return flatten_values(watcher.watchData[0].get("values", {}))
        finally:
            if watcher is not None:
                watcher.close()
            sys.path.pop(0)


def flatten_values(values: dict[str, Any], prefix: str = "") -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in values.items():
        name = f"{prefix}/{key}" if prefix else str(key)
        if isinstance(value, dict):
            result.update(flatten_values(value, name))
        else:
            result[name] = value
    return result

