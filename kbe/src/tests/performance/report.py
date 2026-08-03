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


def build_summary(
    events: Iterable[dict[str, Any]],
    thresholds: dict[str, Any] | None = None,
    readiness: dict[str, Any] | None = None,
    configured_bots: int = 0,
    workload_processes: int = 1,
) -> dict[str, Any]:
    latency: dict[str, LatencyHistogram] = defaultdict(LatencyHistogram)
    samples: dict[str, LatencyHistogram] = defaultdict(LatencyHistogram)
    sample_units: dict[str, str] = {}
    counters: Counter[str] = Counter()
    operation_counters: dict[str, Counter[str]] = defaultdict(Counter)
    phase_counters: dict[str, Counter[str]] = defaultdict(Counter)
    gauges: dict[str, float] = {}
    metadata: dict[str, str] = {}
    for event in events:
        metric = str(event["metric"])
        tags = event.get("tags") or {}
        operation = str(tags.get("operation", "default"))
        if metric == "request.latency":
            latency[operation].observe(float(event["value"]))
        elif tags.get("kind") == "sample":
            sample_key = f"{event['component']}.{event['instance']}.{metric}"
            samples[sample_key].observe(float(event["value"]))
            sample_units.setdefault(sample_key, str(event.get("unit", "")))
        elif metric.startswith("log.") or tags.get("kind") == "counter":
            counters[metric] += int(float(event["value"]))
            if operation != "default":
                operation_counters[operation][metric] += int(float(event["value"]))
            phase = tags.get("phase")
            if phase:
                phase_counters[str(phase)][metric] += int(float(event["value"]))
        elif isinstance(event["value"], (int, float)):
            gauges[f"{event['component']}.{metric}"] = float(event["value"])
        metadata.setdefault("run_id", str(event["run_id"]))
        metadata.setdefault("scenario", str(event["scenario"]))
    success = counters.get("request.success.count", 0)
    failures = counters.get("request.error.count", 0) + counters.get("request.timeout.count", 0)
    total_requests = success + failures
    summary = {
        **metadata,
        "latency": {operation: histogram.summary() for operation, histogram in sorted(latency.items())},
        "samples": {
            name: {**histogram.distribution(), "unit": sample_units.get(name, "")}
            for name, histogram in sorted(samples.items())
        },
        "counters": dict(sorted(counters.items())),
        "operation_counters": {
            operation: dict(sorted(values.items()))
            for operation, values in sorted(operation_counters.items())
        },
        "phase_counters": {
            phase: dict(sorted(values.items()))
            for phase, values in sorted(phase_counters.items())
        },
        "gauges": dict(sorted(gauges.items())),
        "requests": {
            "total": total_requests,
            "success": success,
            "failures": failures,
            "success_rate_percent": (success / total_requests * 100.0) if total_requests else 0.0,
        },
    }
    summary["quality"] = evaluate_quality(
        summary,
        thresholds or {},
        readiness or {},
        configured_bots,
        workload_processes,
    )
    return summary


