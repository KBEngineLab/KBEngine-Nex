"""Standalone KBE performance runner. / 独立的 KBE 性能运行器。"""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
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
from .topology import build_local_cluster_components, partition_batch_size
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
    parser.add_argument("--workload-processes", "--bots-processes", dest="workload_processes", type=int,
                        help="Number of Bots workload processes")
    parser.add_argument("--workload-cid-start", type=int, help="First workload component ID")
    parser.add_argument("--workload-gus-start", type=int, help="First workload group order ID")
    parser.add_argument("--assets-root", type=Path, help="Read-only game assets used for the isolated config overlay")
    parser.add_argument("--bots", type=int, help="Override the scenario Bots count")
    parser.add_argument("--bots-batch-size", type=int,
                        help="Aggregate Bots added per batch across all Bots processes")
    parser.add_argument("--bots-batch-interval", type=float,
                        help="Seconds between Bots connection batches")
    parser.add_argument("--bots-dev", action="store_true",
                        help="Enable Bots Logger forwarding for IDE development")
    parser.add_argument("--reuse-existing-accounts", action="store_true",
                        help="Skip account creation and authenticate pre-provisioned deterministic Bots accounts")
    parser.add_argument("--tools-root", type=Path, help="kbe/tools/server root for Watcher queries")
    parser.add_argument("--watcher-target", action="append", default=[], metavar="TYPE=HOST:PORT:PATH|TYPE=@COMPONENT:PATH")
    parser.add_argument("--watcher-timeout", type=float,
                        help="Watcher request timeout; defaults to watcher_timeout_seconds in the scenario")
    parser.add_argument("--watcher-concurrency", type=int,
                        help="Parallel Watcher queries; defaults to watcher_concurrency in the scenario")
    parser.add_argument("--workload-ready-timeout", type=float, default=60.0)
    parser.add_argument("--thresholds", type=Path, help="Quality thresholds JSON; defaults to the scenario reference")
    parser.add_argument("--start-cluster", action="store_true", help="Start and own an isolated server cluster")
    parser.add_argument("--server-binary-dir", type=Path,
                        help="Server binary directory; defaults to kbe/bin/server with --start-cluster")
    parser.add_argument("--baseapp-count", type=int, help="Number of local BaseApp processes")
    parser.add_argument("--cellapp-count", type=int, help="Number of local CellApp processes")
    parser.add_argument("--cluster-components", help="Pipe-separated NAME::EXE::CID::GUS component specification")
    parser.add_argument("--cluster-binary-root", "--build-root", dest="cluster_binary_root", type=Path,
                        help="CMake build root used for isolated cluster runtime files")
    parser.add_argument("--cluster-startup-timeout", type=int, default=90)
    parser.add_argument("--server-ready-timeout", type=float, default=240.0,
                        help="Seconds to wait for BaseAppMgr and CellAppMgr onReadyForLogin readiness")
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
    args.server_binary_dir = resolve_server_binary_dir(
        args.server_binary_dir,
        args.start_cluster,
    )
    scenario = load_scenario(args.scenario)
    args.watcher_timeout = float(
        args.watcher_timeout
        if args.watcher_timeout is not None
        else scenario.get("watcher_timeout_seconds", 2.0)
    )
    if args.watcher_timeout <= 0:
        raise ValueError("--watcher-timeout must be positive")
    args.watcher_concurrency = int(
        args.watcher_concurrency
        if args.watcher_concurrency is not None
        else scenario.get("watcher_concurrency", 1)
    )
    if args.watcher_concurrency <= 0:
        raise ValueError("--watcher-concurrency must be positive")
    fixture_root = resolve_fixture_root(str(scenario["fixture"])) if scenario.get("fixture") else None
    name = str(scenario["name"])
    duration = float(args.duration if args.duration is not None else scenario.get("duration_seconds", 30))
    configured_bots = int(args.bots if args.bots is not None else scenario.get("bots", 0))
    workload_process_count = int(
        args.workload_processes
        if args.workload_processes is not None
        else scenario.get("workload_processes", 1)
    )
    workload_cid_start = int(
        args.workload_cid_start
        if args.workload_cid_start is not None
        else scenario.get("workload_cid_start", 10000)
    )
    workload_gus_start = int(
        args.workload_gus_start
        if args.workload_gus_start is not None
        else scenario.get("workload_gus_start", 40)
    )
    declared_component_ids = dict(scenario.get("watcher_component_ids", {}))
    baseapp_count = int(args.baseapp_count if args.baseapp_count is not None else
                        len(declared_component_ids.get("baseapp", [7001])))
    cellapp_count = int(args.cellapp_count if args.cellapp_count is not None else
                        len(declared_component_ids.get("cellapp", [8001])))
    if baseapp_count < 1 or cellapp_count < 1:
        raise ValueError("--baseapp-count and --cellapp-count must be positive")
    if args.server_binary_dir:
        generated_components, generated_component_ids = build_local_cluster_components(
            args.server_binary_dir, baseapp_count, cellapp_count,
        )
        if not args.cluster_components:
            args.cluster_components = generated_components
        if not args.command:
            dev_argument = " --dev" if args.bots_dev else ""
            reuse_accounts = args.reuse_existing_accounts or bool(
                scenario.get("reuse_existing_accounts", False)
            )
            reuse_argument = " --reuse-existing-accounts" if reuse_accounts else ""
            args.command = (
                f'"{args.server_binary_dir / "bots.exe"}" '
                f"--cid={{cid}} --gus={{gus}} --hide=1{dev_argument}{reuse_argument}"
            )
        scenario = dict(scenario)
        scenario["watcher_component_ids"] = generated_component_ids
    elif args.bots_dev or args.reuse_existing_accounts:
        if not args.command:
            raise ValueError("Bots mode flags require --server-binary-dir or --command")
        if args.bots_dev:
            args.command = f"{args.command} --dev"
        if args.reuse_existing_accounts:
            args.command = f"{args.command} --reuse-existing-accounts"

    bots_per_process = partition_workload_bots(configured_bots, workload_process_count)
    interval = max(float(args.sample_interval), 0.1)
    watcher_target_specs = merge_watcher_target_specs(
        list(scenario.get("watcher_targets", [])),
        args.watcher_target,
    )
    watcher_targets = expand_bots_watcher_targets(
        [parse_target(value) for value in watcher_target_specs],
        workload_process_count,
        workload_cid_start,
    )
    watcher_targets = expand_component_watcher_targets(
        watcher_targets,
        dict(scenario.get("watcher_component_ids", {})),
    )
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
        per_process_scenario = dict(scenario)
        if args.bots_batch_size is not None:
            per_process_scenario["bots_tick_count"] = partition_batch_size(
                args.bots_batch_size, workload_process_count,
            )
        if args.bots_batch_interval is not None:
            if args.bots_batch_interval <= 0:
                raise ValueError("--bots-batch-interval must be positive")
            per_process_scenario["bots_tick_time"] = args.bots_batch_interval
        if "connect_rate_per_second" in per_process_scenario:
            per_process_scenario["connect_rate_per_second"] = (
                float(per_process_scenario["connect_rate_per_second"]) / workload_process_count
            )
        bots_tick_time, bots_tick_count = resolve_bots_schedule(
            per_process_scenario,
            bots_per_process,
        )
        create_config_overlay(
            args.assets_root,
            output,
            bots_per_process,
            bots_tick_time,
            bots_tick_count,
            int(scenario["external_receive_messages"]) if "external_receive_messages" in scenario else None,
            int(scenario["external_receive_bytes"]) if "external_receive_bytes" in scenario else None,
            float(scenario["external_timeout_seconds"]) if "external_timeout_seconds" in scenario else None,
            int(scenario["reliable_udp_tick_interval_ms"]) if "reliable_udp_tick_interval_ms" in scenario else None,
            int(scenario["reliable_udp_min_rto_ms"]) if "reliable_udp_min_rto_ms" in scenario else None,
            str(scenario["runtime_log_level"]) if "runtime_log_level" in scenario else None,
            str(scenario["server_runtime_log_level"]) if "server_runtime_log_level" in scenario else None,
            int(scenario["bots_account_suffix_start"]) if "bots_account_suffix_start" in scenario else None,
            int(scenario["database_connections"]) if "database_connections" in scenario else None,
            baseapp_count,
            bool(scenario.get("performance_probes_enabled", False)),
        )
        environment = build_environment(_repository_root(), args.assets_root, output, fixture_root)
        environment.update(scenario_environment(scenario))
        scenario_metadata = dict(scenario)
        scenario_metadata["effective_bots_tick_time"] = bots_tick_time
        scenario_metadata["effective_bots_tick_count"] = bots_tick_count
        scenario_metadata["effective_workload_processes"] = workload_process_count
        scenario_metadata["effective_bots_per_process"] = bots_per_process
        scenario_metadata["effective_workload_cids"] = list(
            range(workload_cid_start, workload_cid_start + workload_process_count)
        )
        write_scenario_metadata(output, scenario_metadata, configured_bots)
    elif args.command and configured_bots > 0:
        raise ValueError("--assets-root is required when starting a scenario with Bots")
    cluster = None
    owned_processes: dict[str, int] = {"controller": os.getpid()}
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
            scenario_cluster_environment(scenario),
        )
        try:
            owned_processes.update({f"cluster:{item.name}": item.pid for item in cluster.start()})
            if not args.tools_root:
                raise ValueError("--tools-root is required to verify server onReadyForLogin readiness")
            wait_for_cluster_ready_for_login(
                args.tools_root,
                [cluster.run_root / "server/logs"],
                baseapp_count,
                cellapp_count,
                args.server_ready_timeout,
                args.watcher_timeout,
                cluster,
            )
            server_readiness = scenario.get("server_readiness")
            if server_readiness is not None:
                if not isinstance(server_readiness, dict):
                    raise ValueError("server_readiness must be an object")
                cluster.wait_for_log_readiness(
                    str(server_readiness["pattern"]),
                    int(server_readiness["min_count"]),
                    str(server_readiness.get("log_glob", "*.log")),
                    float(server_readiness.get("timeout_seconds", 180.0)),
                )
        except Exception:
            cluster.stop()
            raise
    try:
        processes = start_workload_commands(
            args.command,
            workload_process_count,
            workload_cid_start,
            workload_gus_start,
            environment,
            output,
        )
    except Exception:
        if cluster is not None:
            cluster.stop()
        raise
    for index, process in enumerate(processes):
        process_name = (
            "workload"
            if workload_process_count == 1
            else f"workload:{workload_cid_start + index}"
        )
        owned_processes[process_name] = process.pid
    process_collector = ProcessGroupCollector(owned_processes) if owned_processes else None
    log_roots = {output.resolve()}
    if args.log_root:
        log_roots.add(args.log_root.resolve())
    if cluster is not None:
        log_roots.add((cluster.run_root / "server/logs").resolve())
    log_collectors = [IncrementalLogCollector(path) for path in sorted(log_roots)]
    watcher_collector = WatcherCollector(args.tools_root, args.watcher_timeout) if args.tools_root and watcher_targets else None
    events_path = output / "raw.jsonl"
    readiness_failure: WorkloadReadinessError | None = None
    with JsonlRecorder(events_path, run_id, name) as recorder:
        try:
            # Capture initialization separately from the potentially long readiness window.
            # 将初始化日志与可能持续较久的负载就绪阶段分开记录。
            for log_collector in log_collectors:
                record_log_samples(recorder, log_collector, "startup")
            watcher_targets = wait_for_workload_ready(
                processes,
                watcher_collector,
                watcher_targets,
                list(log_roots),
                max(float(args.workload_ready_timeout), 1.0),
                dict(scenario.get("readiness", {})),
                configured_bots,
                cluster,
                args.watcher_concurrency,
            )
            for log_collector in log_collectors:
                record_log_samples(recorder, log_collector, "readiness")
            wait_for_warmup(
                processes,
                max(float(scenario.get("warmup_seconds", 0.0)), 0.0),
                cluster,
            )
            for log_collector in log_collectors:
                record_log_samples(recorder, log_collector, "warmup")
            if watcher_collector:
                for target, error in drain_watcher_windows(
                    watcher_collector, watcher_targets, args.watcher_concurrency
                ):
                    recorder.record(
                        "watcher",
                        watcher_instance(target),
                        "windowDrain.error.count",
                        1,
                        "count",
                        {"kind": "counter", "error": type(error).__name__},
                    )
            watcher_sample_times: dict[tuple[str, str, int, str], float] = {}
            watcher_stamps_per_second: dict[tuple[str, str, int], float] = {}
            cprofile_previous: dict[tuple[str, str, int, str], tuple[int, float, float, float]] = {}
            watcher_counter_previous: dict[
                tuple[str, str, int, str], tuple[float, float]
            ] = {}
            if watcher_collector:
                for target in watcher_targets:
                    instance = watcher_instance(target)
                    recorder.record_sample(
                        "watcher",
                        instance,
                        f"{target.path}/sampling/configuredIntervalMs",
                        watcher_schedule.interval(target) * 1000.0 if watcher_schedule else interval * 1000.0,
                        "ms",
                    )
                    if watcher_schedule is not None:
                        recorder.record_sample(
                            "watcher",
                            instance,
                            f"{target.path}/sampling/configuredPhaseOffsetMs",
                            watcher_schedule.phase_offset(target) * 1000.0,
                            "ms",
                        )
            started = time.monotonic()
            if watcher_schedule is not None:
                watcher_schedule.start(started)
            next_sample = started
            while time.monotonic() - started < duration:
                workload_exit = first_workload_exit(processes)
                if workload_exit is not None:
                    recorder.record("runner", "workload", "process.exit.count", 1, "count", {"kind": "counter"})
                    break
                if cluster is not None:
                    cluster.assert_running("measurement")
                if process_collector:
                    record_process_samples(recorder, process_collector)
                for log_collector in log_collectors:
                    record_log_samples(recorder, log_collector, "measurement")
                if watcher_collector:
                    sample_now = time.monotonic()
                    due_targets: list[WatcherTarget] = []
                    for target in watcher_targets:
                        instance = watcher_instance(target)
                        target_id = (target.component_type, target.host, target.port, target.path)
                        operation = f"watcher:{instance}:{target.path}"
                        if watcher_schedule is not None and not watcher_schedule.due(target, sample_now):
                            recorder.record(
                                "watcher",
                                instance,
                                "schedule.skipped.count",
                                1,
                                "count",
                                {"kind": "counter", "operation": operation},
                            )
                            continue
                        recorder.record(
                            "watcher",
                            instance,
                            "schedule.due.count",
                            1,
                            "count",
                            {"kind": "counter", "operation": operation},
                        )
                        previous_sample = watcher_sample_times.get(target_id)
                        if previous_sample is not None:
                            recorder.record_sample(
                                "watcher",
                                instance,
                                f"{target.path}/sampling/actualIntervalMs",
                                (sample_now - previous_sample) * 1000.0,
                                "ms",
                            )
                        watcher_sample_times[target_id] = sample_now
                        due_targets.append(target)
                        if watcher_schedule is not None:
                            # 查询并发时截止时间必须在投递前统一推进，避免快响应目标改变同轮判断。
                            # Advance deadlines before dispatch so fast responses cannot alter this cycle's due set.
                            watcher_schedule.mark_sampled(target, sample_now)

                    for target, request_started_ms, request_finished_ms, values, error in query_watcher_batch(
                        watcher_collector,
                        due_targets,
                        args.watcher_concurrency,
                    ):
                        instance = watcher_instance(target)
                        operation = f"watcher:{instance}:{target.path}"
                        if error is not None:
                            recorder.record_request_latency(
                                "watcher",
                                instance,
                                operation,
                                request_started_ms,
                                request_finished_ms,
                                False,
                                type(error).__name__,
                                float(scenario.get("slow_request_threshold_ms", 0.0)) or None,
                            )
                            recorder.record("watcher", instance, "query.error.count", 1, "count", {"kind": "counter", "error": type(error).__name__})
                        else:
                            recorder.record_request_latency(
                                "watcher",
                                instance,
                                operation,
                                request_started_ms,
                                request_finished_ms,
                                True,
                                slow_threshold_ms=float(scenario.get("slow_request_threshold_ms", 0.0)) or None,
                            )
                            record_watcher_samples(
                                recorder,
                                target,
                                values,
                                sample_now,
                                watcher_stamps_per_second,
                                cprofile_previous,
                                watcher_counter_previous,
                            )
                            query_stats = watcher_collector.last_query_stats(target)
                            if query_stats is not None:
                                recorder.record_sample(
                                    "watcher",
                                    instance,
                                    f"{target.path}/sampling/responseValues",
                                    query_stats.value_count,
                                    "count",
                                )
                                recorder.record_sample(
                                    "watcher",
                                    instance,
                                    f"{target.path}/sampling/responseBytesEstimated",
                                    query_stats.response_bytes_estimated,
                                    "bytes_estimated",
                                )
                                recorder.record_sample(
                                    "watcher",
                                    instance,
                                    f"{target.path}/sampling/connectionReused",
                                    int(query_stats.connection_reused),
                                    "count",
                                )
                recorder.flush()
                next_sample += interval
                delay = next_sample - time.monotonic()
                if delay > 0:
                    time.sleep(delay)
                elif delay < -interval:
                    next_sample = time.monotonic()
        except WorkloadReadinessError as exc:
            readiness_failure = exc
            for log_collector in log_collectors:
                record_log_samples(recorder, log_collector, "readiness")
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
            stop_processes(processes)
            processes = []
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
        workload_process_count,
    )
    if readiness_failure is not None:
        summary["failure"] = {"phase": "readiness", "message": str(readiness_failure)}
    write_report(summary, output / "summary.json", output / "report.md")
    print(json.dumps({"run_id": run_id, "output": str(output), "summary": summary}, ensure_ascii=True))
    return 1 if readiness_failure is not None else 0


