"""Contract tests for the Performance Lab. / 性能实验室契约测试。"""

import ast
import tempfile
import sys
from pathlib import Path
import xml.etree.ElementTree as ET

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from performance.assets import create_config_overlay, resolve_bots_schedule, resolve_fixture_root
from performance.log_metrics import IncrementalLogCollector
from performance.metrics import JsonlRecorder
from performance.process_metrics import ProcessCollector, _parse_windows_process_row
from performance.report import build_summary, load_events, validate_event
from performance.run import _repository_root
from performance.watcher_metrics import parse_target, resolve_target


def assert_fixture_callbacks() -> None:
    fixture_root = resolve_fixture_root("network-baseline")
    required = {
        "base/kbemain.py": ("onInit", "onBaseAppReady", "onReadyForLogin"),
        "cell/kbemain.py": ("onInit",),
        "db/kbemain.py": ("onDBMgrReady", "onSelectAccountDBInterface"),
        "interface/kbemain.py": ("onInterfaceAppReady",),
        "logger/kbemain.py": ("onLoggerAppReady",),
        "login/kbemain.py": ("onLoginAppReady", "onLoginCallbackFromDB", "onCreateAccountCallbackFromDB"),
    }
    for relative, callbacks in required.items():
        module = ast.parse((fixture_root / relative).read_text(encoding="utf-8"))
        functions = {node.name for node in module.body if isinstance(node, ast.FunctionDef)}
        assert set(callbacks).issubset(functions), f"missing fixture callback in {relative}"

    avatar_def = ET.parse(fixture_root / "entity_defs/Avatar.def").getroot()
    client_methods = avatar_def.findall("./ClientMethods")
    assert len(client_methods) == 1, "Avatar.def must contain one ClientMethods section"
    assert client_methods[0].find("./pythonPerformanceProbeResponse") is not None
    avatar_source = (fixture_root / "bots/Avatar.py").read_text(encoding="utf-8")
    assert "KBE_PERF_PYTHON_RTT_INTERVAL" in avatar_source
    assert "max(0.1, min(60.0, interval))" in avatar_source
    assert "KBE_PERF_PYTHON_RTT_SAMPLE_EVERY" in avatar_source
    assert "max(1, min(10000, every))" in avatar_source


