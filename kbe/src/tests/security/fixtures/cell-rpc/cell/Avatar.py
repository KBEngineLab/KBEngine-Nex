import random

import KBEngine
import GlobalDefine
from KBEDebug import DEBUG_MSG, ERROR_MSG, INFO_MSG
from data import d_npcs


class Avatar(KBEngine.Entity):
	def __init__(self):
		KBEngine.Entity.__init__(self)

	def useSkill(self, exposed, targetID, skillID):
		if exposed != self.id:
			return
		target = KBEngine.entities.get(targetID)
		if target is None:
			ERROR_MSG("Avatar::useSkill(%i):targetID=%i not found" % (self.id, targetID))
			return
		target.recvDamage(self.id, skillID, random.randint(5, 20))

	def recvDamage(self, attackerID, skillID, damage):
		if attackerID != self.id:
			self.HP = max(0, self.HP - damage)
			self.checkState()

	def checkState(self):
		if self.HP <= 0:
			self.die()

	def die(self):
		self.HP = 0
		self.MP = 0
		self.state = GlobalDefine.ENTITY_STATE_DEAD

	def relive(self, exposed):
		if exposed == self.id and self.HP <= 0:
			self.setHP(self.HP_Max)
			self.setMP(self.MP_Max)
			self.state = GlobalDefine.ENTITY_STATE_FREE

	def jump(self, exposed):
		# This marker is emitted only after the C++ relationship guard admits
		# the request. Different caller and target IDs prove the accepted target
		# belongs to another authenticated client and is not source-controlled.
		# 此标记只会在 C++ 关系守卫放行后产生；不同的调用方与目标 ID 证明目标属于
		# 另一条认证客户端连接，并非来源主体控制的实体。
		INFO_MSG("SECURITY_CELL_RPC_ACCEPTED caller=%i target=%i spaceID=%i" % (
			exposed, self.id, self.spaceID))
		if exposed == self.id:
			# The controller uses a self-call after the positive cross-client
			# assertion to move this Avatar outside the source Witness while
			# keeping it in the same Space.
			# 控制器在正向跨客户端断言后通过 self-call 将该 Avatar 移出来源
			# Witness，但保留在同一个 Space 内。
			self.position = (-40.0, 2.0, 40.0)
			DEBUG_MSG("Avatar::jump: %i" % self.id)
			self.otherClients.onJump()

	def setHP(self, value):
		self.HP = value

	def setMP(self, value):
		self.MP = value

	def setHPMax(self, value):
		self.HP_Max = value

	def setMPMax(self, value):
		self.MP_Max = value

	def dialog(self, exposed, entityID, eid):
		if exposed != self.id:
			return
		entity = KBEngine.entities.get(entityID)
		if entity is None:
			ERROR_MSG("Avatar::dialog(%i):entityID=%i not found" % (self.id, entityID))
			return
		spaceKey = KBEngine.globalData["spaces"]["space_%i" % self.spaceID]["space_key"]
		npcs = d_npcs.data.get(spaceKey)
		if npcs is None or eid not in npcs:
			ERROR_MSG("Avatar::dialog(%i):space=%s not found" % (self.id, spaceKey))
			return
		self.client.onDialog(entityID, random.choice(npcs[eid]["dialog"]))
