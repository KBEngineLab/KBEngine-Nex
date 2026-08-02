"""Standalone KBE performance runner. / 独立的 KBE 性能运行器。"""

from __future__ import annotations

import argparse
import json
import os
import shlex
import signal
import subprocess
import time
import uuid
from pathlib import Path

from .assets import (
    build_environment,
    create_config_overlay,
    resolve_bots_schedule,
    resolve_fixture_root,
    write_scenario_metadata,
)
from .cluster import PerformanceCluster
from .log_metrics import IncrementalLogCollector
from .metrics import JsonlRecorder, monotonic_milliseconds
from .process_metrics import ProcessGroupCollector
from .report import build_summary, load_events, write_report
from .watcher_metrics import WatcherCollector, WatcherSchedule, WatcherTarget, parse_target, resolve_target


class WorkloadReadinessError(RuntimeError):
    """Readiness failure carrying the last complete Watcher snapshot.
    携带最后一份完整 Watcher 快照的就绪失败。
    """

    def __init__(self, message: str, observed: dict[str, object] | None = None):
        super().__init__(message)
        self.observed = dict(observed or {})


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run an isolated KBE performance scenario")
    parser.add_argument("--scenario", required=True, type=Path)
    # Resolve the default from the repository instead of the caller's cwd.  The
    # runner changes child-process directories, and a relative default used from
    # kbe/src/tests previously created a nested tests/kbe/src/out tree.
    # 默认输出必须相对仓库根目录解析；运行器会切换子进程 cwd，旧的相对默认值
    # 从 kbe/src/tests 启动时会错误生成 tests/kbe/src/out 嵌套目录。
    parser.add_argument("--output-root", type=Path)
    parser.add_argument("--log-root", type=Path)
    parser.add_argument("--duration", type=float)
    parser.add_argument("--sample-interval", type=float, default=1.0)
    parser.add_argument("--command", help="Command line to run the workload")
    parser.add_argument("--assets-root", type=Path, help="Read-only game assets used for the isolated config overlay")
    parser.add_argument("--bots", type=int, help="Override the scenario Bots count")
    parser.add_argument("--tools-root", type=Path, help="kbe/tools/server root for Watcher queries")
    parser.add_argument("--watcher-target", action="append", default=[], metavar="TYPE=HOST:PORT:PATH|TYPE=@COMPONENT:PATH")
    parser.add_argument("--watcher-timeout", type=float, default=2.0)
    parser.add_argument("--workload-ready-timeout", type=float, default=60.0)
    parser.add_argument("--thresholds", type=Path, help="Quality thresholds JSON; defaults to the scenario reference")
    parser.add_argument("--start-cluster", action="store_true", help="Start and own an isolated nine-component server cluster")
    parser.add_argument("--cluster-components", help="Pipe-separated NAME::EXE::CID::GUS component specification")
    parser.add_argument("--cluster-binary-root", type=Path, help="CMake build root used for isolated cluster runtime files")
    parser.add_argument("--cluster-startup-timeout", type=int, default=90)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    # 子进程切换到隔离目录前冻结所有用户路径，避免相对 KBE_RES_PATH 随 cwd 失效。
    # Freeze every user path before child processes change cwd so relative KBE_RES_PATH entries remain valid.
    args.scenario = args.scenario.resolve()
    args.output_root = (args.output_root or (_repository_root() / "kbe/src/out/performance-runs")).resolve()
    if args.assets_root:
        args.assets_root = args.assets_root.resolve()
    if args.tools_root:
        args.tools_root = args.tools_root.resolve()
    if args.log_root:
        args.log_root = args.log_root.resolve()
    if args.thresholds:
        args.thresholds = args.thresholds.resolve()
    if args.cluster_binary_root:
        args.cluster_binary_root = args.cluster_binary_root.resolve()
    scenario = json.loads(args.scenario.read_text(encoding="utf-8"))
    fixture_root = resolve_fixture_root(str(scenario["fixture"])) if scenario.get("fixture") else None
    name = str(scenario["name"])
    duration = float(args.duration if args.duration is not None else scenario.get("duration_seconds", 30))
    configured_bots = int(args.bots if args.bots is not None else scenario.get("bots", 0))
    interval = max(float(args.sample_interval), 0.1)
    watcher_targets = [parse_target(value) for value in args.watcher_target]
    watcher_schedule = (
        WatcherSchedule(
            watcher_targets,
            interval,
            dict(scenario.get("watcher_intervals", {})),
        )
        if watcher_targets
        else None
    )
    run_id = f"{time.strftime('%Y%m%d-%H%M%S')}-{uuid.uuid4().hex[:8]}"
    output = args.output_root / f"{name}-{run_id}"
    output.mkdir(parents=True, exist_ok=True)
    environment = None
    if args.assets_root:
        bots_tick_time, bots_tick_count = resolve_bots_schedule(scenario, configured_bots)
        create_config_overlay(
            args.assets_root,
            output,
            configured_bots,
            bots_tick_time,
            bots_tick_count,
            int(scenario["external_receive_messages"]) if "external_receive_messages" in scenario else None,
            int(scenario["external_receive_bytes"]) if "external_receive_bytes" in scenario else None,
        )
        environment = build_environment(_repository_root(), args.assets_root, output, fixture_root)
        scenario_metadata = dict(scenario)
        scenario_metadata["effective_bots_tick_time"] = bots_tick_time
        scenario_metadata["effective_bots_tick_count"] = bots_tick_count
        write_scenario_metadata(output, scenario_metadata, configured_bots)
    elif args.command and configured_bots > 0:
        raise ValueError("--assets-root is required when starting a scenario with Bots")
    cluster = None
    owned_processes: dict[str, int] = {}
    if args.start_cluster:
        if not args.assets_root or not args.cluster_components or not args.cluster_binary_root:
            raise ValueError("--start-cluster requires --assets-root, --cluster-components and --cluster-binary-root")
        cluster = PerformanceCluster(
            _repository_root(),
            args.assets_root,
            args.cluster_binary_root,
            args.cluster_binary_root / "Testing" / f"performance-cluster-{run_id}",
            args.cluster_components,
            output / "config-overlay/res",
            fixture_root,
            args.cluster_startup_timeout,
        )
        owned_processes.update({f"cluster:{item.name}": item.pid for item in cluster.start()})
    try:
        process = start_command(args.command, environment, output)
    except Exception:
        if cluster is not None:
            cluster.stop()
        raise
    if process:
        owned_processes["workload"] = process.pid
    process_collector = ProcessGroupCollector(owned_processes) if owned_processes else None
    log_roots = {output.resolve()}
    if args.log_root:
        log_roots.add(args.log_root.resolve())
    if cluster is not None:
        log_roots.add((cluster.run_root / "server/logs").resolve())
    log_collectors = [IncrementalLogCollector(path) for path in sorted(log_roots)]
    watcher_collector = WatcherCollector(args.tools_root, args.watcher_timeout) if args.tools_root and args.watcher_target else None
    events_path = output / "raw.jsonl"
    readiness_failure: WorkloadReadinessError | None = None
    with JsonlRecorder(events_path, run_id, name) as recorder:
        try:
            watcher_targets = wait_for_workload_ready(
                process,
                watcher_collector,
                watcher_targets,
                list(log_roots),
                max(float(args.workload_ready_timeout), 1.0),
                dict(scenario.get("readiness", {})),
                configured_bots,
            )
            wait_for_warmup(process, max(float(scenario.get("warmup_seconds", 0.0)), 0.0))
            if watcher_collector:
                drain_watcher_windows(watcher_collector, watcher_targets)
            # 丢弃启动阶段日志，避免初始化告警污染稳态质量门；测量和关闭阶段仍分别保留。
            # Drain startup logs so initialization noise cannot fail the steady-state gate; measurement and shutdown remain visible.
            for log_collector in log_collectors:
                record_log_samples(recorder, log_collector, "startup")
            started = time.monotonic()
            next_sample = started
            while time.monotonic() - started < duration:
                if process and process.poll() is not None:
                    recorder.record("runner", "workload", "process.exit.count", 1, "count", {"kind": "counter"})
                    break
                if process_collector:
                    record_process_samples(recorder, process_collector)
                for log_collector in log_collectors:
                    record_log_samples(recorder, log_collector, "measurement")
                if watcher_collector:
                    sample_now = time.monotonic()
                    for target in watcher_targets:
                        if watcher_schedule is not None and not watcher_schedule.due(target, sample_now):
                            continue
                        request_started_ms = monotonic_milliseconds()
                        operation = f"watcher:{target.component_type}:{target.path}"
                        try:
                            values = watcher_collector.query(target)
                        except (OSError, RuntimeError, TimeoutError, ValueError) as exc:
                            recorder.record_request_latency(
                                "watcher",
                                target.component_type,
                                operation,
                                request_started_ms,
                                monotonic_milliseconds(),
                                False,
                                type(exc).__name__,
                                float(scenario.get("slow_request_threshold_ms", 0.0)) or None,
                            )
                            recorder.record("watcher", target.component_type, "query.error.count", 1, "count", {"kind": "counter", "error": type(exc).__name__})
                        else:
                            recorder.record_request_latency(
                                "watcher",
                                target.component_type,
                                operation,
                                request_started_ms,
                                monotonic_milliseconds(),
                                True,
                                slow_threshold_ms=float(scenario.get("slow_request_threshold_ms", 0.0)) or None,
                            )
                            for metric, value in values.items():
                                if isinstance(value, (int, float)):
                                    metric_name = f"{target.path}/{metric}".replace("//", "/")
                                    recorder.record_sample(
                                        "watcher",
                                        target.component_type,
                                        metric_name,
                                        value,
                                        watcher_unit(metric_name),
                                    )
                        finally:
                            if watcher_schedule is not None:
                                # Anchor deadlines to the sampling cycle so query RTT cannot
                                # accidentally halve a one-second target's effective cadence.
                                # 截止时间锚定采样周期，避免查询 RTT 把一秒目标意外降成隔轮采样。
                                watcher_schedule.mark_sampled(target, sample_now)
                recorder.flush()
                next_sample += interval
                delay = next_sample - time.monotonic()
                if delay > 0:
                    time.sleep(delay)
                elif delay < -interval:
                    next_sample = time.monotonic()
        except WorkloadReadinessError as exc:
            readiness_failure = exc
            recorder.record(
                "runner",
                "workload",
                "readiness.failure.count",
                1,
                "count",
                {"kind": "counter", "error": type(exc).__name__},
            )
            for metric, value in sorted(exc.observed.items()):
                if isinstance(value, (int, float)):
                    recorder.record_sample("readiness", "workload", metric, value, watcher_unit(metric))
            if process_collector:
                record_process_samples(recorder, process_collector)
        finally:
            # 先停止本轮进程再读取日志，保证文件内容闭合且扫描开销不污染测量窗口。
            # Stop owned processes before reading logs so files are complete and scan cost cannot perturb the measured window.
            stop_process(process)
            process = None
            if cluster is not None:
                cluster.stop()
                cluster = None
            if watcher_collector is not None:
                watcher_collector.close()
            for log_collector in log_collectors:
                record_log_samples(recorder, log_collector, "shutdown")
            recorder.flush()
    summary = build_summary(
        load_events(events_path),
        load_thresholds(args.scenario, scenario, args.thresholds),
        dict(scenario.get("readiness", {})),
        configured_bots,
    )
    if readiness_failure is not None:
        summary["failure"] = {"phase": "readiness", "message": str(readiness_failure)}
    write_report(summary, output / "summary.json", output / "report.md")
    print(json.dumps({"run_id": run_id, "output": str(output), "summary": summary}, ensure_ascii=True))
    return 1 if readiness_failure is not None else 0


