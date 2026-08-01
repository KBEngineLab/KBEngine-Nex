"""Keep engine logs available for the controller's post-run scan.
保留引擎日志，供控制器在运行结束后扫描。
"""


def onLoggerAppReady():
    """Keep logger startup deterministic without adding script work.
    保持 Logger 启动确定性，不增加脚本层工作。
    """
    pass


def onReadyForShutDown():
    return True


def onLogWrote(logData):
    return True
