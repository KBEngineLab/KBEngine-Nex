import time

import KBEngine


class Avatar(KBEngine.Entity):
    def __init__(self):
        KBEngine.Entity.__init__(self)

    def pythonPerformanceCellProbe(self, request_id, client_started_ns, base_received_ns):
        cell_received_ns = time.perf_counter_ns()
        self.base.pythonPerformanceCellResponse(
            request_id, client_started_ns, base_received_ns, cell_received_ns
        )