def partition_workload_bots(configured_bots: int, process_count: int) -> int:
    """Return an equal per-process Bot count for an aggregate workload.
    将全局 Bots 数量等分到各压测进程，避免 XML 的单进程语义造成重复创建。
    """
    if process_count < 1:
        raise ValueError("workload process count must be positive")
    if configured_bots < 1:
        raise ValueError("configured Bots must be positive")
    if configured_bots % process_count != 0:
        raise ValueError(
            f"configured Bots ({configured_bots}) must be divisible by workload processes "
            f"({process_count})"
        )
    return configured_bots // process_count


def expand_bots_watcher_targets(
    targets: list[WatcherTarget],
    process_count: int,
    cid_start: int,
) -> list[WatcherTarget]:
    """Expand an unqualified Bots target to every owned Bots component.
    将未指定组件 ID 的 Bots 目标扩展到本轮拥有的每个 Bots 进程。
    """
    expanded: list[WatcherTarget] = []
    for target in targets:
        if target.component_name.lower() != "bots" or process_count == 1:
            expanded.append(target)
            continue
        expanded.extend(
            WatcherTarget(
                target.component_type,
                target.host,
                target.port,
                target.path,
                f"bots#{cid_start + index}",
            )
            for index in range(process_count)
        )
    return expanded


