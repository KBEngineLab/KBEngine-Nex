"""The baseline Bots run no asset-level timers or movement.
基线 Bots 不运行资产层定时器或移动逻辑。
"""

import os

import KBEngine


def onInit(isReload):
    pass


def onStart():
    if os.environ.get("KBE_PERF_PYTHON_RTT") == "1":
        KBEngine.callback(1.0, _start_python_probes)


def onFinish():
    pass


def _start_python_probes():
    for entity in list(KBEngine.entities.values()):
        if hasattr(entity, "_start_python_probe"):
            entity._start_python_probe()
    if os.environ.get("KBE_PERF_PYTHON_RTT") == "1":
        KBEngine.callback(1.0, _start_python_probes)
