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
            KBEngine.callback(1.0, self._python_performance_probe)

    def pythonPerformanceProbeResponse(self, started_ns):
        KBEngine.recordPerformanceLatency(started_ns, time.perf_counter_ns())
