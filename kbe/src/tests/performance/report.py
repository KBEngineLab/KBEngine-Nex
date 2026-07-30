"""Aggregate raw metrics into machine and human reports. / 将原始指标聚合为机器和人工报告。"""

from __future__ import annotations

import json
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any, Iterable

from .metrics import LatencyHistogram


def load_events(path: Path) -> Iterable[dict[str, Any]]:
    with path.open("r", encoding="utf-8") as stream:
        for line_number, line in enumerate(stream, 1):
            if not line.strip():
                continue
            try:
                event = json.loads(line)
            except json.JSONDecodeError as exc:
                raise ValueError(f"invalid JSONL at line {line_number}: {exc}") from exc
            validate_event(event)
            yield event


def validate_event(event: dict[str, Any]) -> None:
    required = {"schema_version", "run_id", "scenario", "timestamp", "component", "instance", "metric", "value"}
    missing = required.difference(event)
    if missing:
        raise ValueError(f"missing metric fields: {sorted(missing)}")
    if event["schema_version"] != 1:
        raise ValueError(f"unsupported metric schema: {event['schema_version']}")


def build_summary(events: Iterable[dict[str, Any]]) -> dict[str, Any]:
    latency: dict[str, LatencyHistogram] = defaultdict(LatencyHistogram)
    counters: Counter[str] = Counter()
    gauges: dict[str, float] = {}
    metadata: dict[str, str] = {}
    for event in events:
        metric = str(event["metric"])
        tags = event.get("tags") or {}
        operation = str(tags.get("operation", "default"))
        if metric == "request.latency":
            latency[operation].observe(float(event["value"]))
        elif metric.startswith("log.") or tags.get("kind") == "counter":
            counters[metric] += int(float(event["value"]))
        elif isinstance(event["value"], (int, float)):
            gauges[f"{event['component']}.{metric}"] = float(event["value"])
        metadata.setdefault("run_id", str(event["run_id"]))
        metadata.setdefault("scenario", str(event["scenario"]))
    success = counters.get("request.success.count", 0)
    failures = counters.get("request.error.count", 0) + counters.get("request.timeout.count", 0)
    total_requests = success + failures
    return {
        **metadata,
        "latency": {operation: histogram.summary() for operation, histogram in sorted(latency.items())},
        "counters": dict(sorted(counters.items())),
        "gauges": dict(sorted(gauges.items())),
        "requests": {
            "total": total_requests,
            "success": success,
            "failures": failures,
            "success_rate_percent": (success / total_requests * 100.0) if total_requests else 0.0,
        },
    }


def write_report(summary: dict[str, Any], json_path: Path, markdown_path: Path) -> None:
    json_path.parent.mkdir(parents=True, exist_ok=True)
    json_path.write_text(json.dumps(summary, ensure_ascii=True, indent=2) + "\n", encoding="utf-8")
    lines = [
        "# KBE Performance Report",
        "",
        f"- Run: `{summary.get('run_id', '')}`",
        f"- Scenario: `{summary.get('scenario', '')}`",
        "",
        "## Request Result",
        "",
        "| Total | Success | Failures | Success rate |",
        "| ---: | ---: | ---: | ---: |",
        "| {total} | {success} | {failures} | {success_rate_percent:.3f}% |".format(**summary.get("requests", {})),
        "",
        "## Latency",
        "",
        "| Operation | Count | P50 (ms) | P95 (ms) | P99 (ms) | P99.9 (ms) | Max (ms) |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for operation, values in summary.get("latency", {}).items():
        lines.append(
            "| {0} | {count} | {p50_ms:.2f} | {p95_ms:.2f} | {p99_ms:.2f} | {p999_ms:.2f} | {max_ms:.2f} |".format(operation, **values)
        )
    lines.extend(["", "## Counters", "", "| Metric | Value |", "| --- | ---: |"])
    for metric, value in summary.get("counters", {}).items():
        lines.append(f"| `{metric}` | {value} |")
    lines.extend(["", "## Gauges", "", "| Metric | Value |", "| --- | ---: |"])
    for metric, value in summary.get("gauges", {}).items():
        lines.append(f"| `{metric}` | {value:.2f} |")
    markdown_path.parent.mkdir(parents=True, exist_ok=True)
    markdown_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