def expand_component_watcher_targets(
    targets: list[WatcherTarget],
    configured_ids: dict[object, object],
) -> list[WatcherTarget]:
    """Expand selected server Watchers to every declared component instance.
    将指定服务端 Watcher 扩展到场景声明的全部组件实例。

    Explicit IDs keep runs reproducible and avoid guessing ownership from unrelated
    machine logs. Already-qualified selectors such as baseapp#7001 remain unchanged.
    显式 CID 保证运行可复现，也避免从无关 Machine 日志猜测归属；已经带 CID 的目标保持不变。
    """
    component_ids: dict[str, tuple[int, ...]] = {}
    for component_name, raw_ids in configured_ids.items():
        name = str(component_name).lower()
        if not name or not isinstance(raw_ids, list):
            raise ValueError("watcher_component_ids values must be arrays")
        ids = tuple(int(value) for value in raw_ids)
        if not ids or any(value <= 0 for value in ids) or len(set(ids)) != len(ids):
            raise ValueError(f"watcher_component_ids contains invalid IDs: {component_name}")
        component_ids[name] = ids

    expanded: list[WatcherTarget] = []
    for target in targets:
        name = target.component_name.lower()
        ids = component_ids.get(name)
        if not ids or "#" in target.component_name:
            expanded.append(target)
            continue
        expanded.extend(
            WatcherTarget(
                target.component_type,
                target.host,
                target.port,
                target.path,
                f"{target.component_name}#{component_id}",
            )
            for component_id in ids
        )
    return expanded


