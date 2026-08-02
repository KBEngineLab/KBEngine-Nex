"""Contract tests for the Performance Lab. / 性能实验室契约测试。"""

import ast
import types
import tempfile
import sys
from pathlib import Path
import xml.etree.ElementTree as ET

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from performance.assets import create_config_overlay, resolve_bots_schedule, resolve_fixture_root
from performance.cluster import PerformanceCluster
from performance.log_metrics import IncrementalLogCollector
from performance.metrics import JsonlRecorder
from performance.process_metrics import ProcessCollector, _parse_windows_process_row
from performance.report import build_summary, load_events, validate_event
from performance.run import (
    _repository_root,
    expand_bots_watcher_targets,
    expand_component_watcher_targets,
    load_scenario,
    merge_watcher_target_specs,
    partition_workload_bots,
    record_watcher_samples,
    readiness_value_matches,
    select_readiness_targets,
    scenario_environment,
    wait_for_workload_ready,
)
from performance.watcher_metrics import WatcherCollector, WatcherSchedule, parse_target, resolve_target


def assert_watcher_connection_reuse() -> None:
    class FakeWatcher:
        instances = []
        fail_next = False

        def __init__(self, component_type):
            self.watchData = []
            self.connect_count = 0
            self.clear_count = 0
            self.query_count = 0
            self.close_count = 0
            FakeWatcher.instances.append(self)

        def connect(self, host, port):
            self.connect_count += 1

        def clearWatchData(self):
            self.clear_count += 1
            self.watchData = []

        def requireQueryWatcher(self, path):
            self.query_count += 1

        def processOne(self, timeout):
            if FakeWatcher.fail_next:
                FakeWatcher.fail_next = False
                raise OSError("connection reset")
            self.watchData.append({"values": {"clients": self.query_count}})

        def close(self):
            self.close_count += 1

    fake_pycommon = types.ModuleType("pycommon")
    fake_pycommon.Define = types.SimpleNamespace(BOTS_TYPE=9)
    fake_pycommon.Watcher = types.SimpleNamespace(Watcher=FakeWatcher)
    previous = sys.modules.get("pycommon")
    sys.modules["pycommon"] = fake_pycommon
    try:
        collector = WatcherCollector(Path("."), timeout_seconds=0.1)
        target = parse_target("BOTS_TYPE=127.0.0.1:11000:root/bots")
        assert collector.query(target)["clients"] == 1
        first_stats = collector.last_query_stats(target)
        assert first_stats is not None
        assert first_stats.value_count == 1
        assert first_stats.response_bytes_estimated > 0
        assert not first_stats.connection_reused
        assert collector.query(target)["clients"] == 2
        second_stats = collector.last_query_stats(target)
        assert second_stats is not None and second_stats.connection_reused
        assert len(FakeWatcher.instances) == 1
        second_path = parse_target("BOTS_TYPE=127.0.0.1:11000:root/network")
        assert collector.query(second_path)["clients"] == 1
        assert len(FakeWatcher.instances) == 2
        watcher = FakeWatcher.instances[0]
        assert watcher.connect_count == 1 and watcher.clear_count == 2
        collector.close()
        assert watcher.close_count == 1
        FakeWatcher.fail_next = True
        reconnecting = WatcherCollector(Path("."), timeout_seconds=0.1)
        try:
            try:
                reconnecting.query(target)
            except OSError:
                pass
            else:
                raise AssertionError("failed Watcher query must propagate")
            assert reconnecting.query(target)["clients"] == 1
            assert len(FakeWatcher.instances) == 4
            assert FakeWatcher.instances[2].close_count == 1
        finally:
            reconnecting.close()
    finally:
        if previous is None:
            sys.modules.pop("pycommon", None)
        else:
            sys.modules["pycommon"] = previous


