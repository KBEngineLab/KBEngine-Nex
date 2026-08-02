import os
import time

import KBEngine


class Avatar(KBEngine.Entity):
    def __init__(self):
        KBEngine.Entity.__init__(self)
        self._python_probe_started = False

    def onEnterWorld(self):
        self._start_python_probe()

    def onEnterSpace(self):
        self._start_python_probe()

    def _start_python_probe(self):
        if os.environ.get("KBE_PERF_PYTHON_RTT") == "1":
            if self._python_probe_started:
                return
            self._python_probe_started = True
            self._python_performance_probe()

    def _python_performance_probe(self):
        if self.base is not None:
            self.base.pythonPerformanceProbe(time.perf_counter_ns())
        if os.environ.get("KBE_PERF_PYTHON_RTT") == "1":
            KBEngine.callback(self._python_probe_interval(), self._python_performance_probe)

    @staticmethod
    def _python_probe_interval():
        try:
            interval = float(os.environ.get("KBE_PERF_PYTHON_RTT_INTERVAL", "1.0"))
        except (TypeError, ValueError):
            interval = 1.0
        return max(0.1, min(60.0, interval))

    def pythonPerformanceProbeResponse(self, started_ns):
        KBEngine.recordPerformanceLatency(started_ns, time.perf_counter_ns())