def manager_readiness_satisfied(values: dict[str, object], expected_apps: int) -> bool:
    """Validate one Manager's aggregated onReadyForLogin snapshot.
    校验一个 Manager 聚合的 onReadyForLogin 快照。
    """
    try:
        ready_flag = values["readyForLogin"]
        ready = ready_flag is True or str(ready_flag).strip().lower() in {"1", "true", "yes"}
        return (
            ready
            and int(values["readyApps"]) == expected_apps
            and int(values["totalApps"]) == expected_apps
            and float(values["minProgress"]) >= 1.0
        )
    except (KeyError, TypeError, ValueError):
        return False


def wait_for_cluster_ready_for_login(
    tools_root: Path,
    log_roots: list[Path],
    baseapp_count: int,
    cellapp_count: int,
    timeout_seconds: float,
    watcher_timeout_seconds: float,
    cluster: PerformanceCluster | None = None,
) -> dict[str, dict[str, object]]:
    """Wait for standard BaseApp/CellApp onReadyForLogin aggregation.
    等待标准 BaseApp/CellApp onReadyForLogin 聚合状态。
    """
    if timeout_seconds <= 0:
        raise ValueError("server readiness timeout must be positive")
    declarations = (
        (parse_target("BASEAPPMGR_TYPE=@baseappmgr:root/readiness"), baseapp_count),
        (parse_target("CELLAPPMGR_TYPE=@cellappmgr:root/readiness"), cellapp_count),
    )
    collector = WatcherCollector(tools_root, max(watcher_timeout_seconds, 0.1))
    observed: dict[str, dict[str, object]] = {}
    deadline = time.monotonic() + timeout_seconds
    try:
        while time.monotonic() < deadline:
            if cluster is not None:
                cluster.assert_running("server readiness")
            all_ready = True
            for unresolved, expected_apps in declarations:
                try:
                    target = collector.resolve_target(unresolved, log_roots)
                    values = collector.query(target)
                    observed[unresolved.component_name] = values
                    if not manager_readiness_satisfied(values, expected_apps):
                        all_ready = False
                except (LookupError, OSError, RuntimeError, TimeoutError, ValueError):
                    all_ready = False
            if all_ready:
                return observed
            time.sleep(0.25)
    finally:
        collector.close()
    raise TimeoutError(
        f"server onReadyForLogin readiness timed out after {timeout_seconds:.1f}s: {observed}"
    )


