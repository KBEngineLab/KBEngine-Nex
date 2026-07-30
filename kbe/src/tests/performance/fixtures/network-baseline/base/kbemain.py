"""Bootstrap one empty Space for the network baseline.
为网络基线启动一个空 Space。
"""

import KBEngine


def onBaseAppReady(isBootstrap):
    if isBootstrap:
        KBEngine.createEntityLocally("Space", {})


def onReadyForLogin(isBootstrap):
    # 登录必须等到 Space cell 可用，否则首批客户端会把资产启动竞态计入连接结果。
    # Login waits for the Space cell so the first clients cannot observe an asset-startup race.
    return 1.0 if "performanceSpace" in KBEngine.globalData else 0.0


def onReadyForShutDown():
    return True
