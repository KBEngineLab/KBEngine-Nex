"""Contract tests for the Performance Lab. / 性能实验室契约测试。"""

import ast
import importlib.util
import json
import os
import types
import tempfile
import sys
from pathlib import Path
import xml.etree.ElementTree as ET

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from performance.assets import build_environment, create_config_overlay, resolve_bots_schedule, resolve_fixture_root
from performance.cluster import PerformanceCluster
from performance.log_metrics import IncrementalLogCollector
from performance.metrics import JsonlRecorder
from performance.process_metrics import ProcessCollector, ProcessGroupCollector, _parse_windows_process_row
from performance.report import build_summary, load_events, validate_event
from performance.run import (
    _repository_root,
    collect_readiness_failure_snapshot,
    expand_bots_watcher_targets,
    expand_component_watcher_targets,
    load_scenario,
    manager_readiness_satisfied,
    merge_watcher_target_specs,
    partition_workload_bots,
    record_watcher_samples,
    readiness_value_matches,
    resolve_server_binary_dir,
    select_readiness_targets,
    scenario_cluster_environment,
    scenario_environment,
    start_command,
    wait_for_workload_ready,
    start_workload_commands,
)
from performance.topology import build_local_cluster_components, partition_batch_size
from performance.watcher_metrics import WatcherCollector, WatcherSchedule, parse_target, resolve_target


