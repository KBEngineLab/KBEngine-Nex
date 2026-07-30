"""Minimal login proxy for the network baseline.
网络基线使用的最小登录 Proxy。
"""

import KBEngine


class Account(KBEngine.Proxy):
    def __init__(self):
        KBEngine.Proxy.__init__(self)

    def onClientEnabled(self):
        avatar = KBEngine.createEntityLocally("Avatar", {})
        avatar.accountEntity = self
        self.giveClientTo(avatar)

    def onLogOnAttempt(self, ip, port, password):
        return KBEngine.LOG_ON_ACCEPT

    def onClientDeath(self):
        self.destroy()
