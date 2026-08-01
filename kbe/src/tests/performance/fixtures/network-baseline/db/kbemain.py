"""Select the configured default database without asset-specific routing.
不执行业务分库，始终选择已配置的默认数据库。
"""


def onDBMgrReady():
    """The local network baseline needs no database bootstrap work.
    本地网络基线不执行额外数据库启动逻辑。
    """
    pass


def onDBMgrShutDown():
    pass


def onReadyForShutDown():
    return True


def onSelectAccountDBInterface(accountName):
    return "default"