def start_workload_commands(
    command: str | None,
    process_count: int,
    cid_start: int,
    gus_start: int,
    environment: dict[str, str] | None = None,
    working_directory: Path | None = None,
) -> list[subprocess.Popen[bytes]]:
    """Start an owned workload group and clean up partial startup failures.
    启动本轮拥有的压测进程组；若中途失败，立即清理已经启动的成员。
    """
    if not command:
        return []
    processes: list[subprocess.Popen[bytes]] = []
    try:
        for index in range(process_count):
            rendered = (
                command.replace("{index}", str(index))
                .replace("{cid}", str(cid_start + index))
                .replace("{gus}", str(gus_start + index))
            )
            output_stem = "workload" if process_count == 1 else f"workload.{cid_start + index}"
            process = start_command(rendered, environment, working_directory, output_stem)
            if process is None:
                raise ValueError("workload command cannot be empty")
            processes.append(process)
        return processes
    except Exception:
        stop_processes(processes)
        raise


def start_command(
    command: str | None,
    environment: dict[str, str] | None = None,
    working_directory: Path | None = None,
    output_stem: str = "workload",
) -> subprocess.Popen[bytes] | None:
    if not command:
        return None
    # CreateProcess owns Windows command-line quoting. shlex(posix=False) keeps
    # wrapping quotes inside argv[0], which breaks executable paths containing spaces.
    # Windows 路径引号交给 CreateProcess 解析；shlex(posix=False) 会把 argv[0]
    # 两侧引号保留下来，导致含空格的可执行文件路径无法启动。
    args: str | list[str] = command if os.name == "nt" else shlex.split(command)
    if not args:
        return None
    stdout_handle = None
    stderr_handle = None
    if working_directory is not None:
        stdout_handle = (working_directory / f"{output_stem}.stdout.log").open("wb")
        stderr_handle = (working_directory / f"{output_stem}.stderr.log").open("wb")
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


def merge_watcher_target_specs(scenario_specs: list[object], cli_specs: list[str]) -> list[str]:
    """Merge versioned scenario targets with additive CLI targets in stable order.
    按稳定顺序合并版本化场景目标和命令行追加目标，并消除完全相同的重复查询。
    """
    merged: list[str] = []
    seen: set[str] = set()
    for value in (*scenario_specs, *cli_specs):
        target = str(value)
        if target in seen:
            continue
        seen.add(target)
        merged.append(target)
    return merged


def load_scenario(path: Path, loading: set[Path] | None = None) -> dict[str, object]:
    """Load one versioned scenario with optional same-directory inheritance.
    加载版本化场景，并支持同目录内的可选继承。
    """
    resolved = path.resolve()
    active = set() if loading is None else set(loading)
    if resolved in active:
        raise ValueError(f"cyclic performance scenario inheritance: {resolved}")
    active.add(resolved)
    scenario = json.loads(resolved.read_text(encoding="utf-8"))
    parent_name = scenario.pop("extends", None)
    additive_targets = list(scenario.pop("watcher_targets_add", []))
    if parent_name is None:
        if additive_targets:
            raise ValueError("watcher_targets_add requires an extended scenario")
        return scenario
    if Path(str(parent_name)).name != str(parent_name):
        raise ValueError(f"scenario extends must name a sibling file: {parent_name}")
    parent_path = (resolved.parent / str(parent_name)).resolve()
    if parent_path.parent != resolved.parent:
        raise ValueError(f"scenario extends escaped its directory: {parent_name}")
    merged = load_scenario(parent_path, active)
    for key, value in scenario.items():
        if isinstance(value, dict) and isinstance(merged.get(key), dict):
            merged[key] = {**dict(merged[key]), **value}
        else:
            merged[key] = value
    merged["watcher_targets"] = merge_watcher_target_specs(
        list(merged.get("watcher_targets", [])),
        [str(value) for value in additive_targets],
    )
    return merged


def scenario_environment(scenario: dict[str, object]) -> dict[str, str]:
    """Validate the narrow environment surface allowed to a workload fixture.
    校验性能 fixture 可使用的受限环境变量表面。
    """
    configured = scenario.get("workload_environment", {})
    if not isinstance(configured, dict):
        raise ValueError("workload_environment must be an object")
    environment: dict[str, str] = {}
    for key, value in configured.items():
        name = str(key)
        if not name.startswith("KBE_PERF_"):
            raise ValueError(f"unsupported workload environment key: {name}")
        if not isinstance(value, (str, int, float)):
            raise ValueError(f"workload environment value must be scalar: {name}")
        environment[name] = str(value)
    return environment


def scenario_cluster_environment(scenario: dict[str, object]) -> dict[str, str]:
    """Validate performance-only variables inherited by server components.
    校验由服务端组件继承的压测专用环境变量。
    """
    configured = scenario.get("cluster_environment", {})
    if not isinstance(configured, dict):
        raise ValueError("cluster_environment must be an object")
    environment: dict[str, str] = {}
    for key, value in configured.items():
        name = str(key)
        if not name.startswith("KBE_PERF_"):
            raise ValueError(f"unsupported cluster environment key: {name}")
        if not isinstance(value, (str, int, float)):
            raise ValueError(f"cluster environment value must be scalar: {name}")
        environment[name] = str(value)
    return environment


def _repository_root() -> Path:
    return Path(__file__).resolve().parents[4]


def resolve_server_binary_dir(configured: Path | None, start_cluster: bool) -> Path | None:
    """Resolve the stable runtime only when the runner owns the cluster.
    仅在运行器接管集群时解析稳定运行目录，避免覆盖自定义 workload 命令。
    """
    if configured is not None:
        return configured.resolve()
    if start_cluster:
        return (_repository_root() / "kbe/bin/server").resolve()
    return None


def load_thresholds(scenario_path: Path, scenario: dict[str, object], explicit: Path | None) -> dict[str, object]:
    path = explicit
    if path is None and scenario.get("thresholds"):
        path = scenario_path.parent / str(scenario["thresholds"])
    if path is None:
        return {}
    return json.loads(path.resolve().read_text(encoding="utf-8"))