def start_command(
    command: str | None,
    environment: dict[str, str] | None = None,
    working_directory: Path | None = None,
) -> subprocess.Popen[bytes] | None:
    if not command:
        return None
    args = shlex.split(command, posix=os.name != "nt")
    if not args:
        return None
    stdout_handle = None
    stderr_handle = None
    if working_directory is not None:
        stdout_handle = (working_directory / "workload.stdout.log").open("wb")
        stderr_handle = (working_directory / "workload.stderr.log").open("wb")
    try:
        return subprocess.Popen(
            args,
            stdout=stdout_handle or subprocess.DEVNULL,
            stderr=stderr_handle or subprocess.DEVNULL,
            env=environment,
            cwd=working_directory,
        )
    finally:
        if stdout_handle is not None:
            stdout_handle.close()
        if stderr_handle is not None:
            stderr_handle.close()


def _repository_root() -> Path:
    return Path(__file__).resolve().parents[4]


def load_thresholds(scenario_path: Path, scenario: dict[str, object], explicit: Path | None) -> dict[str, object]:
    path = explicit
    if path is None and scenario.get("thresholds"):
        path = scenario_path.parent / str(scenario["thresholds"])
    if path is None:
        return {}
    return json.loads(path.resolve().read_text(encoding="utf-8"))