def assert_watcher_schedule() -> None:
    bots = parse_target("BOTS_TYPE=127.0.0.1:11000:root/bots/performance")
    cell = parse_target("CELLAPP_TYPE=127.0.0.1:12000:root/witness")
    schedule = WatcherSchedule(
        [bots, cell],
        1.0,
        {"BOTS_TYPE:root/bots/performance": 5.0},
    )
    assert schedule.interval(bots) == 5.0
    assert schedule.interval(cell) == 1.0
    assert schedule.due(bots, 100.0)
    schedule.mark_sampled(bots, 100.0)
    assert not schedule.due(bots, 104.89)
    assert schedule.due(bots, 104.9)
    schedule.mark_sampled(bots, 104.9)
    assert not schedule.due(bots, 109.89)
    assert schedule.due(bots, 109.9)
    assert schedule.due(cell, 104.9)
    schedule.mark_sampled(cell, 100.0)
    assert not schedule.due(cell, 100.89)
    assert schedule.due(cell, 100.9)
    schedule.mark_sampled(cell, 102.2)
    assert not schedule.due(cell, 102.89)
    assert schedule.due(cell, 102.9)
    second_cell = parse_target("CELLAPP_TYPE=127.0.0.1:12001:root/witness")
    multi_instance = WatcherSchedule([cell, second_cell], 1.0)
    multi_instance.mark_sampled(cell, 100.0)
    assert not multi_instance.due(cell, 100.5)
    assert multi_instance.due(second_cell, 100.5)
    discovered = parse_target("BOTS_TYPE=@bots:root/bots/performance")
    discovered_schedule = WatcherSchedule([discovered], 1.0)
    resolved_discovered = type(discovered)(
        discovered.component_type,
        "127.0.0.1",
        13501,
        discovered.path,
        discovered.component_name,
    )
    assert discovered_schedule.interval(resolved_discovered) == 1.0
    assert discovered_schedule.due(resolved_discovered, 100.0)
    try:
        WatcherSchedule([bots], 1.0, {"BOTS_TYPE:root/unknown": 5.0})
    except ValueError as exc:
        assert "unknown target" in str(exc)
    else:
        raise AssertionError("unknown Watcher interval target must fail")
    try:
        WatcherSchedule([bots], 1.0, {"BOTS_TYPE:root/bots/performance": 0.0})
    except ValueError as exc:
        assert ">= 0.1" in str(exc)
    else:
        raise AssertionError("invalid Watcher interval must fail")


def assert_cprofile_window_metrics() -> None:
    stats = parse_target("BASEAPP_TYPE=127.0.0.1:12000:root/stats")
    profile = parse_target("BASEAPP_TYPE=127.0.0.1:12000:root/cprofiles/default/scriptCall")
    latency_profile = parse_target(
        "BASEAPP_TYPE=127.0.0.1:12000:root/cprofiles/default/scriptCall/latency"
    )
    stamps = {}
    previous = {}
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "cprofile.jsonl"
        with JsonlRecorder(path, "test-run", "contract") as recorder:
            record_watcher_samples(recorder, stats, {"stampsPerSecond": 1000}, 1.0, stamps, previous)
            record_watcher_samples(
                recorder,
                profile,
                {"count": 10, "sumTime": 100, "sumIntTime": 80, "lastTime": 5},
                1.0,
                stamps,
                previous,
            )
            record_watcher_samples(
                recorder,
                latency_profile,
                {
                    "count": 0,
                    "meanMicros": 0,
                    "p50Micros": 0,
                    "p95Micros": 0,
                    "p99Micros": 0,
                    "p999Available": 0,
                    "p999Micros": 0,
                    "maxMicros": 0,
                },
                4.0,
                stamps,
                previous,
            )
            record_watcher_samples(
                recorder,
                latency_profile,
                {"count": 10000, "meanMicros": 12, "p999Available": 1, "p999Micros": 123},
                5.0,
                stamps,
                previous,
            )
            record_watcher_samples(
                recorder,
                profile,
                {"count": 14, "sumTime": 140, "sumIntTime": 110, "lastTime": 6},
                3.0,
                stamps,
                previous,
            )
        summary = build_summary(load_events(path))
        prefix = "watcher.BASEAPP_TYPE.root/cprofiles/default/scriptCall"
        assert summary["samples"][f"{prefix}/lastTimeMicros"]["max"] == 6000
        assert summary["samples"][f"{prefix}/window/callCount"]["max"] == 4
        assert summary["samples"][f"{prefix}/window/callsPerSecond"]["max"] == 2
        assert summary["samples"][f"{prefix}/window/meanMicros"]["max"] == 10000
        assert summary["samples"][f"{prefix}/window/meanSelfMicros"]["max"] == 7500
        assert summary["samples"][f"{prefix}/latency/meanMicros"]["count"] == 1
        assert summary["samples"][f"{prefix}/latency/p999Micros"]["count"] == 1
        assert summary["samples"][f"{prefix}/latency/p999Micros"]["max"] == 123


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
    assert "recordPerformanceTransaction" in avatar_source
    assert "_python_probe_pending" in avatar_source
    assert "_python_probe_entity_sequence" in avatar_source
    assert "(_python_probe_entity_sequence - 1)" in avatar_source
    assert "recordPerformanceProbeTimeout" in avatar_source
    assert "max(1, min(10000, every))" in avatar_source