def watcher_unit(metric: str) -> str:
    lowered = metric.lower()
    if "/cprofiles/" in lowered and lowered.endswith(("lasttime", "sumtime", "lastinttime", "suminttime")):
        return "stamps"
    if lowered.endswith("micros"):
        return "micros"
    if "bytes" in lowered:
        return "bytes"
    if any(token in lowered for token in ("count", "total", "clients", "destroyed", "errors")):
        return "count"
    return ""


def watcher_instance(target: WatcherTarget) -> str:
    """Preserve the resolved process identity in raw performance samples.
    在原始性能样本中保留已解析的进程身份，避免同类型多实例被提前合并。

    Direct endpoint targets predate component discovery and retain their historical
    type label for report compatibility. Expanded discovery targets carry a stable
    ``name#componentID`` selector that distinguishes every process.
    直连目标为兼容旧报告继续使用类型标签；发现并展开的目标携带稳定的
    ``名称#组件ID`` 选择器，可区分每个进程。
    """
    return target.component_name or target.component_type


def query_watcher_batch(
    collector: WatcherCollector,
    targets: list[WatcherTarget],
    concurrency: int,
) -> list[tuple[WatcherTarget, float, float, dict[str, object] | None, Exception | None]]:
    """Query independent component/path connections concurrently and preserve target order.
    并行查询相互独立的组件/路径连接，并按输入顺序返回，保证报告可复现。
    """
    if not targets:
        return []

    def query_one(
        target: WatcherTarget,
    ) -> tuple[WatcherTarget, float, float, dict[str, object] | None, Exception | None]:
        started = monotonic_milliseconds()
        try:
            values = collector.query(target)
        except (OSError, RuntimeError, TimeoutError, ValueError) as exc:
            return target, started, monotonic_milliseconds(), None, exc
        return target, started, monotonic_milliseconds(), values, None

    worker_count = min(max(int(concurrency), 1), len(targets))
    with ThreadPoolExecutor(max_workers=worker_count) as executor:
        return list(executor.map(query_one, targets))