def load_cluster_controller_module():
    path = Path(__file__).resolve().parent.parent / "python/test_server_cluster.py"
    spec = importlib.util.spec_from_file_location("kbe_test_server_cluster", path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def assert_centralized_discovery_matching() -> None:
    controller = load_cluster_controller_module()
    central_log = (
        "OFF baseappmgr [thread] baseappmgr-1 10103 5000 "
        "Components::process(): Found all the components!\n"
        "OFF cellappmgr [thread] cellappmgr-1 10103 6000 "
        "Components::process(): Found all the components!\n"
    )
    assert controller.centralized_discovery_marker(central_log, "baseappmgr", 5000)
    assert controller.centralized_discovery_marker(central_log, "cellappmgr", 6000)
    assert not controller.centralized_discovery_marker(central_log, "baseappmgr", 6000)
    assert not controller.centralized_discovery_marker(central_log, "interfaces", 3000)

    class AliveProcess:
        @staticmethod
        def poll():
            return None

    controller.wait_for_process_stability(
        [{"process": AliveProcess(), "spec": {"name": "baseappmgr"}}],
        0.01,
    )


def assert_partial_workload_start_cleanup() -> None:
    class FakeProcess:
        def __init__(self):
            self.terminated = False
            self.waited = False

        def poll(self):
            return None

        def terminate(self):
            self.terminated = True

        def wait(self, timeout=None):
            self.waited = True
            return 0

    globals_ = start_workload_commands.__globals__
    original = globals_["start_command"]
    first = FakeProcess()
    calls = 0

    def fail_second(*_args, **_kwargs):
        nonlocal calls
        calls += 1
        if calls == 1:
            return first
        raise FileNotFoundError("missing workload")

    try:
        globals_["start_command"] = fail_second
        try:
            start_workload_commands("bots --cid={cid}", 2, 10000, 40)
            raise AssertionError("partial workload startup should fail")
        except FileNotFoundError as exc:
            assert str(exc) == "missing workload"
        assert first.terminated and first.waited

        globals_["start_command"] = lambda *_args, **_kwargs: (_ for _ in ()).throw(
            FileNotFoundError("first workload missing")
        )
        try:
            start_workload_commands("bots", 1, 10000, 40)
            raise AssertionError("first workload startup should fail")
        except FileNotFoundError as exc:
            assert str(exc) == "first workload missing"
    finally:
        globals_["start_command"] = original


def assert_windows_command_line_preserves_quoted_executable() -> None:
    globals_ = start_command.__globals__
    original_name = globals_["os"].name
    original_popen = globals_["subprocess"].Popen
    observed: dict[str, object] = {}

    class FakeProcess:
        pass

    def capture_popen(args, **kwargs):
        observed["args"] = args
        observed["kwargs"] = kwargs
        return FakeProcess()

    try:
        globals_["os"].name = "nt"
        globals_["subprocess"].Popen = capture_popen
        process = start_command('"C:\\Program Files\\KBE\\bots.exe" --cid=10000')
        assert isinstance(process, FakeProcess)
        assert observed["args"] == '"C:\\Program Files\\KBE\\bots.exe" --cid=10000'
    finally:
        globals_["subprocess"].Popen = original_popen
        globals_["os"].name = original_name


def assert_server_binary_directory_resolution() -> None:
    repository_root = _repository_root()
    expected = (repository_root / "kbe/bin/server").resolve()
    assert resolve_server_binary_dir(None, True) == expected
    assert resolve_server_binary_dir(None, False) is None
    configured = Path("explicit/server/runtime")
    assert resolve_server_binary_dir(configured, True) == configured.resolve()
    environment = build_environment(
        repository_root,
        repository_root / "test-assets",
        repository_root / "test-run",
    )
    assert Path(environment["KBE_BIN_PATH"]) == expected


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

    staggered_targets = [
        parse_target(f"CELLAPP_TYPE=127.0.0.1:{13000 + index}:root/network/poller")
        for index in range(7)
    ]
    staggered = WatcherSchedule(
        staggered_targets,
        1.0,
        {"CELLAPP_TYPE:root/network/poller": 5.0},
    )
    assert [staggered.phase_offset(target) for target in staggered_targets] == [
        0.0, 1.0, 2.0, 3.0, 4.0, 0.0, 1.0,
    ]
    staggered.start(200.0)
    assert staggered.due(staggered_targets[0], 200.0)
    assert not staggered.due(staggered_targets[1], 200.0)
    assert staggered.due(staggered_targets[1], 200.9)
    assert not staggered.due(staggered_targets[4], 203.8)
    assert staggered.due(staggered_targets[4], 203.9)
    staggered.mark_sampled(staggered_targets[1], 200.9)
    assert not staggered.due(staggered_targets[1], 205.8)
    assert staggered.due(staggered_targets[1], 205.9)
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


def assert_network_counter_rates() -> None:
    target = parse_target("BASEAPP_TYPE=@baseapp#7001:root/network")
    previous = {}
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "network-rates.jsonl"
        with JsonlRecorder(path, "test-run", "contract") as recorder:
            record_watcher_samples(
                recorder,
                target,
                {"numPacketsSent": 100, "numPacketsReceived": 80,
                 "numBytesSent": 1000, "numBytesReceived": 800},
                10.0,
                {},
                {},
                previous,
            )
            record_watcher_samples(
                recorder,
                target,
                {"numPacketsSent": 140, "numPacketsReceived": 100,
                 "numBytesSent": 1800, "numBytesReceived": 1200},
                12.0,
                {},
                {},
                previous,
            )
        samples = build_summary(load_events(path))["samples"]
        prefix = "watcher.baseapp#7001.root/network/rates"
        assert samples[f"{prefix}/packetsSentPerSecond"]["max"] == 20
        assert samples[f"{prefix}/packetsReceivedPerSecond"]["max"] == 10
        assert samples[f"{prefix}/bytesSentPerSecond"]["max"] == 400
        assert samples[f"{prefix}/bytesReceivedPerSecond"]["max"] == 200


def assert_watcher_instance_isolation() -> None:
    first = parse_target("BASEAPP_TYPE=@baseapp#7001:root/network/kcp")
    second = parse_target("BASEAPP_TYPE=@baseapp#7002:root/network/kcp")
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "watcher-instances.jsonl"
        with JsonlRecorder(path, "test-run", "contract") as recorder:
            record_watcher_samples(recorder, first, {"overdueChannels": 3}, 1.0, {}, {})
            record_watcher_samples(recorder, second, {"overdueChannels": 9}, 1.0, {}, {})
        samples = build_summary(load_events(path))["samples"]
        assert samples["watcher.baseapp#7001.root/network/kcp/overdueChannels"]["max"] == 3
        assert samples["watcher.baseapp#7002.root/network/kcp/overdueChannels"]["max"] == 9


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
    assert scenario["duration_seconds"] == 150
    assert len(scenario["watcher_targets"]) == 33
    assert scenario["watcher_intervals"]["BOTS_TYPE:root/bots/performance"] == 5.0
    assert scenario["workload_environment"]["KBE_PERF_PYTHON_RTT_SAMPLE_EVERY"] == "50"
    assert scenario["workload_environment"]["KBE_PERF_PYTHON_LATENCY_WINDOW_SECONDS"] == "180"
    assert scenario["readiness"]["root/bots/pythonLatency/control/successes"] == {"min": 1}
    assert readiness_value_matches(1, {"min": 1}, 2000)
    assert not readiness_value_matches(0, {"min": 1}, 2000)
    assert readiness_value_matches(2000, "$bots", 2000)
    assert scenario["watcher_intervals"]["BOTS_TYPE:root/bots/pythonLatency/roundTrip"] == 2.0
    environment = scenario_environment(scenario)
    assert environment["KBE_PERF_PYTHON_CHAIN"] == "1"
    cluster_environment = scenario_cluster_environment(scenario)
    assert cluster_environment["KBE_PERF_PROFILE_LATENCY_WINDOW_SECONDS"] == "180"
    try:
        scenario_environment({"workload_environment": {"PATH": "invalid"}})
    except ValueError:
        pass
    else:
        raise AssertionError("unrestricted workload environment key must fail")
    try:
        scenario_cluster_environment({"cluster_environment": {"PATH": "invalid"}})
    except ValueError:
        pass
    else:
        raise AssertionError("unrestricted cluster environment key must fail")

    gameplay_path = Path(__file__).resolve().parent / "scenarios/gameplay_10000.json"
    gameplay = load_scenario(gameplay_path)
    assert gameplay["readiness"]["root/witness/active"] == "$bots"
    assert gameplay["readiness"]["root/bots/performance/clientsStatePlay"] == "$bots"
    assert gameplay["readiness"]["root/bots/performance/clientsDestroyed"] == 0
    assert gameplay["readiness"]["root/bots/performance/networkErrors"] == 0
    assert "BASEAPPMGR_TYPE=@baseappmgr:root/allocation" in gameplay["watcher_targets"]
    assert gameplay["watcher_intervals"]["BASEAPPMGR_TYPE:root/allocation"] == 5.0
    assert "CELLAPPMGR_TYPE=@cellappmgr:root/allocation" in gameplay["watcher_targets"]
    assert gameplay["watcher_intervals"]["CELLAPPMGR_TYPE:root/allocation"] == 5.0
    assert "BASEAPP_TYPE=@baseapp:root/network/clientVolatileBackpressure" in gameplay["watcher_targets"]
    assert gameplay["watcher_intervals"]["BASEAPP_TYPE:root/network/clientVolatileBackpressure"] == 5.0
    assert "BASEAPP_TYPE=@baseapp:root/network/clientInput" in gameplay["watcher_targets"]
    assert gameplay["watcher_intervals"]["BASEAPP_TYPE:root/network/clientInput"] == 5.0
    assert "BASEAPP_TYPE=@baseapp:root/network/messageProcessing" in gameplay["watcher_targets"]
    assert gameplay["watcher_intervals"]["BASEAPP_TYPE:root/network/messageProcessing"] == 5.0
    assert "BASEAPP_TYPE=@baseapp:root/network/kcp/receive" in gameplay["watcher_targets"]
    assert gameplay["watcher_intervals"]["BASEAPP_TYPE:root/network/kcp/receive"] == 5.0
    assert "BASEAPP_TYPE=@baseapp:root/scriptStages" in gameplay["watcher_targets"]
    assert gameplay["watcher_intervals"]["BASEAPP_TYPE:root/scriptStages"] == 5.0
    assert "CELLAPP_TYPE=@cellapp:root/network/messageProcessing" in gameplay["watcher_targets"]
    assert gameplay["watcher_intervals"]["CELLAPP_TYPE:root/network/messageProcessing"] == 5.0
    assert "CELLAPP_TYPE=@cellapp:root/network/kcp/receive" in gameplay["watcher_targets"]
    assert gameplay["watcher_intervals"]["CELLAPP_TYPE:root/network/kcp/receive"] == 5.0
    assert "CELLAPP_TYPE=@cellapp:root/scriptStages" in gameplay["watcher_targets"]
    assert gameplay["watcher_intervals"]["CELLAPP_TYPE:root/scriptStages"] == 5.0
    assert "CELLAPP_TYPE=@cellapp:root/coordinateSystem" in gameplay["watcher_targets"]
    assert gameplay["watcher_intervals"]["CELLAPP_TYPE:root/coordinateSystem"] == 5.0
    assert "CELLAPP_TYPE=@cellapp:root/witness/backpressure" in gameplay["watcher_targets"]
    assert gameplay["watcher_intervals"]["CELLAPP_TYPE:root/witness/backpressure"] == 5.0
    assert "CELLAPP_TYPE=@cellapp:root/witness/scheduler" in gameplay["watcher_targets"]
    assert "CELLAPP_TYPE=@cellapp:root/witness/messages" in gameplay["watcher_targets"]
    assert "CELLAPP_TYPE=@cellapp:root/witness/queues" in gameplay["watcher_targets"]
    assert gameplay["watcher_intervals"]["CELLAPP_TYPE:root/witness/queues"] == 5.0
    for component in ("BASEAPP_TYPE", "CELLAPP_TYPE"):
        component_name = component.split("_", 1)[0].lower()
        assert f"{component}=@{component_name}:root/timers" in gameplay["watcher_targets"]
        assert gameplay["watcher_intervals"][f"{component}:root/timers"] == 5.0
        assert f"{component}=@{component_name}:root/network/receiveWindow" in gameplay["watcher_targets"]
        assert gameplay["watcher_intervals"][f"{component}:root/network/receiveWindow"] == 5.0

    defaults_path = Path(__file__).resolve().parents[3] / "res/server/kbengine_defaults.xml"
    defaults_source = defaults_path.read_text(encoding="utf-8")
    assert "<witness_total_bytes_per_tick> 2048 </witness_total_bytes_per_tick>" in defaults_source
    assert "<witness_global_bytes_per_tick> 1048576 </witness_global_bytes_per_tick>" in defaults_source
    assert "<witness_global_updates_per_tick> 1024 </witness_global_updates_per_tick>" in defaults_source
    assert "<highSegments> 128 </highSegments>" in defaults_source
    assert "<lowSegments> 32 </lowSegments>" in defaults_source
    assert "<spaceAllocationMaxSkew> 2 </spaceAllocationMaxSkew>" in defaults_source
    baseapp_source = (Path(__file__).resolve().parents[2] / "server/baseapp/baseapp.cpp").read_text(encoding="utf-8")
    cellapp_source = (Path(__file__).resolve().parents[2] / "server/cellapp/cellapp.cpp").read_text(encoding="utf-8")
    witness_source = (Path(__file__).resolve().parents[2] / "server/cellapp/witness.cpp").read_text(encoding="utf-8")
    profile_source = (Path(__file__).resolve().parents[2] / "lib/helper/profile.cpp").read_text(encoding="utf-8")
    profile_inline = (Path(__file__).resolve().parents[2] / "lib/helper/profile.inl").read_text(encoding="utf-8")
    entity_app_source = (Path(__file__).resolve().parents[2] / "lib/server/entity_app.h").read_text(encoding="utf-8")
    server_app_source = (Path(__file__).resolve().parents[2] / "lib/server/serverapp.cpp").read_text(encoding="utf-8")
    assert 'WATCH_OBJECT("timers/system/skippedIntervals"' in server_app_source
    assert 'WATCH_OBJECT("timers/script/budgetExhaustions"' in server_app_source
    assert 'WATCH_OBJECT("network/receiveWindow/condemnedChannels"' in server_app_source
    assert 'WATCH_OBJECT("network/clientVolatileBackpressure/activeClients"' in baseapp_source
    assert 'WATCH_OBJECT("network/clientInput/staleNoCellDrops"' in baseapp_source
    assert "++staleClientInputNoCellDrops_;" in baseapp_source
    assert 'WATCH_OBJECT("witness/backpressure/activeSuppressed"' in cellapp_source
    assert 'WATCH_OBJECT("witness/scheduler/deferred"' in cellapp_source
    assert 'WATCH_OBJECT("witness/messages/enterBytes"' in cellapp_source
    assert 'WATCH_OBJECT("witness/messages/enterProcessingMaxNanos"' in cellapp_source
    assert 'WATCH_OBJECT("witness/messages/leaveProcessingMaxNanos"' in cellapp_source
    assert 'WATCH_OBJECT("coordinateSystem/equalCorrectionMoves"' in cellapp_source
    assert 'WATCH_OBJECT("coordinateSystem/equalCorrectionCallbacksSuppressed"' in cellapp_source
    assert 'prefix + "SampledMaxNanos"' in server_app_source
    assert 'categoryName(category) + "/"' not in server_app_source
    assert 'WATCH_OBJECT("witness/queues/cancelledPendingLeaves"' in cellapp_source
    assert 'WATCH_OBJECT("scriptStages/rpcCalls"' in baseapp_source
    assert 'WATCH_OBJECT("scriptStages/rpcCalls"' in cellapp_source
    assert '&pSlow->' not in baseapp_source
    assert '&pSlow->' not in cellapp_source
    script_metrics = (Path(__file__).resolve().parents[2] / "lib/server/script_stage_metrics.h").read_text(encoding="utf-8")
    assert "SLOW_TOP_CAPACITY = 8" in script_metrics
    assert "RPC_SAMPLE_RATE = 8" in script_metrics
    assert "durationNanos >= 1000000" in script_metrics
    assert 'WATCH_OBJECT("witness/backpressure/staleControlDrops"' in cellapp_source
    assert "++staleWitnessVolatileControlDrops_;" in cellapp_source
    assert 'WATCH_OBJECT("network/clientInput/staleEntityDrops"' in cellapp_source
    assert 'WATCH_OBJECT("network/clientInput/staleControlledEntityDrops"' in cellapp_source
    assert "++staleClientInputEntityDrops_;" in cellapp_source
    assert "++staleControlledClientInputEntityDrops_;" in cellapp_source
    assert "onUpdateDataFromClient: not found entity" not in cellapp_source
    assert "onUpdateDataFromClientForControlledEntity: not found entity" not in cellapp_source
    for metric in ("slowCalls", "warningLogs", "suppressedWarnings", "slowMaxMicros"):
        assert f"latency/{metric}" in profile_source
    assert "suppressedSinceLast" in profile_inline
    assert "warningLimiter_.record" in profile_inline
    for metric in ("tick/slowPeriods", "tick/warningLogs", "tick/suppressedWarnings", "tick/slowMaxMicros"):
        assert metric in entity_app_source
    assert "tickWarningLimiter_.record" in entity_app_source
    gameplay_scenario = json.loads(
        (Path(__file__).resolve().parent / "scenarios/gameplay_10000.json").read_text(encoding="utf-8")
    )
    for target in (
        "BASEAPP_TYPE=@baseapp:root/tick",
        "CELLAPP_TYPE=@cellapp:root/tick",
        "CELLAPP_TYPE=@cellapp:root/network/clientInput",
    ):
        assert target in gameplay_scenario["watcher_targets"]
    semantic_message_handler = witness_source.split(
        "const Network::MessageHandler& Witness::getViewEntityMessageHandler", 1
    )[1].split("bool Witness::entityID2AliasID", 1)[0]
    assert "ialiasID = -1;" in semantic_message_handler
    assert "return normalMsgHandler;" in semantic_message_handler
    assert "return optimizedMsgHandler;" not in semantic_message_handler

    pending_leave_reentry = witness_source.split(
        "void Witness::onEnterView", 1
    )[1].split("void Witness::onLeaveView", 1)[0]
    assert "recordCancelledPendingLeave" in pending_leave_reentry
    assert "wasVisibleToClient" in pending_leave_reentry
    assert "sendCall(pSendBundle)" not in pending_leave_reentry

    pending_leave_send = witness_source.split(
        "bool Witness::processEntityRefUpdate", 1
    )[1].split("void Witness::removeViewEntityRef", 1)[0]
    assert "ClientInterface::onEntityLeaveWorld, leaveWorld" in pending_leave_send
    assert "ClientInterface::onEntityLeaveWorldOptimized" not in pending_leave_send

    client_base_source = (Path(__file__).resolve().parents[2] / "lib/client_lib/clientobjectbase.cpp").read_text(encoding="utf-8")
    idempotent_leave = client_base_source.split(
        "void ClientObjectBase::onEntityLeaveWorld(Network::Channel * pChannel, ENTITY_ID eid)", 1
    )[1].split("void ClientObjectBase::onEntityDestroyed", 1)[0]
    assert "++staleViewMessageDrops_;" in idempotent_leave
    assert "ClientObjectBase::onEntityLeaveWorld: not found entity" not in idempotent_leave
    stale_force_position = client_base_source.split(
        "void ClientObjectBase::onSetEntityPosAndDir", 1
    )[1].split("void ClientObjectBase::onUpdateData", 1)[0]
    assert "++staleViewMessageDrops_;" in stale_force_position
    assert "ClientObjectBase::onSetEntityPosAndDir: not found entity" not in stale_force_position

    entity_garbages_source = (
        Path(__file__).resolve().parents[2] / "lib/entitydef/entity_garbages.h"
    ).read_text(encoding="utf-8")
    duplicate_garbage_add = entity_garbages_source.split(
        "void EntityGarbages<T>::add", 1
    )[1].split("if(_entities.size() == 0)", 1)[0]
    assert "already tracks an older generation" in duplicate_garbage_add
    assert "ERROR_MSG" not in duplicate_garbage_add
    client_entity_source = (Path(__file__).resolve().parents[2] / "lib/client_lib/entity.cpp").read_text(encoding="utf-8")
    script_callbacks_source = (Path(__file__).resolve().parents[2] / "lib/client_lib/script_callbacks.cpp").read_text(encoding="utf-8")
    bots_source = (Path(__file__).resolve().parents[2] / "server/tools/bots/bots.cpp").read_text(encoding="utf-8")
    assert "pEntity->pClientApp(NULL);" in client_base_source
    assert "pGarbages->clear();" in client_base_source

    # Client callbacks can hold an Entity's final Python reference. Both finalise() and reset()
    # must cancel those callbacks before owner detachment, and Entity detachment must stop movement.
    # 客户端回调可能持有 Entity 的最后一个 Python 引用；finalise/reset 必须先取消回调，解绑时也必须停止移动。
    finalise_body = client_base_source.split("void ClientObjectBase::finalise(void)", 1)[1].split(
        "void ClientObjectBase::reset(void)", 1
    )[0]
    reset_body = client_base_source.split("void ClientObjectBase::reset(void)", 1)[1].split(
        "void ClientObjectBase::releaseOwnedEntities()", 1
    )[0]
    for lifecycle_body in (finalise_body, reset_body):
        assert lifecycle_body.index("scriptCallbacks_.cancelAll();") < lifecycle_body.index("releaseOwnedEntities();")
        assert lifecycle_body.index("pyCallbackMgr_.finalise();") < lifecycle_body.index("releaseOwnedEntities();")
    owner_setter = client_entity_source.split("void Entity::pClientApp(ClientObjectBase* p)", 1)[1].split(
        "PyObject* Entity::pyGetBaseEntityCall()", 1
    )[0]
    assert owner_setter.index("stopMove();") < owner_setter.index("pClientApp_ = p;")
    stop_move_body = client_entity_source.split("bool Entity::stopMove()", 1)[1].split(
        "uint32 Entity::moveToPoint", 1
    )[0]
    assert stop_move_body.index("pMoveHandlerID_ = 0;") < stop_move_body.index("delCallback(moveHandlerID)")
    assert "if (pClientApp_ != NULL)" in stop_move_body
    cancel_all_body = script_callbacks_source.split("void ScriptCallbacks::cancelAll()", 1)[1].split(
        "ScriptID ScriptCallbacks::getIDForHandle", 1
    )[0]
    assert "while (!map_.empty())" in cancel_all_body
    assert "map_.size() < sizeBeforeCancel" in cancel_all_body
    assert "Py_XDECREF(pPrevious);" in client_entity_source
    assert "++staleViewMessageDrops_;" in client_base_source
    assert "iter->second->append(s.data() + s.rpos(), s.length());" in client_base_source
    assert 'ClientObjectBase::onRemoteMethodCall: not found entity' not in client_base_source
    assert 'ClientObjectBase::onUpdatePropertys: not found entity' not in client_base_source
    assert 'WATCH_OBJECT("bots/performance/clearedEntityGarbages"' in bots_source


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
    assert scenario["database_connections"] == 32
    assert scenario["reliable_udp_tick_interval_ms"] == 10
    assert scenario["reliable_udp_min_rto_ms"] == 50
    assert scenario["runtime_log_level"] == "warn"
    assert scenario["server_runtime_log_level"] == "warn"
    assert scenario["workload_processes"] == 40
    assert scenario["watcher_timeout_seconds"] == 10
    assert scenario["watcher_concurrency"] == 8
    assert scenario["workload_cid_start"] == 10000
    assert "fixture" not in scenario
    assert "server_readiness" not in scenario
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
    sustainable = load_scenario(scenario_path.with_name("gameplay_10000_kcp50.json"))
    assert sustainable["name"] == "gameplay-10000-kcp50"
    assert sustainable["reliable_udp_tick_interval_ms"] == 50
    assert sustainable["reliable_udp_min_rto_ms"] == 50
    default_cadence = load_scenario(scenario_path.with_name("gameplay_10000_kcp100.json"))
    assert default_cadence["name"] == "gameplay-10000-kcp100"
    assert default_cadence["reliable_udp_tick_interval_ms"] == 100
    assert default_cadence["reliable_udp_min_rto_ms"] == 50

    cpu_sustainable = load_scenario(scenario_path.with_name("gameplay_10000_kcp250.json"))
    assert cpu_sustainable["name"] == "gameplay-10000-kcp250"
    assert cpu_sustainable["external_timeout_seconds"] == 120
    assert cpu_sustainable["reliable_udp_tick_interval_ms"] == 250
    assert cpu_sustainable["reliable_udp_min_rto_ms"] == 50


def assert_parameterized_topology_and_manager_readiness() -> None:
    with tempfile.TemporaryDirectory() as directory:
        binary_dir = Path(directory)
        for name in ("machine", "logger", "interfaces", "dbmgr", "baseappmgr",
                     "cellappmgr", "baseapp", "cellapp", "loginapp"):
            (binary_dir / f"{name}.exe").touch()

        components, watcher_ids = build_local_cluster_components(binary_dir, 3, 6)
        declarations = components.split("|")
        assert len(declarations) == 16
        assert [int(item.split("::")[2]) for item in declarations] == [
            1000, 2000, 3000, 4000, 5000, 6000,
            7001, 7002, 7003,
            8001, 8002, 8003, 8004, 8005, 8006,
            9000,
        ]
        assert [int(item.split("::")[3]) for item in declarations] == list(range(1, 17))
        assert watcher_ids == {
            "baseapp": [7001, 7002, 7003],
            "cellapp": [8001, 8002, 8003, 8004, 8005, 8006],
        }

        (binary_dir / "cellapp.exe").unlink()
        try:
            build_local_cluster_components(binary_dir, 3, 6)
        except FileNotFoundError as exc:
            assert "cellapp.exe" in str(exc)
        else:
            raise AssertionError("a missing server executable must fail before startup")

    assert partition_batch_size(100, 4) == 25
    try:
        partition_batch_size(10, 4)
    except ValueError as exc:
        assert "divisible" in str(exc)
    else:
        raise AssertionError("uneven aggregate batches must be rejected")

    ready = {"readyForLogin": True, "readyApps": 3, "totalApps": 3, "minProgress": 1.0}
    assert manager_readiness_satisfied(ready, 3)
    assert manager_readiness_satisfied({**ready, "readyForLogin": "true"}, 3)
    assert not manager_readiness_satisfied({**ready, "readyApps": 2}, 3)
    assert not manager_readiness_satisfied({**ready, "totalApps": 2}, 3)
    assert not manager_readiness_satisfied({**ready, "minProgress": 0.99}, 3)


def assert_bots_dev_and_manager_watcher_source_contract() -> None:
    source_root = _repository_root() / "kbe/src"
    bots_main = (source_root / "server/tools/bots/main.cpp").read_text(encoding="utf-8")
    bots_source = (source_root / "server/tools/bots/bots.cpp").read_text(encoding="utf-8")
    components_source = (source_root / "lib/server/components.cpp").read_text(encoding="utf-8")
    assert 'std::string(argv[index]) == "--dev"' in bots_main
    assert 'std::string(argv[index]) == "--reuse-existing-accounts"' in bots_main
    assert 'WATCH_OBJECT("bots/devMode", g_botsDevMode)' in bots_source
    assert 'WATCH_OBJECT("bots/reuseAccounts", g_botsReuseAccounts)' in bots_source
    network_index = bots_source.index("DebugHelper::getSingleton().pNetworkInterface(&networkInterface())")
    logger_index = bots_source.index("Components::getSingleton().findLogger(true)")
    assert network_index < logger_index
    assert "g_componentType == BOTS_TYPE && !allowBots" in components_source

    for relative_path, manager_name in (
        ("server/baseappmgr/baseappmgr.cpp", "Baseappmgr"),
        ("server/cellappmgr/cellappmgr.cpp", "Cellappmgr"),
    ):
        manager_source = (source_root / relative_path).read_text(encoding="utf-8")
        for watcher_path in ("readyForLogin", "readyApps", "totalApps", "minProgress"):
            assert f'WATCH_OBJECT("readiness/{watcher_path}"' in manager_source
        assert f"bool {manager_name}::initializeWatcher()" in manager_source


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
        def query(target, _timeout_seconds=None):
            assert target.component_type == "BOTS_TYPE"
            assert _timeout_seconds is not None and 0 < _timeout_seconds <= 0.1
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

    class FailureCollector:
        @staticmethod
        def query(target, _timeout_seconds=None):
            return {"value": target.port}

    failure_targets = [
        parse_target("BASEAPP_TYPE=127.0.0.1:12001:root"),
        parse_target("BASEAPP_TYPE=127.0.0.1:12001:root/network/kcp"),
        parse_target("BASEAPP_TYPE=127.0.0.1:12001:root/network/poller"),
    ]
    snapshot = collect_readiness_failure_snapshot(
        FailureCollector(), failure_targets, {"root/witness/active": 9999}
    )
    assert snapshot["root/witness/active"] == 9999
    assert "BASEAPP_TYPE@127.0.0.1:12001/root/value" not in snapshot
    assert snapshot[
        "BASEAPP_TYPE@127.0.0.1:12001/root/network/kcp/value"
    ] == 12001
    assert snapshot[
        "BASEAPP_TYPE@127.0.0.1:12001/root/network/poller/value"
    ] == 12001


def main() -> int:
    assert_centralized_discovery_matching()
    assert_partial_workload_start_cleanup()
    assert_windows_command_line_preserves_quoted_executable()
    assert_server_binary_directory_resolution()
    assert_watcher_connection_reuse()
    assert_watcher_schedule()
    assert_cprofile_window_metrics()
    assert_network_counter_rates()
    assert_watcher_instance_isolation()
    assert_fixture_callbacks()
    assert_python_latency_scenario()
    assert_multi_component_cluster_manifest()
    assert_gameplay_stress_scenario()
    assert_parameterized_topology_and_manager_readiness()
    assert_bots_dev_and_manager_watcher_source_contract()
    assert_readiness_target_ownership()
    repository_root = _repository_root()
    assert repository_root.is_dir()
    assert (repository_root / "kbe/src/tests/performance/run.py").is_file()
    bots_client_source = (
        repository_root / "kbe/src/server/tools/bots/clientobject.cpp"
    ).read_text(encoding="utf-8")
    assert bots_client_source.count("state_ = C_STATE_LOGIN_BASEAPP_HELLO;") == 2
    assert "kcpHandshakeTimeoutStamps()" in bots_client_source
    assert "Network::g_channelExternalTimeout" in bots_client_source
    assert "onKcpHandshakeInvalidPacket()" in bots_client_source
    assert 'fallbackToBaseappTcp("invalid KCP hello ACK")' not in bots_client_source
    assert "case C_STATE_LOGIN_BASEAPP_HELLO:" in bots_client_source
    assert "onHelloCB_ activates encryption" in bots_client_source
    encryption_source = (
        repository_root / "kbe/src/lib/network/encryption_filter.cpp"
    ).read_text(encoding="utf-8")
    assert "encryptedPayloadLen % BLOCK_SIZE" in encryption_source
    assert "return REASON_CORRUPTED_PACKET;" in encryption_source
    db_task_source = (
        repository_root / "kbe/src/lib/db_interface/db_tasks.cpp"
    ).read_text(encoding="utf-8")
    assert "queue delay {:.2f}s, queryBytes={}, suppressed={}" in db_task_source
    assert "sql:({" not in db_task_source
    script_level_source = (
        repository_root / "kbe/src/lib/helper/script_loglevel.h"
    ).read_text(encoding="utf-8")
    assert "SCRIPT_DBG = Level::DEBUG_INT + 1" in script_level_source
    assert "SCRIPT_WAR = Level::WARN_INT + 1" in script_level_source
    assert "SCRIPT_ERR = Level::ERROR_INT + 1" in script_level_source
    channel_source = (
        repository_root / "kbe/src/lib/network/channel.cpp"
    ).read_text(encoding="utf-8")
    assert "recordDiscardedPacketAfterClose" in channel_source
    scheduler_source = (
        repository_root / "kbe/src/lib/network/kcp_update_scheduler.cpp"
    ).read_text(encoding="utf-8")
    assert "KCP_MIN_UPDATES_PER_WAKEUP = 1" in scheduler_source
    assert "KCP_MIN_ACK_FLUSHES_PER_WAKEUP = 4" in scheduler_source
    assert "KCP_MAX_UPDATES_PER_WAKEUP = 2048" in scheduler_source
    assert "KCP_BACKLOG_RETRY_DELAY_MICROS = 1000" in scheduler_source
    assert "protocolTickMissCount_" in scheduler_source
    assert "g_rudp_tickInterval > 0 ? g_rudp_tickInterval : 100" in scheduler_source
    ikcp_source = (
        repository_root / "kbe/src/lib/network/ikcp.c"
    ).read_text(encoding="utf-8")
    assert "ikcp_advance_flush_cursor_on_remove" in ikcp_source
    assert "kcp->flush_cursor = p" in ikcp_source
    serverapp_source = (
        repository_root / "kbe/src/lib/server/serverapp.cpp"
    ).read_text(encoding="utf-8")
    assert 'WATCH_OBJECT("network/kcp/configuredExternalFlushSegmentsBudget"' in serverapp_source
    for watcher in (
        "pendingPayloadBytes",
        "queuedPayloadBytes",
        "unackedPayloadBytes",
        "sendBufferMemoryBytes",
        "averageQueuedPayloadBytes",
        "streamCoalesces",
        "streamCoalescedBytes",
        "ackScheduleRequests",
        "ackTotalProcessingMicros",
        "ackMaxProcessingMicros",
        "dataTotalProcessingMicros",
        "dataMaxProcessingMicros",
        "flushCalls",
        "flushScannedSegments",
        "flushDataSegments",
        "flushEmptyDataCalls",
        "ackOutputCalls",
        "ackOutputBytes",
        "dataOutputCalls",
        "dataOutputBytes",
        "sendtoSampleCalls",
        "sendtoSampleTotalMicros",
        "sendtoSampleMaxMicros",
    ):
        assert f'WATCH_OBJECT("network/kcp/{watcher}"' in serverapp_source
    assert 'WATCH_OBJECT("network/kcp/configuredExternalWriteQueueMaxBytes"' in serverapp_source
    baseapp_source = (
        repository_root / "kbe/src/server/baseapp/baseapp.cpp"
    ).read_text(encoding="utf-8")
    for watcher in (
        "maxPendingBytes",
        "configuredHighBytes",
        "configuredLowBytes",
        "configuredWriteQueueMaxBytes",
    ):
        assert f'WATCH_OBJECT("network/clientVolatileBackpressure/{watcher}"' in baseapp_source
    profile_source = (
        repository_root / "kbe/src/lib/helper/profile.cpp"
    ).read_text(encoding="utf-8")
    assert "KBE_PERF_PROFILE_LATENCY_WINDOW_SECONDS" in profile_source
    bots_source = (
        repository_root / "kbe/src/server/tools/bots/bots.cpp"
    ).read_text(encoding="utf-8")
    assert "KBE_PERF_PYTHON_LATENCY_WINDOW_SECONDS" in bots_source
    assert "seconds * 1000000000.0" in bots_source
    assert 'WATCH_OBJECT("bots/performance/numPacketsSent"' in bots_source
    baseappmgr_source = (
        repository_root / "kbe/src/server/baseappmgr/baseappmgr.cpp"
    ).read_text(encoding="utf-8")
    assert "baseappPlacementScore(" in baseappmgr_source
    assert "reservePendingLogin(timestamp())" in baseappmgr_source
    assert 'WATCH_OBJECT("allocation/pendingLogins"' in baseappmgr_source
    baseapp_state_source = (
        repository_root / "kbe/src/server/baseappmgr/baseapp.h"
    ).read_text(encoding="utf-8")
    assert "updateConfirmedClients" in baseapp_state_source
    assert "expirePendingLogins" in baseapp_state_source
    assert "updateBaseappArgs6" in (
        repository_root / "kbe/src/server/baseapp/baseapp.cpp"
    ).read_text(encoding="utf-8")
    bots_source = (
        repository_root / "kbe/src/server/tools/bots/bots.cpp"
    ).read_text(encoding="utf-8")
    assert 'WATCH_OBJECT("bots/performance/pendingPollerRearms"' in bots_source
    assert 'WATCH_OBJECT("bots/performance/pollerRearmAttempts"' in bots_source
    assert 'WATCH_OBJECT("bots/performance/pollerRearmRetries"' in bots_source
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
    if os.name == "nt":
        # The formal runner must not depend on spawning PowerShell while the host
        # is CPU saturated. Exercise the direct Win32 process path in the contract.
        # 正式压测在 CPU 饱和时不能依赖创建 PowerShell；契约直接覆盖 Win32 采样路径。
        native_samples = ProcessGroupCollector({"self": os.getpid()}).sample()
        assert native_samples["self"].working_set_bytes > 0
        assert native_samples["self"].private_bytes > 0
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
        manifest_root = root / "cluster/server"
        manifest_logs = manifest_root / "logs"
        manifest_logs.mkdir(parents=True)
        (manifest_root / "components.json").write_text(
            '[{"name":"baseappmgr","component_id":5000,'
            '"intaddr":"127.0.0.1","intport":15000}]\n',
            encoding="utf-8",
        )
        resolved_manifest = resolve_target(
            parse_target("BASEAPPMGR_TYPE=@baseappmgr:root/readiness"),
            [manifest_logs],
        )
        assert resolved_manifest.host == "127.0.0.1" and resolved_manifest.port == 15000
        assets = root / "assets/res/server"
        assets.mkdir(parents=True)
        (assets / "kbengine.xml").write_text(
            "<root><bots /><dbmgr><databaseInterfaces><default><numConnections>5</numConnections>"
            "</default></databaseInterfaces></dbmgr></root>\n",
            encoding="utf-8",
        )
        overlay = create_config_overlay(
            root / "assets",
            root / "run",
            500,
            external_receive_messages=1024,
            external_receive_bytes=1048576,
            external_timeout_seconds=120,
            reliable_udp_tick_interval_ms=20,
            reliable_udp_min_rto_ms=50,
            runtime_log_level="warn",
            server_runtime_log_level="info",
            database_connections=32,
            baseapp_external_port_count=10,
        )
        xml_root = ET.parse(overlay).getroot()
        assert xml_root.findtext("./bots/defaultAddBots/totalCount") == "500"
        assert xml_root.findtext("./channelCommon/windowOverflow/receive/messages/external") == "1024"
        assert xml_root.findtext("./channelCommon/windowOverflow/receive/messages/critical") == "1024"
        assert xml_root.findtext("./channelCommon/windowOverflow/receive/bytes/external") == "1048576"
        assert xml_root.findtext("./channelCommon/timeout/external") == "120"
        assert xml_root.findtext("./channelCommon/reliableUDP/tickInterval") == "20"
        assert xml_root.findtext("./channelCommon/reliableUDP/minRTO") == "50"
        assert xml_root.find("./networkInterface/reliableUDP") is None
        assert xml_root.findtext("./dbmgr/databaseInterfaces/default/numConnections") == "32"
        assert xml_root.findtext("./baseapp/externalPorts_min") == "20015"
        assert xml_root.findtext("./baseapp/externalPorts_max") == "20024"
        deterministic_run = root / "deterministic-run"
        create_config_overlay(
            assets,
            deterministic_run,
            10,
            bots_account_suffix_start=1,
        )
        deterministic_root = ET.parse(
            deterministic_run / "config-overlay/res/server/kbengine.xml"
        ).getroot()
        assert deterministic_root.findtext("./bots/account_infos/account_name_suffix_inc") == "1"
        bots_log_config = (
            root / "run/config-overlay/res/server/log4cxx_properties/bots.properties"
        ).read_text(encoding="utf-8")
        legacy_bots_log_config = (root / "run/config-overlay/res/log4j.properties").read_text(encoding="utf-8")
        baseapp_log_config = (
            root / "run/config-overlay/res/server/log4cxx_properties/baseapp.properties"
        ).read_text(encoding="utf-8")
        assert "log4j.rootLogger=warn, R" in bots_log_config
        assert "logs/bots.${KBE_COMPONENTID}.log" in bots_log_config
        assert bots_log_config == legacy_bots_log_config
        assert "log4j.rootLogger=info, R" in baseapp_log_config
        log_path = root / "streamed.log"
        log_path.write_text(
            "ordinary line\nWARNING split across chunks\n"
            "WARN root - ClientObject::onNetworkError: error=channel inactivity timeout\n"
            "ERROR final\n",
            encoding="utf-8",
        )
        log_collector = IncrementalLogCollector(root)
        log_collector.READ_CHUNK_CHARS = 7
        log_counts = log_collector.sample()
        assert log_counts["warning"] == 2 and log_counts["error"] == 1
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
            recorder.record("logs", "all", "log.warning.count", 5, "count", {"kind": "counter", "phase": "startup"})
            recorder.record("logs", "all", "log.error.count", 2, "count", {"kind": "counter", "phase": "readiness"})
            recorder.record("logs", "all", "log.warning.count", 4, "count", {"kind": "counter", "phase": "warmup"})
            recorder.record("logs", "all", "log.error.count", 3, "count", {"kind": "counter", "phase": "shutdown"})
            recorder.record("logs", "all", "log.error.count", 1, "count", {"kind": "counter", "phase": "measurement"})
            recorder.record("runner", "workload", "process.exit.count", 1, "count", {"kind": "counter"})
        phase_summary = build_summary(load_events(phase_path), {"max_log_errors": 0})
        assert phase_summary["counters"]["log.error.count"] == 6
        assert phase_summary["phase_counters"]["startup"]["log.warning.count"] == 5
        assert phase_summary["phase_counters"]["readiness"]["log.error.count"] == 2
        assert phase_summary["phase_counters"]["warmup"]["log.warning.count"] == 4
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
            record_watcher_samples(
                recorder,
                parse_target("CELLAPP_TYPE=127.0.0.1:1:root/scriptStages"),
                {"slow0Name": "Avatar.move", "slow0Stage": "pythonCall", "slow0DurationNanos": 2000000},
                1.0,
                {},
                {},
            )
        sampled = build_summary(load_events(recorder_path), {"tick_p99_max_ms": 2.0})
        assert sampled["samples"]["watcher.BOTS_TYPE.bots/tick/lastMicros"]["count"] == 2
        assert sampled["samples"]["watcher.BOTS_TYPE.bots/tick/lastMicros"]["mean"] == 2000
        assert sampled["samples"]["watcher.BOTS_TYPE.bots/tick/lastMicros"]["total"] == 4000
        assert sampled["samples"]["watcher.BOTS_TYPE.bots/tick/lastMicros"]["p99"] > 2000
        assert sampled["quality"]["status"] == "SLOW"
        assert all("tickMaxMicros" not in item for item in sampled["quality"]["slow"])
        assert sampled["labels"]["watcher.CELLAPP_TYPE.root/scriptStages/slow0Name"] == "Avatar.move"
        assert sampled["labels"]["watcher.CELLAPP_TYPE.root/scriptStages/slow0Stage"] == "pythonCall"

        readiness_path = root / "multi-process-readiness.jsonl"
        with JsonlRecorder(readiness_path, "test-run", "contract") as recorder:
            for cid in range(10000, 10004):
                recorder.record_sample(
                    "readiness", "workload", f"bots#{cid}/root/bots/performance/clientsTotal", 2500, "count"
                )
            recorder.record_sample(
                "readiness", "workload", "root/bots/performance/clientsTotal", 10000, "count"
            )
            for _ in range(4):
                recorder.record_sample(
                    "watcher", "BOTS_TYPE", "root/bots/performance/clientsTotal", 2500, "count"
                )
        multi_process = build_summary(
            load_events(readiness_path),
            readiness={"root/bots/performance/clientsTotal": "$bots"},
            configured_bots=10000,
            workload_processes=4,
        )
        assert not any("readiness metric dropped" in item for item in multi_process["quality"]["blockers"])
        with JsonlRecorder(readiness_path, "test-run", "contract") as recorder:
            recorder.record_sample(
                "readiness", "workload", "bots#10000/root/bots/performance/clientsTotal", 2499, "count"
            )
        dropped_process = build_summary(
            load_events(readiness_path),
            readiness={"root/bots/performance/clientsTotal": "$bots"},
            configured_bots=10000,
            workload_processes=4,
        )
        assert any("min=2499.0, expected=2500" in item for item in dropped_process["quality"]["blockers"])
        with JsonlRecorder(readiness_path, "test-run", "contract") as recorder:
            recorder.record_sample(
                "readiness", "workload", "root/bots/performance/clientsTotal", 9999, "count"
            )
        dropped_aggregate = build_summary(
            load_events(readiness_path),
            readiness={"root/bots/performance/clientsTotal": "$bots"},
            configured_bots=10000,
            workload_processes=4,
        )
        assert any("min=9999.0, expected=10000" in item for item in dropped_aggregate["quality"]["blockers"])

        distributed_path = root / "distributed-readiness.jsonl"
        with JsonlRecorder(distributed_path, "test-run", "contract") as recorder:
            recorder.record_sample("watcher", "CELLAPP_TYPE", "root/witness/active", 2000, "count")
            recorder.record_sample("watcher", "CELLAPP_TYPE", "root/witness/active", 0, "count")
            recorder.record_request_latency("watcher", "CELLAPP_TYPE", "witness", 0, 1, True)
        distributed = build_summary(
            load_events(distributed_path),
            readiness={"root/witness/active": "$bots"},
            configured_bots=2000,
            workload_processes=2,
        )
        assert distributed["quality"]["status"] == "PASS"
    windows_cpu_profiler = (
        repository_root / "kbe/src/tests/performance/windows_cpu_profile.ps1"
    ).read_text(encoding="utf-8")
    for contract in (
        "Team Tools\\DiagnosticsHub\\Collector",
        "VSDiagnostics.exe",
        "CpuUsageLow.json",
        "cpu-summary.json",
        "target_usage_percent",
        "target_modules",
        "DownloadMicrosoftSymbols",
        "Sort-Object { [double]$_['usage_percent'] } -Descending",
    ):
        assert contract in windows_cpu_profiler
    assert "Stop-Service" not in windows_cpu_profiler
    assert "Stop-Process" not in windows_cpu_profiler

    print("PERFORMANCE_METRICS_CONTRACT_TEST_PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