def assert_python_latency_scenario() -> None:
    scenario_path = Path(__file__).resolve().parent / "scenarios/python_latency.json"
    scenario = load_scenario(scenario_path)
    assert scenario["name"] == "python-latency"
    assert len(scenario["watcher_targets"]) == 31
    assert scenario["watcher_intervals"]["BOTS_TYPE:root/bots/performance"] == 5.0
    assert scenario["workload_environment"]["KBE_PERF_PYTHON_RTT_SAMPLE_EVERY"] == "50"
    assert scenario["readiness"]["root/bots/pythonLatency/control/successes"] == {"min": 1}
    assert readiness_value_matches(1, {"min": 1}, 2000)
    assert not readiness_value_matches(0, {"min": 1}, 2000)
    assert readiness_value_matches(2000, "$bots", 2000)
    assert scenario["watcher_intervals"]["BOTS_TYPE:root/bots/pythonLatency/roundTrip"] == 2.0
    environment = scenario_environment(scenario)
    assert environment["KBE_PERF_PYTHON_CHAIN"] == "1"
    try:
        scenario_environment({"workload_environment": {"PATH": "invalid"}})
    except ValueError:
        pass
    else:
        raise AssertionError("unrestricted workload environment key must fail")


def assert_multi_component_cluster_manifest() -> None:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        run_root = root / "run"
        manifest_root = run_root / "server"
        manifest_root.mkdir(parents=True)
        (manifest_root / "components.json").write_text(
            """[
  {"name": "machine", "component_id": 1000, "pid": 1},
  {"name": "baseapp", "component_id": 7001, "pid": 2},
  {"name": "baseapp", "component_id": 7002, "pid": 3}
]\n""",
            encoding="utf-8",
        )
        cluster = PerformanceCluster(root, root, root, run_root, "unused", root)
        processes = cluster.component_processes()
        assert [item.name for item in processes] == ["machine", "baseapp:7001", "baseapp:7002"]
        log_root = manifest_root / "logs"
        log_root.mkdir()
        (log_root / "baseapp.7001.log").write_text("ready marker\nready marker\n", encoding="utf-8")
        assert cluster.wait_for_log_readiness("ready marker", 2, "baseapp*.log", 0.1) == 2