def record_watcher_samples(
    recorder: JsonlRecorder,
    target: WatcherTarget,
    values: dict[str, object],
    sample_now: float,
    stamps_per_second: dict[tuple[str, str, int], float],
    cprofile_previous: dict[tuple[str, str, int, str], tuple[int, float, float, float]],
    counter_previous: dict[tuple[str, str, int, str], tuple[float, float]] | None = None,
) -> None:
    """Record raw Watcher values and normalized cprofile window metrics.
    记录原始 Watcher 值，并把 cprofile 累计 stamp 归一化为可发布的窗口指标。
    """
    instance = watcher_instance(target)
    endpoint = target.component_name or target.host
    endpoint_port = 0 if target.component_name else target.port
    component_key = (target.component_type, endpoint, endpoint_port)
    if target.path.endswith("/stats"):
        stamp_rate = values.get("stampsPerSecond")
        if isinstance(stamp_rate, (int, float)) and stamp_rate > 0:
            stamps_per_second[component_key] = float(stamp_rate)

    stamp_rate = stamps_per_second.get(component_key)
    latency_stat_metrics = {
        "meanMicros",
        "p50Micros",
        "p95Micros",
        "p99Micros",
        "p999Micros",
        "maxMicros",
    }
    has_latency_distribution = "count" in values and any(
        metric in values for metric in latency_stat_metrics
    )
    latency_count = values.get("count") if has_latency_distribution else None
    network_counter_rates = {
        "numPacketsSent": ("packetsSentPerSecond", "packets/s"),
        "numPacketsReceived": ("packetsReceivedPerSecond", "packets/s"),
        "numBytesSent": ("bytesSentPerSecond", "bytes/s"),
        "numBytesReceived": ("bytesReceivedPerSecond", "bytes/s"),
    }
    for metric, value in values.items():
        metric_name = f"{target.path}/{metric}".replace("//", "/")
        if isinstance(value, str):
            if value:
                recorder.record(
                    "watcher",
                    instance,
                    metric_name,
                    value,
                    tags={"kind": "label"},
                )
            continue
        if not isinstance(value, (int, float)):
            continue
        if latency_count == 0 and metric in latency_stat_metrics:
            continue
        if metric in ("p999Micros", "latency/p999Micros") and not values.get(
            metric.replace("p999Micros", "p999Available"), False
        ):
            continue
        recorder.record_sample(
            "watcher",
            instance,
            metric_name,
            value,
            watcher_unit(metric_name),
        )
        rate_definition = network_counter_rates.get(metric) if target.path in (
            "root/network",
            "root/bots/performance",
        ) else None
        if counter_previous is not None and rate_definition is not None:
            counter_key = (*component_key, metric)
            current = (float(value), sample_now)
            previous_counter = counter_previous.get(counter_key)
            counter_previous[counter_key] = current
            if previous_counter is not None and current[0] >= previous_counter[0]:
                elapsed = max(current[1] - previous_counter[1], 1e-6)
                recorder.record_sample(
                    "watcher",
                    instance,
                    f"{target.path}/rates/{rate_definition[0]}",
                    (current[0] - previous_counter[0]) / elapsed,
                    rate_definition[1],
                )
        if stamp_rate and "/cprofiles/" in target.path and metric in (
            "lastTime",
            "sumTime",
            "lastIntTime",
            "sumIntTime",
        ):
            recorder.record_sample(
                "watcher",
                instance,
                f"{target.path}/{metric}Micros",
                float(value) / stamp_rate * 1_000_000.0,
                "micros",
            )

    if not stamp_rate or "/cprofiles/" not in target.path:
        return
    count = values.get("count")
    sum_time = values.get("sumTime")
    sum_int_time = values.get("sumIntTime")
    if not all(isinstance(value, (int, float)) for value in (count, sum_time, sum_int_time)):
        return
    profile_key = (*component_key, target.path)
    current = (int(count), float(sum_time), float(sum_int_time), sample_now)
    previous = cprofile_previous.get(profile_key)
    cprofile_previous[profile_key] = current
    if previous is None:
        return
    delta_count = max(current[0] - previous[0], 0)
    delta_sum_micros = max(current[1] - previous[1], 0.0) / stamp_rate * 1_000_000.0
    delta_self_micros = max(current[2] - previous[2], 0.0) / stamp_rate * 1_000_000.0
    elapsed = max(sample_now - previous[3], 1e-6)
    prefix = f"{target.path}/window"
    recorder.record_sample("watcher", instance, f"{prefix}/callCount", delta_count, "count")
    recorder.record_sample("watcher", instance, f"{prefix}/callsPerSecond", delta_count / elapsed, "count/s")
    recorder.record_sample("watcher", instance, f"{prefix}/totalMicros", delta_sum_micros, "micros")
    recorder.record_sample("watcher", instance, f"{prefix}/selfMicros", delta_self_micros, "micros")
    if delta_count > 0:
        recorder.record_sample("watcher", instance, f"{prefix}/meanMicros", delta_sum_micros / delta_count, "micros")
        recorder.record_sample("watcher", instance, f"{prefix}/meanSelfMicros", delta_self_micros / delta_count, "micros")


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
    processes: list[subprocess.Popen[bytes]],
    watcher_collector: WatcherCollector | None,
    watcher_targets: list[WatcherTarget],
    log_roots: list[Path],
    timeout_seconds: float,
    readiness: dict[str, object],
    configured_bots: int,
    cluster: PerformanceCluster | None = None,
    query_concurrency: int = 1,
) -> list[WatcherTarget]:
    """Exclude process and Watcher startup from the measured request window.
    将进程和 Watcher 启动期排除在请求成功率与延迟窗口之外。
    """
    if not processes or watcher_collector is None or not watcher_targets:
        return watcher_targets
    deadline = time.monotonic() + timeout_seconds
    last_error: Exception | None = None
    last_observed: dict[str, object] = {}
    last_resolved_targets: list[WatcherTarget] = []
    resolved_targets: list[WatcherTarget] | None = None
    while time.monotonic() < deadline:
        if cluster is not None:
            cluster.assert_running("workload readiness")
        workload_exit = first_workload_exit(processes)
        if workload_exit is not None:
            raise WorkloadReadinessError(
                f"workload exited before readiness with code {workload_exit}",
                last_observed,
            )
        try:
            if resolved_targets is None:
                if hasattr(watcher_collector, "resolve_targets"):
                    resolved_targets = watcher_collector.resolve_targets(
                        watcher_targets, log_roots, deadline,
                    )
                else:
                    resolved_targets = [
                        (
                            watcher_collector.resolve_target(target, log_roots)
                            if hasattr(watcher_collector, "resolve_target")
                            else resolve_target(target, log_roots)
                        )
                        for target in watcher_targets
                    ]
            last_resolved_targets = resolved_targets
            observed: dict[str, object] = {}
            readiness_targets = select_readiness_targets(resolved_targets, readiness)
            aggregate_readiness = len(readiness_targets) > 1
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("workload readiness deadline exhausted")

            # 每个目标拥有独立 Watcher 连接，因此可按组件并行读取；聚合仍在主线程按稳定顺序完成。
            # Every target owns a separate Watcher connection, so component reads may run in
            # parallel while deterministic aggregation remains on the controller thread.
            worker_count = min(max(query_concurrency, 1), max(len(readiness_targets), 1))
            with ThreadPoolExecutor(max_workers=worker_count) as executor:
                query_results = list(executor.map(
                    lambda target: watcher_collector.query(target, remaining),
                    readiness_targets,
                ))

            for target, values in zip(readiness_targets, query_results):
                for metric, value in values.items():
                    full_metric = f"{target.path}/{metric}".replace("//", "/")
                    if aggregate_readiness and full_metric in readiness:
                        previous = observed.get(full_metric, 0)
                        if not isinstance(previous, (int, float)) or not isinstance(
                            value, (int, float)
                        ):
                            raise ValueError(
                                f"multi-process readiness metric must be numeric: {full_metric}"
                            )
                        observed[full_metric] = previous + value
                    elif aggregate_readiness:
                        observed[f"{target.component_name}/{full_metric}"] = value
                    else:
                        observed[full_metric] = value
            last_observed = observed
            for metric, expected in readiness.items():
                if not readiness_value_matches(observed.get(metric), expected, configured_bots):
                    raise RuntimeError(
                        f"readiness {metric}={observed.get(metric)!r}, expected={expected!r}"
                    )
            return resolved_targets
        except (LookupError, OSError, RuntimeError, TimeoutError, ValueError) as exc:
            last_error = exc
            time.sleep(0.25)
    failure_snapshot = collect_readiness_failure_snapshot(
        watcher_collector,
        last_resolved_targets,
        last_observed,
        time.monotonic() + min(watcher_collector.timeout_seconds, 10.0),
    )
    raise WorkloadReadinessError(
        f"workload readiness timed out after {timeout_seconds:.1f}s: {last_error}",
        failure_snapshot,
    )


