import os
import time

import KBEngine


_python_probe_entity_sequence = 0
_python_probe_enabled = (
    os.environ.get("KBE_PERF_PYTHON_CHAIN") == "1"
    or os.environ.get("KBE_PERF_PYTHON_RTT") == "1"
)


class Avatar(KBEngine.Entity):
    def __init__(self):
        global _python_probe_entity_sequence
        KBEngine.Entity.__init__(self)
        self._python_probe_started = False
        self._python_probe_selected = False
        if _python_probe_enabled:
            _python_probe_entity_sequence += 1
            self._python_probe_selected = (
                (_python_probe_entity_sequence - 1) % self._python_probe_sample_every() == 0
            )
        self._python_probe_sequence = 0
        self._python_probe_pending = None

    def onEnterWorld(self):
        self._start_python_probe()

    def onEnterSpace(self):
        self._start_python_probe()

    def _start_python_probe(self):
        if _python_probe_enabled:
            if self._python_probe_started:
                return
            self._python_probe_started = True
            if not self._python_probe_selected:
                return
            self._python_performance_probe()

    def _python_performance_probe(self):
        now_ns = time.perf_counter_ns()
        if self._python_probe_pending is not None:
            request_id, started_ns = self._python_probe_pending
            if now_ns - started_ns >= self._python_probe_timeout_ns():
                KBEngine.recordPerformanceProbeTimeout(request_id)
                self._python_probe_pending = None
        if self._python_probe_pending is None and self.base is not None:
            self._python_probe_sequence = (self._python_probe_sequence + 1) & 0xFFFFFFFF
            request_id = ((self.id & 0xFFFFFFFF) << 32) | self._python_probe_sequence
            self._python_probe_pending = (request_id, now_ns)
            self.base.pythonPerformanceProbe(request_id, now_ns)
        if _python_probe_enabled:
            KBEngine.callback(self._python_probe_interval(), self._python_performance_probe)

    @staticmethod
    def _python_probe_interval():
        try:
            interval = float(os.environ.get("KBE_PERF_PYTHON_RTT_INTERVAL", "1.0"))
        except (TypeError, ValueError):
            interval = 1.0
        return max(0.1, min(60.0, interval))

    @staticmethod
    def _python_probe_sample_every():
        try:
            every = int(os.environ.get("KBE_PERF_PYTHON_RTT_SAMPLE_EVERY", "1"))
        except (TypeError, ValueError):
            every = 1
        return max(1, min(10000, every))

    @staticmethod
    def _python_probe_timeout_ns():
        try:
            timeout = float(os.environ.get("KBE_PERF_PYTHON_RTT_TIMEOUT", "5.0"))
        except (TypeError, ValueError):
            timeout = 5.0
        return int(max(1.0, min(60.0, timeout)) * 1_000_000_000)

    def pythonPerformanceProbeResponse(
        self, request_id, client_started_ns, base_received_ns, cell_received_ns, base_returned_ns
    ):
        client_completed_ns = time.perf_counter_ns()
        pending = self._python_probe_pending
        if pending is None or pending[0] != request_id or pending[1] != client_started_ns:
            KBEngine.recordPerformanceProbeInvalidResponse(request_id)
            return
        self._python_probe_pending = None
        KBEngine.recordPerformanceTransaction(
            request_id,
            client_started_ns,
            base_received_ns,
            cell_received_ns,
            base_returned_ns,
            client_completed_ns,
        )
