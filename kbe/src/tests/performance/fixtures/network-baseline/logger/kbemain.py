"""Keep engine logs available for the controller's post-run scan.
保留引擎日志，供控制器在运行结束后扫描。
"""


def onReadyForShutDown():
    return True


def onLogWrote(logData):
    return True
