"""Contract tests for the Performance Lab. / 性能实验室契约测试。"""

import tempfile
import sys
from pathlib import Path

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from performance.metrics import JsonlRecorder
from performance.report import build_summary, load_events, validate_event


def main() -> int:
    with tempfile.TemporaryDirectory() as directory:
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
        assert summary["requests"]["success"] == 4
        assert summary["requests"]["failures"] == 1
        assert summary["counters"]["request.slow.count"] == 1
    print("PERFORMANCE_METRICS_CONTRACT_TEST_PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
