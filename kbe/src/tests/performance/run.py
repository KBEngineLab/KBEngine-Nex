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

from .assets import build_environment, create_config_overlay, write_scenario_metadata
from .log_metrics import IncrementalLogCollector
from .metrics import JsonlRecorder, LatencyHistogram
from .process_metrics import ProcessCollector
from .report import build_summary, load_events, write_report
from .watcher_metrics import WatcherCollector, parse_target


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run an isolated KBE performance scenario")
    parser.add_argument("--scenario", required=True, type=Path)
    parser.add_argument("--output-root", type=Path, default=Path("kbe/src/out/performance-runs"))
    parser.add_argument("--log-root", type=Path)
    parser.add_argument("--duration", type=float)
    parser.add_argument("--sample-interval", type=float, default=1.0)
    parser.add_argument("--command", help="Command line to run the workload")
    parser.add_argument("--assets-root", type=Path, help="Read-only game assets used for the isolated config overlay")
    parser.add_argument("--bots", type=int, help="Override the scenario Bots count")
    parser.add_argument("--tools-root", type=Path, help="kbe/tools/server root for Watcher queries")
    parser.add_argument("--watcher-target", action="append", default=[], metavar="TYPE=HOST:PORT:PATH")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    scenario = json.loads(args.scenario.read_text(encoding="utf-8"))
    name = str(scenario["name"])
    duration = float(args.duration if args.duration is not None else scenario.get("duration_seconds", 30))
    configured_bots = int(args.bots if args.bots is not None else scenario.get("bots", 0))
    interval = max(float(args.sample_interval), 0.1)
    run_id = f"{time.strftime('%Y%m%d-%H%M%S')}-{uuid.uuid4().hex[:8]}"
    output = args.output_root / f"{name}-{run_id}"
    output.mkdir(parents=True, exist_ok=True)
    environment = None
    if args.assets_root:
        create_config_overlay(
            args.assets_root,
            output,
            configured_bots,
            float(scenario.get("bots_tick_time", 0.1)),
            int(scenario["bots_tick_count"]) if "bots_tick_count" in scenario else None,
        )
        environment = build_environment(_repository_root(), args.assets_root, output)
        write_scenario_metadata(output, scenario, configured_bots)
    elif args.command and configured_bots > 0:
        raise ValueError("--assets-root is required when starting a scenario with Bots")
    process = start_command(args.command, environment, output)
    process_collector = ProcessCollector(process.pid) if process else None
    log_collector = IncrementalLogCollector(args.log_root) if args.log_root else None
    watcher_collector = WatcherCollector(args.tools_root) if args.tools_root and args.watcher_target else None
    watcher_targets = [parse_target(value) for value in args.watcher_target]
    events_path = output / "raw.jsonl"
    try:
        with JsonlRecorder(events_path, run_id, name) as recorder:
            started = time.monotonic()
            while time.monotonic() - started < duration:
                if process and process.poll() is not None:
                    recorder.record("runner", "workload", "process.exit.count", 1, "count")
                    break
                if process_collector:
                    sample = process_collector.sample()
                    if sample:
                        recorder.record("process", str(process.pid), "cpu.percent", sample.cpu_percent, "%")
                        recorder.record("process", str(process.pid), "memory.working_set", sample.working_set_bytes, "bytes")
                        recorder.record("process", str(process.pid), "threads.active", sample.thread_count, "count")
                if log_collector:
                    for metric, value in log_collector.sample().items():
                        recorder.record("logs", "all", f"log.{metric}.count", value, "count")
                if watcher_collector:
                    for target in watcher_targets:
                        try:
                            values = watcher_collector.query(target)
                        except (OSError, RuntimeError, TimeoutError, ValueError) as exc:
                            recorder.record("watcher", target.component_type, "query.error.count", 1, "count", {"kind": "counter", "error": type(exc).__name__})
                            continue
                        for metric, value in values.items():
                            if isinstance(value, (int, float)):
                                recorder.record("watcher", target.component_type, metric, value, "")
                recorder.flush()
                time.sleep(interval)
    finally:
        stop_process(process)
    summary = build_summary(load_events(events_path))
    write_report(summary, output / "summary.json", output / "report.md")
    print(json.dumps({"run_id": run_id, "output": str(output), "summary": summary}, ensure_ascii=True))
    return 0


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
    return subprocess.Popen(
        args,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        env=environment,
        cwd=working_directory,
    )


def _repository_root() -> Path:
    return Path(__file__).resolve().parents[4]


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