def main() -> int:
    assert_fixture_callbacks()
    repository_root = _repository_root()
    assert repository_root.is_dir()
    assert (repository_root / "kbe/src/tests/performance/run.py").is_file()
    process_row = {
        "CPU": 12.5,
        "WorkingSet64": 101,
        "PrivateMemorySize64": 102,
        "PeakWorkingSet64": 103,
        "ThreadCount": 7,
        "Handles": 19,
    }
    assert _parse_windows_process_row(process_row) == (12.5, 101, 102, 103, 7, 19)
    process_collector = ProcessCollector(1)
    process_sample = process_collector._finish(*_parse_windows_process_row(process_row))
    assert process_sample.working_set_bytes == 101
    assert process_sample.private_bytes == 102
    assert process_sample.peak_working_set_bytes == 103
    assert process_sample.thread_count == 7 and process_sample.handle_count == 19
    target = parse_target("BOTS_TYPE=127.0.0.1:11000:root/bots/network/poller")
    assert target.component_type == "BOTS_TYPE"
    assert target.port == 11000
    assert target.path.endswith("poller")
    discovered = parse_target("BOTS_TYPE=@bots:root/bots")
    assert discovered.component_name == "bots"
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        fixture = resolve_fixture_root("network-baseline")
        assert fixture is not None and (fixture / "entity_defs/Avatar.def").is_file()
        tick_time, tick_count = resolve_bots_schedule({"connect_rate_per_second": 25}, 500)
        assert tick_time == 0.08 and tick_count == 2
        assert resolve_bots_schedule({}, 500) == (0.1, 50)
        log_root = root / "logs"
        log_root.mkdir()
        (log_root / "machine.log").write_text(
            "componentType:bots, intaddr:127.0.0.1, intport:13501\n",
            encoding="utf-8",
        )
        resolved = resolve_target(discovered, [log_root])
        assert resolved.host == "127.0.0.1" and resolved.port == 13501
        assets = root / "assets/res/server"
        assets.mkdir(parents=True)
        (assets / "kbengine.xml").write_text("<root><bots /></root>\n", encoding="utf-8")
        overlay = create_config_overlay(root / "assets", root / "run", 500, external_receive_messages=1024, external_receive_bytes=1048576)
        xml_root = ET.parse(overlay).getroot()
        assert xml_root.findtext("./bots/defaultAddBots/totalCount") == "500"
        assert xml_root.findtext("./channelCommon/windowOverflow/receive/messages/external") == "1024"
        assert xml_root.findtext("./channelCommon/windowOverflow/receive/bytes/external") == "1048576"
        log_path = root / "streamed.log"
        log_path.write_text("ordinary line\nWARNING split across chunks\nERROR final\n", encoding="utf-8")
        log_collector = IncrementalLogCollector(root)
        log_collector.READ_CHUNK_CHARS = 7
        log_counts = log_collector.sample()
        assert log_counts["warning"] == 1 and log_counts["error"] == 1
        assert log_collector.sample()["warning"] == 0
        path = Path(directory) / "raw.jsonl"
        with JsonlRecorder(path, "test-run", "contract") as recorder:
            for latency in (10, 20, 30, 40, 100):
                recorder.record_request_latency("bots", "1", "heartbeat", 0, latency, latency < 90, "timeout" if latency >= 90 else "", 50)
            recorder.record("logs", "all", "log.error.count", 2, "count")
        events = list(load_events(path))
        for event in events:
            validate_event(event)
        summary = build_summary(events)
        assert summary["latency"]["heartbeat"]["count"] == 5
        assert summary["latency"]["heartbeat"]["p99_ms"] >= 40
        assert summary["counters"]["log.error.count"] == 2
        assert summary["phase_counters"] == {}
        phase_path = Path(directory) / "phase.jsonl"
        with JsonlRecorder(phase_path, "test-run", "contract") as recorder:
            recorder.record("logs", "all", "log.error.count", 3, "count", {"kind": "counter", "phase": "shutdown"})
            recorder.record("logs", "all", "log.error.count", 1, "count", {"kind": "counter", "phase": "measurement"})
            recorder.record("runner", "workload", "process.exit.count", 1, "count", {"kind": "counter"})
        phase_summary = build_summary(load_events(phase_path), {"max_log_errors": 0})
        assert phase_summary["counters"]["log.error.count"] == 4
        assert phase_summary["phase_counters"]["shutdown"]["log.error.count"] == 3
        assert phase_summary["quality"]["status"] == "BLOCKER"
        assert "workload process exits=1" in phase_summary["quality"]["blockers"]
        log_only_summary = build_summary(load_events(phase_path), {"max_log_errors": 1})
        assert not any(item.startswith("log errors=") for item in log_only_summary["quality"]["blockers"])
        assert summary["requests"]["success"] == 4
        assert summary["requests"]["failures"] == 1
        assert summary["counters"]["request.slow.count"] == 1
        assert summary["quality"]["status"] == "BLOCKER"
        recorder_path = root / "samples.jsonl"
        with JsonlRecorder(recorder_path, "test-run", "contract") as recorder:
            recorder.record_sample("watcher", "BOTS_TYPE", "bots/tick/lastMicros", 1000, "micros")
            recorder.record_sample("watcher", "BOTS_TYPE", "bots/tick/lastMicros", 3000, "micros")
            recorder.record_sample("watcher", "BOTS_TYPE", "bots/performance/tickMaxMicros", 100000, "micros")
        sampled = build_summary(load_events(recorder_path), {"tick_p99_max_ms": 2.0})
        assert sampled["samples"]["watcher.BOTS_TYPE.bots/tick/lastMicros"]["count"] == 2
        assert sampled["samples"]["watcher.BOTS_TYPE.bots/tick/lastMicros"]["p99"] > 2000
        assert sampled["quality"]["status"] == "SLOW"
        assert all("tickMaxMicros" not in item for item in sampled["quality"]["slow"])
    print("PERFORMANCE_METRICS_CONTRACT_TEST_PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