def assert_gameplay_stress_scenario() -> None:
    scenario_path = Path(__file__).resolve().parent / "scenarios/gameplay_10000.json"
    scenario = load_scenario(scenario_path)
    assert scenario["bots"] == 10000
    assert scenario["duration_seconds"] == 300
    assert scenario["connect_rate_per_second"] == 100
    assert scenario["reliable_udp_tick_interval_ms"] == 10
    assert scenario["reliable_udp_min_rto_ms"] == 50
    assert scenario["workload_processes"] == 4
    assert scenario["workload_cid_start"] == 10000
    assert "fixture" not in scenario
    assert scenario["server_readiness"]["min_count"] == 52
    targets = expand_component_watcher_targets(
        [parse_target(value) for value in scenario["watcher_targets"]],
        scenario["watcher_component_ids"],
    )
    assert {target.component_name for target in targets if target.path == "root/network/kcp"} == {
        "baseapp#7001",
        "baseapp#7002",
        "baseapp#7003",
        "cellapp#8001",
        "cellapp#8002",
        "cellapp#8003",
        "cellapp#8004",
        "cellapp#8005",
        "cellapp#8006",
    }
    comparison = load_scenario(scenario_path.with_name("gameplay_10000_kcp20.json"))
    assert comparison["name"] == "gameplay-10000-kcp20"
    assert comparison["reliable_udp_tick_interval_ms"] == 20
    assert comparison["reliable_udp_min_rto_ms"] == 50


def assert_readiness_target_ownership() -> None:
    targets = [
        parse_target("BOTS_TYPE=@bots:root/bots/performance"),
        parse_target("BASEAPP_TYPE=@baseapp:root"),
        parse_target("CELLAPP_TYPE=@cellapp:root"),
    ]
    selected = select_readiness_targets(
        targets,
        {
            "root/bots/performance/clientsTotal": 10000,
            "root/bots/performance/clientsKcp": 10000,
        },
    )
    assert selected == [targets[0]]

    aggregate = [
        parse_target("BOTS_TYPE=127.0.0.1:11000:root/bots/performance"),
        parse_target("BOTS_TYPE=127.0.0.1:11001:root/bots/performance"),
    ]
    assert select_readiness_targets(
        aggregate,
        {"root/bots/performance/clientsTotal": 1},
    ) == aggregate

    ambiguous = [
        parse_target("BOTS_TYPE=127.0.0.1:11000:root/bots/performance"),
        parse_target("CELLAPP_TYPE=127.0.0.1:11001:root/bots/performance"),
    ]
    try:
        select_readiness_targets(ambiguous, {"root/bots/performance/clientsTotal": 1})
    except ValueError as exc:
        assert "ambiguous readiness target" in str(exc)
    else:
        raise AssertionError("ambiguous readiness ownership must be rejected")

    assert partition_workload_bots(10000, 4) == 2500
    try:
        partition_workload_bots(10000, 3)
    except ValueError as exc:
        assert "must be divisible" in str(exc)
    else:
        raise AssertionError("uneven workload partitions must be rejected")

    expanded = expand_bots_watcher_targets([targets[0], targets[1]], 4, 10000)
    assert [target.component_name for target in expanded] == [
        "bots#10000",
        "bots#10001",
        "bots#10002",
        "bots#10003",
        "baseapp",
    ]

    class AliveProcess:
        @staticmethod
        def poll():
            return None

    class AggregateCollector:
        @staticmethod
        def query(target):
            assert target.component_type == "BOTS_TYPE"
            return {"clientsTotal": 2500, "clientsKcp": 2500}

    aggregate_targets = [
        parse_target(
            f"BOTS_TYPE=127.0.0.1:{11000 + index}:root/bots/performance"
        )
        for index in range(4)
    ]
    ready_targets = wait_for_workload_ready(
        [AliveProcess()],
        AggregateCollector(),
        aggregate_targets,
        [],
        0.1,
        {
            "root/bots/performance/clientsTotal": 10000,
            "root/bots/performance/clientsKcp": 10000,
        },
        10000,
    )
    assert ready_targets == aggregate_targets


