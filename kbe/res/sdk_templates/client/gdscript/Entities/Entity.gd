class_name Entity
## KBEngine逻辑层的实体基础类
## 所有扩展出的游戏实体都应该继承于该模块

#-----------------------*--------------------------------
var id:int
var className:String
var position:Vector3 = Vector3.ZERO
var direction:Vector3 = Vector3.ZERO
var spaceID:int = 0
var velocity:float
var isOnGround:bool = true
var renderObj
var inWorld:bool = false
var inited:bool = false # __init__调用之后设置为true

# 对于玩家自身来说，它表示是否自己被其它玩家控制了；
# 对于其它entity来说，表示我本机是否控制了这个entity
var isControlled:bool = false
# 当前玩家最后一次同步到服务端的位置与朝向
# 这两个属性是给引擎KBEngine.cs用的，别的地方不要修改
var entityLastLocalPos:Vector3 = Vector3.ZERO
var entityLastLocalDir:Vector3 = Vector3.ZERO

func _init():
	pass

func destroy()-> void:
	detachComponents()
	onDestroy()

func onDestroy()-> void:
	pass

func isPlayer()-> bool:
	return self.id == KBEngine.app.entity_id

func onRemoteMethodCall(_stream:MemoryStream)-> void:
	pass # 动态生成

func onUpdatePropertys(_stream:MemoryStream)-> void:
	pass # 动态生成

func onGetBase()-> void:
	pass # 动态生成

func onGetCell()-> void:
	pass # 动态生成

func onLoseCell()-> void:
	pass # 动态生成

func onComponentsEnterworld()-> void:
	pass # 动态生成， 通知组件onEnterworld

func onComponentsLeaveworld()-> void:
	pass # 动态生成， 通知组件onLeaveworld

func getBaseEntityCall()-> EntityCall:
	return null # 动态生成

func getCellEntityCall()-> EntityCall:
	return null # 动态生成

## KBEngine的实体构造函数，与服务器脚本对应。
## 存在于这样的构造函数是因为KBE需要创建好实体并将属性等数据填充好才能告诉脚本层初始化
func __init__()-> void:
	pass

func callPropertysSetMethods()-> void:
	pass # 动态生成

func attachComponents()-> void:
	pass # 动态生成

func detachComponents()-> void:
	pass # 动态生成

func baseCall(_methodname:String, _arguments:Array = [])-> void:
	if KBEngine.app.currserver == "loginapp":
		Dbg.ERROR_MSG(self.className + "::baseCall(" + _methodname + "), currserver=!" + KBEngine.app.currserver)  
		return
	var module:ScriptModule = EntityDef.moduledefs.get(self.className)
	if not module:
		Dbg.ERROR_MSG("entity::baseCall:  entity-module(" + self.className + ") error, can not find from EntityDef.moduledefs")
		return
	var method:Method = module.base_methods.get(_methodname)
	if not method:
		Dbg.ERROR_MSG(self.className + "::baseCall(" + _methodname + "), not found method!")
		return
	var methodID:int = method.methodUtype
	if _arguments.size() != method.args.size():
		Dbg.ERROR_MSG(self.className + "::baseCall(" + _methodname + "): args(" + str(_arguments.size()) + "!= " + str(method.args.size()) + ") size is error!")
		return

	var entityCall:EntityCall = getBaseEntityCall()
	if entityCall == null:
		Dbg.ERROR_MSG(self.className + "::baseCall(" + _methodname + "), baseEntityCall is null")
		return
	entityCall.newCall()
	entityCall.bundle.writeUint16(0)
	entityCall.bundle.writeUint16(methodID)

	for i:int in range(method.args.size()):
		if method.args[i].isSameType(_arguments[i]):
			method.args[i].addToStream(entityCall.bundle, _arguments[i])
		else:
			Dbg.ERROR_MSG(self.className + "::baseCall(method=" + _methodname + "): args is error(arg" + str(i) + ": " + str(method.args[i]) + ")!")	
			entityCall.bundle = null
			return
	entityCall.sendCall(null)

