"""Low-overhead metric recording primitives. / 低开销指标记录基础设施。"""

from __future__ import annotations

import json
import random
import time
from dataclasses import asdict, dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, TextIO

from . import SCHEMA_VERSION


def utc_timestamp() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds")


@dataclass
class MetricEvent:
    """One append-only observation. / 一条追加式观测记录。"""

    run_id: str
    scenario: str
    timestamp: str
    component: str
    instance: str
    metric: str
    value: float | int | str
    unit: str = ""
    tags: dict[str, str] = field(default_factory=dict)
    schema_version: int = SCHEMA_VERSION

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


class JsonlRecorder:
    """Writes compact JSONL without touching engine threads. / 写入紧凑 JSONL，不接触引擎线程。"""

    def __init__(self, path: Path, run_id: str, scenario: str):
        self.path = path
        self.run_id = run_id
        self.scenario = scenario
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self._stream: TextIO = path.open("w", encoding="utf-8", newline="\n")

    def record(
        self,
        component: str,
        instance: str,
        metric: str,
        value: float | int | str,
        unit: str = "",
        tags: dict[str, str] | None = None,
    ) -> None:
        event = MetricEvent(
            self.run_id,
            self.scenario,
            utc_timestamp(),
            component,
            instance,
            metric,
            value,
            unit,
            tags or {},
        )
        self._stream.write(json.dumps(event.to_dict(), ensure_ascii=True, separators=(",", ":")))
        self._stream.write("\n")

    def flush(self) -> None:
        self._stream.flush()

    def record_request_latency(
        self,
        component: str,
        instance: str,
        operation: str,
        started_ms: float,
        completed_ms: float,
        success: bool,
        error_code: str = "",
        slow_threshold_ms: float | None = None,
    ) -> None:
        """Record one request outcome and latency. / 记录一次请求结果和延迟。"""
        latency = max(completed_ms - started_ms, 0.0)
        tags = {"operation": operation}
        if error_code:
            tags["error_code"] = error_code
        self.record(component, instance, "request.latency", latency, "ms", tags)
        result_metric = "request.success.count" if success else "request.error.count"
        self.record(component, instance, result_metric, 1, "count", {"kind": "counter", **tags})
        if slow_threshold_ms is not None and latency > slow_threshold_ms:
            self.record(component, instance, "request.slow.count", 1, "count", {"kind": "counter", **tags})

    def record_sample(
        self,
        component: str,
        instance: str,
        metric: str,
        value: float | int,
        unit: str = "",
        tags: dict[str, str] | None = None,
    ) -> None:
        """Record a time-series sample without treating it as a final gauge.
        将时间序列样本写入事件流，避免报告只保留最后一次采样值。
        """
        sample_tags = {"kind": "sample", **(tags or {})}
        self.record(component, instance, metric, value, unit, sample_tags)

    def close(self) -> None:
        self._stream.close()

    def __enter__(self) -> "JsonlRecorder":
        return self

    def __exit__(self, *_: object) -> None:
        self.close()


class LatencyHistogram:
    """Bounded reservoir for percentile estimates. / 有界采样池，用于估算分位数。"""

    def __init__(self, max_samples: int = 100_000):
        if max_samples < 100:
            raise ValueError("max_samples must be at least 100")
        self.max_samples = max_samples
        self._samples: list[float] = []
        self.count = 0
        self.total = 0.0

    def observe(self, milliseconds: float) -> None:
        if milliseconds < 0:
            raise ValueError("latency cannot be negative")
        self.count += 1
        self.total += milliseconds
        if len(self._samples) < self.max_samples:
            self._samples.append(milliseconds)
            return
        index = random.randrange(self.count)
        if index < self.max_samples:
            self._samples[index] = milliseconds

    def percentile(self, percentile: float) -> float:
        if not self._samples:
            return 0.0
        if not 0 <= percentile <= 100:
            raise ValueError("percentile must be between 0 and 100")
        ordered = sorted(self._samples)
        rank = (len(ordered) - 1) * percentile / 100.0
        lower = int(rank)
        upper = min(lower + 1, len(ordered) - 1)
        return ordered[lower] + (ordered[upper] - ordered[lower]) * (rank - lower)

    def summary(self) -> dict[str, float | int]:
        return {
            "count": self.count,
            "sampled": len(self._samples),
            "p50_ms": self.percentile(50),
            "p95_ms": self.percentile(95),
            "p99_ms": self.percentile(99),
            "p999_ms": self.percentile(99.9),
            "max_ms": max(self._samples, default=0.0),
            "mean_ms": self.total / self.count if self.count else 0.0,
            "total_ms": self.total,
        }

    def distribution(self) -> dict[str, float | int]:
        """Return a unit-neutral distribution for CPU, bytes, counts, or time.
        返回可用于 CPU、字节、计数或时间的单位无关分布。
        """
        return {
            "count": self.count,
            "sampled": len(self._samples),
            "p50": self.percentile(50),
            "p95": self.percentile(95),
            "p99": self.percentile(99),
            "p999": self.percentile(99.9),
            "min": min(self._samples, default=0.0),
            "max": max(self._samples, default=0.0),
            "mean": self.total / self.count if self.count else 0.0,
            "total": self.total,
        }


def monotonic_milliseconds() -> float:
    return time.monotonic_ns() / 1_000_000.0
