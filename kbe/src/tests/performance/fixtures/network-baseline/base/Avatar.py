"""Proxy that enters the shared empty Space without persistence or game systems.
不经过持久化和业务系统、直接进入空 Space 的 Proxy。
"""

import time

import KBEngine


class Avatar(KBEngine.Proxy):
    def __init__(self):
        KBEngine.Proxy.__init__(self)
        self.accountEntity = None
        # 固定大间距避免 500 个 Avatar 互相进入 AOI；基线只测每条连接自己的 Witness 流量。
        # A fixed wide spacing prevents cross-AOI fan-out so the baseline isolates each connection's Witness traffic.
        self.cellData["position"] = ((self.id % 1000) * 10000.0, 0.0, (self.id // 1000) * 10000.0)
        self.cellData["direction"] = (0.0, 0.0, 0.0)

    def onClientEnabled(self):
        KBEngine.globalData["performanceSpace"].loginToSpace(self)

    def createCell(self, spaceCell):
        self.createCellEntity(spaceCell)

    def onClientDeath(self):
        if self.cell is not None:
            self.destroyCellEntity()
        else:
            self._destroyBase()

    def onLoseCell(self):
        self._destroyBase()

    def _destroyBase(self):
        account = self.accountEntity
        self.accountEntity = None
        if account is not None and not account.isDestroyed:
            account.destroy()
        if not self.isDestroyed:
            self.destroy()

    def pythonPerformanceProbe(self, request_id, client_started_ns):
        base_received_ns = time.perf_counter_ns()
        if self.cell is not None:
            self.cell.pythonPerformanceCellProbe(request_id, client_started_ns, base_received_ns)

    def pythonPerformanceCellResponse(
        self, request_id, client_started_ns, base_received_ns, cell_received_ns
    ):
        base_returned_ns = time.perf_counter_ns()
        if self.client is not None:
            self.client.pythonPerformanceProbeResponse(
                request_id,
                client_started_ns,
                base_received_ns,
                cell_received_ns,
                base_returned_ns,
            )