def collect_readiness_failure_snapshot(
    watcher_collector: WatcherCollector,
    resolved_targets: list[WatcherTarget],
    readiness_observed: dict[str, object],
    deadline: float | None = None,
) -> dict[str, object]:
    """Collect one per-component diagnostic snapshot after readiness has failed.
    readiness 失败后采集一次按组件区分的诊断快照。

    Normal polling deliberately queries only readiness owners. At failure time the
    KCP/IOCP state is more valuable than another blind retry, but querying both
    ``root`` and all of its children would duplicate work. Keep only leaf targets
    for each resolved component and preserve the original aggregate readiness keys.
    正常轮询只查询 readiness 所属目标；失败时再保留每个组件的 KCP/IOCP 状态。
    同一组件只查询叶子路径，避免同时读取 ``root`` 及其全部子树造成重复开销。
    """
    snapshot = dict(readiness_observed)
    leaf_targets = [
        target
        for target in resolved_targets
        if not any(
            other is not target
            and _watcher_component_identity(other) == _watcher_component_identity(target)
            and other.path.startswith(f"{target.path}/")
            for other in resolved_targets
        )
    ]
    for target in leaf_targets:
        remaining = None if deadline is None else deadline - time.monotonic()
        if remaining is not None and remaining <= 0:
            break
        try:
            values = watcher_collector.query(target, remaining)
        except (LookupError, OSError, RuntimeError, TimeoutError, ValueError):
            continue
        instance = target.component_name or (
            f"{target.component_type}@{target.host}:{target.port}"
        )
        for metric, value in values.items():
            full_metric = f"{target.path}/{metric}".replace("//", "/")
            snapshot[f"{instance}/{full_metric}"] = value
    return snapshot


def _watcher_component_identity(target: WatcherTarget) -> tuple[str, str, int, str]:
    """Return the resolved component identity independently of its Watcher path.
    返回与 Watcher 路径无关的已解析组件身份。
    """
    return target.component_type, target.host, target.port, target.component_name


def select_readiness_targets(
    watcher_targets: list[WatcherTarget],
    readiness: dict[str, object],
) -> list[WatcherTarget]:
    """Select the most specific Watcher owner for every readiness metric.
    为每个就绪指标选择路径最具体的 Watcher 数据源。

    A generic ``root`` target also prefixes every nested metric. Querying it during
    readiness would repeatedly walk the full BaseApp/CellApp tree and could overwrite
    a dedicated ``root/bots/performance`` result. Longest-prefix ownership keeps the
    control-plane load bounded and prevents cross-component metric contamination.
    通用 ``root`` 路径也会匹配所有子指标；最长前缀归属可避免反复遍历完整
    BaseApp/CellApp 指标树，并防止其覆盖专用的 Bots 就绪数据。
    """
    selected: set[int] = set()
    for metric in readiness:
        candidates = [
            (index, target)
            for index, target in enumerate(watcher_targets)
            if metric == target.path or metric.startswith(f"{target.path}/")
        ]
        if not candidates:
            continue
        longest_path = max(len(target.path) for _, target in candidates)
        owners = [
            (index, target)
            for index, target in candidates
            if len(target.path) == longest_path
        ]
        owner_shapes = {(target.component_type, target.path) for _, target in owners}
        if len(owner_shapes) != 1:
            descriptions = ", ".join(
                f"{target.component_type}={target.host}:{target.port}:{target.path}"
                for _, target in owners
            )
            raise ValueError(f"ambiguous readiness target for {metric}: {descriptions}")
        selected.update(index for index, _ in owners)
    return [target for index, target in enumerate(watcher_targets) if index in selected]


def readiness_value_matches(observed: object, expected: object, configured_bots: int) -> bool:
    """Match exact readiness values or an explicit numeric minimum.
    匹配精确就绪值，或显式声明的数值下限。
    """
    expected_value = configured_bots if expected == "$bots" else expected
    if isinstance(expected_value, dict):
        if set(expected_value) != {"min"}:
            raise ValueError(f"unsupported readiness predicate: {expected_value!r}")
        minimum = expected_value["min"]
        return (
            isinstance(observed, (int, float))
            and isinstance(minimum, (int, float))
            and observed >= minimum
        )
    return observed == expected_value


def first_workload_exit(processes: list[subprocess.Popen[bytes]]) -> int | None:
    """Return the first observed workload exit code, if all members should be alive.
    当压测组成员本应存活时，返回首个已经退出的进程代码。
    """
    for process in processes:
        result = process.poll()
        if result is not None:
            return result
    return None


def wait_for_warmup(
    processes: list[subprocess.Popen[bytes]],
    seconds: float,
    cluster: PerformanceCluster | None = None,
) -> None:
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if cluster is not None:
            cluster.assert_running("warmup")
        workload_exit = first_workload_exit(processes)
        if workload_exit is not None:
            raise RuntimeError(f"workload exited during warmup with code {workload_exit}")
        time.sleep(min(0.25, max(deadline - time.monotonic(), 0.0)))


def drain_watcher_windows(
    watcher_collector: WatcherCollector,
    watcher_targets: list[WatcherTarget],
    concurrency: int = 1,
) -> list[tuple[WatcherTarget, Exception]]:
    """Discard readiness/warmup watcher windows before steady-state sampling.
    丢弃 readiness/warmup 阶段累积的窗口指标，避免启动排队污染稳态 P99。

    A control-plane timeout must not abort a healthy workload after readiness.
    Measurement sampling already isolates queries per target, so preserve the same
    failure boundary here and expose failures as metrics instead.
    readiness 已通过后，控制面超时不能终止健康负载。稳态采样本就按目标隔离
    查询失败，因此窗口清理保持相同边界，并将失败作为指标暴露。
    """
    return [
        (target, error)
        for target, _started, _finished, _values, error in query_watcher_batch(
            watcher_collector, watcher_targets, concurrency
        )
        if error is not None
    ]


def stop_process(process: subprocess.Popen[bytes] | None) -> None:
    if not process or process.poll() is not None:
        return
    if os.name == "nt":
        process.terminate()
    else:
        process.send_signal(signal.SIGTERM)


def stop_processes(processes: list[subprocess.Popen[bytes]]) -> None:
    """Stop every owned workload process. / 停止本轮拥有的全部压测进程。"""
    for process in processes:
        stop_process(process)
    # 先通知全部成员，再逐个等待，避免串行等待期间其他成员继续产生负载。
    # Notify every member first, then wait individually so peers cannot keep
    # producing load while an earlier process consumes its shutdown timeout.
    for process in processes:
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


if __name__ == "__main__":
    raise SystemExit(main())