def watcher_unit(metric: str) -> str:
    lowered = metric.lower()
    if lowered.endswith("micros"):
        return "micros"
    if "bytes" in lowered:
        return "bytes"
    if any(token in lowered for token in ("count", "total", "clients", "destroyed", "errors")):
        return "count"
    return ""


def record_process_samples(recorder: JsonlRecorder, collector: ProcessGroupCollector) -> None:
    """Record one batch obtained by the collector's single OS query.
    记录采集器通过一次系统查询取得的一批进程快照。
    """
    for process_name, sample in collector.sample().items():
        recorder.record_sample("process", process_name, "cpu.percent", sample.cpu_percent, "%")
        recorder.record_sample("process", process_name, "memory.working_set", sample.working_set_bytes, "bytes")
        if sample.private_bytes is not None:
            recorder.record_sample("process", process_name, "memory.private", sample.private_bytes, "bytes")
        if sample.peak_working_set_bytes is not None:
            recorder.record_sample(
                "process",
                process_name,
                "memory.working_set_peak",
                sample.peak_working_set_bytes,
                "bytes",
            )
        recorder.record_sample("process", process_name, "threads.active", sample.thread_count, "count")
        if sample.handle_count is not None:
            recorder.record_sample("process", process_name, "handles.active", sample.handle_count, "count")