def main() -> int:
    assert_watcher_connection_reuse()
    assert_watcher_schedule()
    assert_cprofile_window_metrics()
    assert_fixture_callbacks()
    assert_python_latency_scenario()
    assert_multi_component_cluster_manifest()
    assert_gameplay_stress_scenario()
    assert_readiness_target_ownership()
    repository_root = _repository_root()
    assert repository_root.is_dir()
    assert (repository_root / "kbe/src/tests/performance/run.py").is_file()
    bots_client_source = (
        repository_root / "kbe/src/server/tools/bots/clientobject.cpp"
    ).read_text(encoding="utf-8")
    assert bots_client_source.count("state_ = C_STATE_LOGIN_BASEAPP_HELLO;") == 2
    assert "case C_STATE_LOGIN_BASEAPP_HELLO:" in bots_client_source
    assert "onHelloCB_ activates encryption" in bots_client_source
    encryption_source = (
        repository_root / "kbe/src/lib/network/encryption_filter.cpp"
    ).read_text(encoding="utf-8")
    assert "encryptedPayloadLen % BLOCK_SIZE" in encryption_source
    assert "return REASON_CORRUPTED_PACKET;" in encryption_source
    assert "WATCH_OBJECT(\"spaceSize\", this, &Cellapp::spaceSize)" in (
        repository_root / "kbe/src/server/cellapp/cellapp.cpp"
    ).read_text(encoding="utf-8")
    assert merge_watcher_target_specs(["A", "B"], ["B", "C"]) == ["A", "B", "C"]
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
            "componentType:bots, componentID:10000, intaddr:127.0.0.1, intport:13501\n"
            "componentType:bots, componentID:10001, intaddr:127.0.0.1, intport:13502\n",
            encoding="utf-8",
        )
        resolved = resolve_target(discovered, [log_root])
        assert resolved.host == "127.0.0.1" and resolved.port == 13501
        selected_bot = parse_target("BOTS_TYPE=@bots#10001:root/bots")
        resolved_selected_bot = resolve_target(selected_bot, [log_root])
        assert resolved_selected_bot.port == 13502
        assets = root / "assets/res/server"
        assets.mkdir(parents=True)
        (assets / "kbengine.xml").write_text("<root><bots /></root>\n", encoding="utf-8")
        overlay = create_config_overlay(
            root / "assets",
            root / "run",
            500,
            external_receive_messages=1024,
            external_receive_bytes=1048576,
            reliable_udp_tick_interval_ms=20,
            reliable_udp_min_rto_ms=50,
        )
        xml_root = ET.parse(overlay).getroot()
        assert xml_root.findtext("./bots/defaultAddBots/totalCount") == "500"
        assert xml_root.findtext("./channelCommon/windowOverflow/receive/messages/external") == "1024"
        assert xml_root.findtext("./channelCommon/windowOverflow/receive/bytes/external") == "1048576"
        assert xml_root.findtext("./networkInterface/reliableUDP/tickInterval") == "20"
        assert xml_root.findtext("./networkInterface/reliableUDP/minRTO") == "50"
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
        assert summary["latency"]["heartbeat"]["mean_ms"] == 40
        assert summary["latency"]["heartbeat"]["total_ms"] == 200
        assert summary["operation_counters"]["heartbeat"]["request.success.count"] == 4
        assert summary["operation_counters"]["heartbeat"]["request.error.count"] == 1
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
        assert sampled["samples"]["watcher.BOTS_TYPE.bots/tick/lastMicros"]["mean"] == 2000
        assert sampled["samples"]["watcher.BOTS_TYPE.bots/tick/lastMicros"]["total"] == 4000
        assert sampled["samples"]["watcher.BOTS_TYPE.bots/tick/lastMicros"]["p99"] > 2000
        assert sampled["quality"]["status"] == "SLOW"
        assert all("tickMaxMicros" not in item for item in sampled["quality"]["slow"])
    print("PERFORMANCE_METRICS_CONTRACT_TEST_PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
