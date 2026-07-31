import KBEngine
from KBEDebug import DEBUG_MSG, INFO_MSG


class Avatar(KBEngine.Proxy):
	def __init__(self):
		KBEngine.Proxy.__init__(self)

	def createCell(self, spaceCall):
		self.createCellEntity(spaceCall)

	def onClientEnabled(self):
		if self.cell is not None:
			return

		# Only the explicitly named cross-Space test role enters the second
		# Space. Production assets remain read-only and retain their behavior.
		# 仅显式命名的跨 Space 测试角色进入第二个 Space；生产资产保持只读且行为不变。
		avatarName = self.cellData.get("name", "")
		spaceKey = "security_second" if avatarName.startswith("security_cross_") else "xinshoucun"
		INFO_MSG("SECURITY_CELL_RPC_SPACE_ASSIGN entityID=%i spaceKey=%s" % (self.id, spaceKey))
		KBEngine.globalData["SpaceMgr"].loginToSpace(self, spaceKey)

	def onLogOnAttempt(self, ip, port, password):
		return KBEngine.LOG_ON_ACCEPT

	def onClientDeath(self):
		DEBUG_MSG("Avatar[%i].onClientDeath:" % self.id)
		if self.cell is not None:
			self.destroyCellEntity()

	def onLoseCell(self):
		self.destroy()