def record_log_samples(recorder: JsonlRecorder, collector: IncrementalLogCollector, phase: str) -> None:
    """Record incremental logs with lifecycle phase metadata.
    按生命周期阶段记录增量日志，避免把主动关闭期间的断链提示算入稳态错误。
    """
    for metric, value in collector.sample().items():
        recorder.record(
            "logs",
            str(collector.root),
            f"log.{metric}.count",
            value,
            "count",
            {"kind": "counter", "phase": phase},
        )


def wait_for_workload_ready(
    process: subprocess.Popen[bytes] | None,
    watcher_collector: WatcherCollector | None,
    watcher_targets: list[WatcherTarget],
    log_roots: list[Path],
    timeout_seconds: float,
    readiness: dict[str, object],
    configured_bots: int,
) -> list[WatcherTarget]:
    """Exclude process and Watcher startup from the measured request window.
    将进程和 Watcher 启动期排除在请求成功率与延迟窗口之外。
    """
    if process is None or watcher_collector is None or not watcher_targets:
        return watcher_targets
    deadline = time.monotonic() + timeout_seconds
    last_error: Exception | None = None
    last_observed: dict[str, object] = {}
    while time.monotonic() < deadline:
        result = process.poll()
        if result is not None:
            raise WorkloadReadinessError(
                f"workload exited before readiness with code {result}",
                last_observed,
            )
        try:
            resolved_targets = [resolve_target(target, log_roots) for target in watcher_targets]
            observed: dict[str, object] = {}
            for target in resolved_targets:
                values = watcher_collector.query(target)
                observed.update(
                    {
                        f"{target.path}/{metric}".replace("//", "/"): value
                        for metric, value in values.items()
                    }
                )
            last_observed = observed
            for metric, expected in readiness.items():
                expected_value = configured_bots if expected == "$bots" else expected
                if observed.get(metric) != expected_value:
                    raise RuntimeError(
                        f"readiness {metric}={observed.get(metric)!r}, expected={expected_value!r}"
                    )
            return resolved_targets
        except (LookupError, OSError, RuntimeError, TimeoutError, ValueError) as exc:
            last_error = exc
            time.sleep(0.25)
    raise WorkloadReadinessError(
        f"workload readiness timed out after {timeout_seconds:.1f}s: {last_error}",
        last_observed,
    )


def wait_for_warmup(process: subprocess.Popen[bytes] | None, seconds: float) -> None:
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if process is not None and process.poll() is not None:
            raise RuntimeError(f"workload exited during warmup with code {process.returncode}")
        time.sleep(min(0.25, max(deadline - time.monotonic(), 0.0)))


def drain_watcher_windows(watcher_collector: WatcherCollector, watcher_targets: list[WatcherTarget]) -> None:
    """Discard readiness/warmup watcher windows before steady-state sampling.
    丢弃 readiness/warmup 阶段累积的窗口指标，避免启动排队污染稳态 P99。
    """
    for target in watcher_targets:
        watcher_collector.query(target)


def stop_process(process: subprocess.Popen[bytes] | None) -> None:
    if not process or process.poll() is not None:
        return
    if os.name == "nt":
        process.terminate()
    else:
        process.send_signal(signal.SIGTERM)
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


if __name__ == "__main__":
    raise SystemExit(main())