def evaluate_quality(
    summary: dict[str, Any],
    thresholds: dict[str, Any],
    readiness: dict[str, Any],
    configured_bots: int,
    workload_processes: int = 1,
) -> dict[str, Any]:
    """Classify observed results without hiding missing business traffic.
    按观测结果分类；没有请求事件时明确保持 UNKNOWN，而不是伪造成功率。
    """
    blockers: list[str] = []
    slow: list[str] = []
    request_result = summary.get("requests", {})
    minimum_success = float(thresholds.get("success_rate_min_percent", 99.0))
    if request_result.get("total", 0) and request_result.get("success_rate_percent", 0.0) < minimum_success:
        blockers.append(f"request success rate below {minimum_success:.3f}%")
    phase_counters = summary.get("phase_counters", {})
    # 只有日志计数按测量窗口裁剪；进程退出、请求失败和协议计数没有阶段标签，
    # 必须保留全局值，否则测量阶段存在日志记录时会把真正的失败静默掉。
    # Only log counters are scoped to the measurement window. Process exits,
    # request failures, and protocol counters are unphased and must stay global.
    quality_counters = Counter(summary.get("counters", {}))
    measurement_counters = phase_counters.get("measurement", {})
    for metric, value in measurement_counters.items():
        if metric.startswith("log."):
            quality_counters[metric] = int(value)
    process_exits = int(quality_counters.get("process.exit.count", 0))
    if process_exits > 0:
        blockers.append(f"workload process exits={process_exits}")
    readiness_failures = int(summary.get("counters", {}).get("readiness.failure.count", 0))
    if readiness_failures > 0:
        blockers.append(f"workload readiness failures={readiness_failures}")
    max_protocol_errors = int(thresholds.get("max_protocol_errors", 0))
    protocol_errors = sum(
        value
        for metric, value in quality_counters.items()
        if "protocol" in metric.lower() or "not_found_msgid" in metric.lower()
    )
    if protocol_errors > max_protocol_errors:
        blockers.append(f"protocol errors={protocol_errors} > {max_protocol_errors}")
    max_destroyed = int(thresholds.get("max_unexpected_entity_destroyed", 0))
    destroyed = sum(value for metric, value in summary.get("counters", {}).items() if "destroy" in metric.lower())
    destroyed += sum(
        int(values["max"])
        for metric, values in summary.get("samples", {}).items()
        if "destroy" in metric.lower()
    )
    if destroyed > max_destroyed:
        blockers.append(f"destroyed entities={destroyed} > {max_destroyed}")
    network_errors = sum(
        int(values["max"])
        for metric, values in summary.get("samples", {}).items()
        if "networkerrors" in metric.lower()
    )
    if network_errors > 0:
        blockers.append(f"Bots network errors={network_errors}")
    max_log_errors = int(thresholds.get("max_log_errors", 0))
    log_errors = int(quality_counters.get("log.error.count", 0))
    if log_errors > max_log_errors:
        blockers.append(f"log errors={log_errors} > {max_log_errors}")
    for metric in ("log.resource_unavailable.count", "log.abnormal_exit.count"):
        value = int(quality_counters.get(metric, 0))
        if value > 0:
            blockers.append(f"{metric}={value}")
    for metric, expected in readiness.items():
        matches = [
            (name, values)
            for name, values in summary.get("samples", {}).items()
            if name.endswith(metric)
        ]
        if not matches:
            blockers.append(f"readiness metric was not sampled: {metric}")
            continue

        for name, values in matches:
            # `$bots` 在 CellApp 等服务端组件上表示跨组件总和；直方图已经丢失同一
            # 采样周期的组件身份，不能再把单个 CellApp 的 0 误判为总量下降。
            # On server components `$bots` is a cross-component aggregate. The summary
            # histogram no longer retains synchronized instance identity, so a single
            # CellApp's zero cannot be treated as an aggregate drop.
            if (
                expected == "$bots"
                and name.startswith("watcher.")
                and not name.startswith("watcher.BOTS_TYPE.")
            ):
                continue
            # readiness.workload.root/... 是控制器写入的多进程总和；带 bots#CID 的样本才是进程局部值。
            # readiness.workload.root/... is the controller aggregate; samples containing bots#CID remain process-local.
            # 只有 readiness 控制器显式写入的 root 样本是进程总和；测量阶段同名
            # Watcher 样本会合并为分布，但每个观测仍来自单个 Bots 进程。
            # Only the readiness controller's explicit root sample is an aggregate;
            # measurement Watcher observations remain process-local after histogram merging.
            is_process_local = not name.startswith("readiness.workload.root/")
            expected_value = (
                configured_bots // workload_processes
                if expected == "$bots" and is_process_local and workload_processes > 1
                else configured_bots if expected == "$bots" else expected
            )
            if isinstance(expected_value, dict) and set(expected_value) == {"min"}:
                minimum_expected = expected_value["min"]
                if not isinstance(minimum_expected, (int, float)):
                    blockers.append(f"invalid readiness minimum: {metric} expected={minimum_expected!r}")
                elif values["min"] < minimum_expected:
                    blockers.append(
                        f"readiness metric dropped: {name} min={values['min']}, expected>={minimum_expected}"
                    )
            elif values["min"] != expected_value:
                blockers.append(
                    f"readiness metric dropped: {name} min={values['min']}, expected={expected_value}"
                )

    p99_limit = float(thresholds.get("request_latency_p99_max_ms", 0.0))
    if p99_limit > 0:
        for operation, values in summary.get("latency", {}).items():
            if values["p99_ms"] > p99_limit:
                slow.append(f"{operation} p99={values['p99_ms']:.2f}ms > {p99_limit:.2f}ms")
    tick_limit = float(thresholds.get("tick_p99_max_ms", 0.0))
    if tick_limit > 0:
        for name, values in summary.get("samples", {}).items():
            metric_leaf = name.rsplit("/", 1)[-1].lower()
            # 累计 tickMaxMicros 包含就绪与预热阶段，不能伪装成测量窗口分位数；质量门只读取逐次 Tick 样本。
            # Cumulative tickMaxMicros includes readiness and warmup, so only per-tick samples may drive the window P99 gate.
            is_tick_sample = "tick" in name.lower() and metric_leaf in ("lastmicros", "ticklastmicros")
            if is_tick_sample and values.get("unit") == "micros" and values["p99"] / 1000.0 > tick_limit:
                slow.append(f"{name} p99={values['p99'] / 1000.0:.2f}ms > {tick_limit:.2f}ms")

    status = "BLOCKER" if blockers else "SLOW" if slow else "PASS"
    if not request_result.get("total", 0) and not blockers and not slow:
        status = "UNKNOWN"
    return {"status": status, "blockers": blockers, "slow": slow}


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
        "| Operation | Count | Mean (ms) | P50 (ms) | P95 (ms) | P99 (ms) | P99.9 (ms) | Max (ms) | Total (ms) |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for operation, values in summary.get("latency", {}).items():
        lines.append(
            "| {0} | {count} | {mean_ms:.2f} | {p50_ms:.2f} | {p95_ms:.2f} | {p99_ms:.2f} | {p999_ms:.2f} | {max_ms:.2f} | {total_ms:.2f} |".format(operation, **values)
        )
    lines.extend(["", "## Quality", "", f"- Status: `{summary.get('quality', {}).get('status', 'UNKNOWN')}`"])
    if summary.get("failure"):
        lines.append(f"- Failure phase: `{summary['failure'].get('phase', '')}`")
        lines.append(f"- Failure: {summary['failure'].get('message', '')}")
    for item in summary.get("quality", {}).get("blockers", []):
        lines.append(f"- BLOCKER: {item}")
    for item in summary.get("quality", {}).get("slow", []):
        lines.append(f"- SLOW: {item}")
    lines.extend(["", "## Samples", "", "| Metric | Count | Min | Mean | P99 | Max | Total | Unit |", "| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |"])
    for metric, values in summary.get("samples", {}).items():
        lines.append(f"| `{metric}` | {values['count']} | {values['min']:.2f} | {values['mean']:.2f} | {values['p99']:.2f} | {values['max']:.2f} | {values['total']:.2f} | {values['unit']} |")
    lines.extend(["", "## Counters", "", "| Metric | Value |", "| --- | ---: |"])
    for metric, value in summary.get("counters", {}).items():
        lines.append(f"| `{metric}` | {value} |")
    if summary.get("operation_counters"):
        lines.extend(["", "## Operation Counters", "", "| Operation | Metric | Value |", "| --- | --- | ---: |"])
        for operation, counters in summary["operation_counters"].items():
            for metric, value in counters.items():
                lines.append(f"| `{operation}` | `{metric}` | {value} |")
    if summary.get("phase_counters"):
        lines.extend(["", "## Phase Counters", "", "| Phase | Metric | Value |", "| --- | --- | ---: |"])
        for phase, counters in summary["phase_counters"].items():
            for metric, value in counters.items():
                lines.append(f"| `{phase}` | `{metric}` | {value} |")
    lines.extend(["", "## Gauges", "", "| Metric | Value |", "| --- | ---: |"])
    for metric, value in summary.get("gauges", {}).items():
        lines.append(f"| `{metric}` | {value:.2f} |")
    markdown_path.parent.mkdir(parents=True, exist_ok=True)
    markdown_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
