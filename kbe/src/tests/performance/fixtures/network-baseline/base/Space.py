"""Single empty Space shared by isolated baseline clients.
由隔离基线客户端共享的单一空 Space。
"""

import KBEngine


class Space(KBEngine.Entity):
    def __init__(self):
        KBEngine.Entity.__init__(self)
        self.createCellEntityInNewSpace(None)

    def onGetCell(self):
        # EntityCall 放入 globalData 后，未来扩到多 BaseApp 时仍保持显式的数据流。
        # Publishing an EntityCall keeps the ownership path explicit if the fixture later spans BaseApps.
        KBEngine.globalData["performanceSpace"] = self

    def loginToSpace(self, avatarEntityCall):
        avatarEntityCall.createCell(self.cell)

    def onLoseCell(self):
        if "performanceSpace" in KBEngine.globalData:
            del KBEngine.globalData["performanceSpace"]