func cellCall(_methodname:String, _arguments:Array = [])-> void:
	if KBEngine.app.currserver == "loginapp":
		Dbg.ERROR_MSG(self.className + "::cellCall(" + _methodname + "), currserver=!" + KBEngine.app.currserver)  
		return
	var module:ScriptModule = EntityDef.moduledefs.get(self.className)
	if not module:
		Dbg.ERROR_MSG("entity::cellCall:  entity-module(" + self.className + ") error, can not find from EntityDef.moduledefs!")
		return
	var method:Method = module.cell_methods.get(_methodname)
	if not method:
		Dbg.ERROR_MSG(self.className + "::cellCall(" + _methodname + "), not found method!")
		return
	var methodID:int = method.methodUtype
	if _arguments.size() != method.args.size():
		Dbg.ERROR_MSG(self.className + "::cellCall(" + _methodname + "): args(" + str(_arguments.size()) + "!= " + str(method.args.size()) + ") size is error!")
		return

	var entityCall:EntityCall = getCellEntityCall()
	if entityCall == null:
		Dbg.ERROR_MSG(self.className + "::cellCall(" + _methodname + "): no cell!")
		return
	entityCall.newCall()
	entityCall.bundle.writeUint16(0)
	entityCall.bundle.writeUint16(methodID)

	for i:int in range(method.args.size()):
		if method.args[i].isSameType(_arguments[i]):
			method.args[i].addToStream(entityCall.bundle, _arguments[i])
		else:
			Dbg.ERROR_MSG(self.className + "::cellCall(" + _methodname + "): args is error(arg" + str(i) + ": " + str(method.args[i]) + ")!")
			entityCall.bundle = null
			return
	entityCall.sendCall(null)

func enterWorld()-> void:
	Dbg.INFO_MSG(self.className + '::enterWorld: ' + str(self.id)) 
	self.inWorld = true
	self.onEnterWorld()
	self.onComponentsEnterworld()
	KBEEvent.Event.onEnterWorld.emit(self)

func onEnterWorld()-> void:
	pass

func leaveWorld()-> void:
	Dbg.INFO_MSG(self.className + '::leaveWorld: ' + str(self.id))
	self.inWorld = false
	self.onLeaveWorld()
	self.onComponentsLeaveworld()
	KBEEvent.Event.onLeaveWorld.emit(self)

func onLeaveWorld()-> void:
	pass

func enterSpace()-> void:
	Dbg.INFO_MSG(self.className + '::enterSpace: ' + str(self.id))
	self.inWorld = true
	self.onEnterSpace()
	KBEEvent.Event.onEnterSpace.emit(self)
	# 要立即刷新表现层对象的位置
	KBEEvent.Event.set_position.emit(self)
	KBEEvent.Event.set_direction.emit(self)

func onEnterSpace()-> void:
	pass

func leaveSpace()-> void:
	Dbg.INFO_MSG(self.className + '::leaveSpace: ' + str(self.id))
	self.inWorld = false
	self.onLeaveSpace()
	KBEEvent.Event.onLeaveSpace.emit(self)

func onLeaveSpace()-> void:
	pass

func onPositionChanged(_oldValue:Vector3)-> void:
	#Dbg.DEBUG_MSG(self.className + "::set_position: " + str(_oldValue) + " => " + str(self.position))
	if isPlayer():
		KBEngine.app.entityServerPos(position)
	if self.inWorld:
		KBEEvent.Event.set_position.emit(self)

func onUpdateVolatileData()-> void:
	pass

func onDirectionChanged(_oldValue:Vector3)-> void:
	#Dbg.DEBUG_MSG(self.className + "::set_direction: " + str(_oldValue) + " => " + str(self.direction))
	if self.inWorld:
		self.direction.x = self.direction.x * 360 / (PI * 2)
		self.direction.y = self.direction.y * 360 / (PI * 2)
		self.direction.z = self.direction.z * 360 / (PI * 2)
		KBEEvent.Event.set_direction.emit(self)
	else:
		self.direction = _oldValue

func onSpaceIDChanged(_oldValue:int)-> void:
	pass

## This callback method is called when the local entity control by the client has been enabled or disabled. 
## See the Entity.controlledBy() method in the CellApp server code for more infomation.
## <param name="isControlled">
## 对于玩家自身来说，它表示是否自己被其它玩家控制了；
## 对于其它entity来说，表示我本机是否控制了这个entity
## </param>
func onControlled(_bIsControlled:bool)-> void:
	pass

func getComponents(_componentName: String, _all: bool) -> Array[EntityComponent]:
	return []

## 平滑移动
func onSmoothPositionChanged(_oldValue:Vector3)-> void:
	if isPlayer():
		KBEngine.app.entityServerPos(position)
	if self.inWorld:
		KBEEvent.Event.updatePosition.emit(self)
