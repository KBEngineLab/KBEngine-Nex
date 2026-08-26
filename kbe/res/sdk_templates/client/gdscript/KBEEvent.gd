class_name KBEEvent
## 事件模块: KBEngine插件层与Godot表现层通过事件来交互
## 使用 Godot 原生 signals 实现，同时提供 registerOut/registerIn 兼容 API
## 参考 C# Event.cs

static var m_event:KBEEvent
static var mutex: Mutex = Mutex.new()

# 暂停 out 事件即时执行
static var _isPauseOut:bool = false

# out 事件待处理队列 (信号系统下通常为空，保留用于 pause/resume 语义)
static var firedEvents_out:Array[Dictionary] = []
static var doingEvents_out:Array[Dictionary] = []

# in 事件待处理队列
static var firedEvents_in:Array[Dictionary] = []
static var doingEvents_in:Array[Dictionary] = []

static var Event:KBEEvent:
	get:
		if (not m_event) or (not is_instance_valid(m_event)):
			mutex.lock() # 线程锁
			m_event = KBEEvent.new()
			mutex.unlock()
		return m_event


## KBE-Plugin fire-out events(KBE => Godot):

# ------------------------------------账号相关------------------------------------

## Create account feedback results.
## [color=aqua] param1(uint16): retcode. // server_errors[/color]
## [color=aqua] param2(bytes): datas. // If you use third-party account system, the system may fill some of the third-party additional datas. [/color]
signal onCreateAccountResult(_retcode:int, _datas:PackedByteArray)

## Response from binding account Email request.
## [color=aqua] param1(uint16): retcode. // server_errors[/color]
signal onBindAccountEmail(_retcode:int)

## Response from a new password request.
## [color=aqua] param1(uint16): retcode. // server_errors[/color]
signal onNewPassword(_retcode:int)

## Response from a reset password request.
## [color=aqua] param1(uint16): retcode. // server_errors[/color]
signal onResetPassword(_retcode:int)
## ------------------------------------连接相关------------------------------------

## Kicked of the current server.
## [color=aqua] param1(uint16): retcode. // server_errors[/color]
signal onKicked(_retcode:int)

## Disconnected from the server.
signal onDisconnected()

## Status of connection server.
## [color=aqua] param1(bool): success or fail[/color]
signal onConnectionState(_bSuccess:bool)

# ------------------------------------logon相关------------------------------------

## Engine version mismatch.
## [color=aqua] param1(string): clientVersion[/color]
## [color=aqua] param2(string): serverVersion[/color]
signal onVersionNotMatch(_clientVersion:String, _serverVersion:String)

## script version mismatch.
## [color=aqua] param1(string): clientScriptVersion[/color]
## [color=aqua] param2(string): serverScriptVersion[/color]
signal onScriptVersionNotMatch(_clientScriptVersion:String, _serverScriptVersion:String)

## Login failed.
## [color=aqua] param1(uint16): retcode. // server_errors[/color]
signal onLoginFailed(_retcode:int)

## Login to baseapp.
signal onLoginBaseapp()

## Login baseapp failed.
## [color=aqua] param1(uint16): retcode. // server_errors[/color]
signal onLoginBaseappFailed(_retcode:int)

## Relogin to baseapp.
signal onReloginBaseapp()

## Relogin baseapp success.
signal onReloginBaseappSuccessfully()

## Relogin baseapp failed.
## [color=aqua] param1(uint16): retcode. // server_errors[/color]
signal onReloginBaseappFailed(_retcode:int)


# ------------------------------------实体cell相关事件------------------------------------

## Entity enter the client-world.
## [color=aqua] param1: Entity[/color]
signal onEnterWorld(_entity:Entity)

## Entity leave the client-world.
## [color=aqua] param1: Entity[/color]
signal onLeaveWorld(_entity:Entity)

## Player enter the new space.
## [color=aqua] param1: Entity[/color]
signal onEnterSpace(_entity:Entity)

## Player leave the space.
## [color=aqua] param1: Entity[/color]
signal onLeaveSpace(_entity:Entity)

## Sets the current position of the entity.
## [color=aqua] param1: Entity[/color]
signal set_position(_entity:Entity)

## Sets the current direction of the entity.
## [color=aqua] param1: Entity[/color]
signal set_direction(_entity:Entity)

## The entity position is updated, you can smooth the moving entity to new location.
## [color=aqua] param1: Entity[/color]
signal updatePosition(_entity:Entity)

## The current space is specified by the geometry mapping.
## Popular said is to load the specified Map Resources.
## [color=aqua] param1(string): resPath[/color]
signal addSpaceGeometryMapping(_resPath:String)

## Server spaceData set data.
## [color=aqua] param1(int32): spaceID[/color]
## [color=aqua] param2(string): key[/color]
## [color=aqua] param3(string): value[/color]
signal onSetSpaceData(_spaceID:int, _key:String, _value:String)

## Start downloading data.
## [color=aqua] param1(int32): rspaceID[/color]
## [color=aqua] param2(string): key[/color]
signal onDelSpaceData(_spaceID:int, _key:String)

## Triggered when the entity is controlled or out of control.
## [color=aqua] param1: Entity[/color]
## [color=aqua] param2(bool): isControlled[/color]
signal onControlled(_entity:Entity, _isControlled:bool)

## Lose controlled entity.
## [color=aqua] param1: Entity[/color]
signal onLoseControlledEntity(_entity:Entity)

# ------------------------------------数据下载相关------------------------------------

## Start downloading data.
## [color=aqua] param1(uint16): resouce id[/color]
## [color=aqua] param2(uint32): data size[/color]
## [color=aqua] param3(string): description[/color]
signal onStreamDataStarted(_resourceID:int, _dataSize:int, _description:String)

## Receive data.
## [color=aqua] param1(uint16): resouce id[/color]
## [color=aqua] param2(bytes): datas[/color]
signal onStreamDataRecv(_resourceID:int, _datas:PackedByteArray)


## The downloaded data is completed.
## [color=aqua] param1(uint16): resouce id[/color]
signal onStreamDataCompleted(_resourceID:int)


## KBE-Plugin fire-in events(Godot => KBE):

## Create new account.
signal createAccount(_accountName:String, _password:String, _datas:PackedByteArray)

## Login to server.
signal login(_accountName:String, _password:String, _datas:PackedByteArray)

## Logout to baseapp, called when exiting the client.
signal logout()

## Relogin to baseapp.
signal reloginBaseapp()

## Reset password.
signal resetPassword(_accountName:String)

## Request to set up a new password for the account. Note: account must be online.
signal newPassword(_oldPassword:String, _newPassword:String)

## Request server binding account Email.
signal bindAccountEmail(_emailAddress:String)

signal closeNetwork


# ============================================================================
# registerOut / registerIn — 兼容 C# Event API 的字符串注册方式
# ============================================================================

## 注册 out 事件监听 (KBE→App)
## 用法: KBEEvent.registerOut("onKicked", self, "onKicked")
static func registerOut(_eventname:String, _obj:Object, _funcname:String)-> bool:
	if not (_obj is Object) or not _obj.has_method(_funcname):
		Dbg.ERROR_MSG("KBEEvent::registerOut: object has no method " + _funcname)
		return false
	# 检查信号是否存在
	if not _hasSignal(_eventname):
		Dbg.ERROR_MSG("KBEEvent::registerOut: unknown event " + _eventname)
		return false
	var _err:int = Event.connect(_eventname, Callable(_obj, _funcname))
	return _err == OK

## 注销 out 事件监听
static func deregisterOut(_eventname:String, _obj:Object, _funcname:String)-> bool:
	if not _hasSignal(_eventname):
		return false
	if Event.is_connected(_eventname, Callable(_obj, _funcname)):
		Event.disconnect(_eventname, Callable(_obj, _funcname))
		return true
	return false

## 注册 in 事件监听 (App→KBE)  
## 用法: KBEEvent.registerIn("login", self, "login")
static func registerIn(_eventname:String, _obj:Object, _funcname:String)-> bool:
	# in 事件也是通过 signal 连接，与 out 相同
	return registerOut(_eventname, _obj, _funcname)

## 注销 in 事件监听
static func deregisterIn(_eventname:String, _obj:Object, _funcname:String)-> bool:
	return deregisterOut(_eventname, _obj, _funcname)


# ============================================================================
# fireIn / processInEvents — 入队式 in 事件分发
# ============================================================================

## 触发 in 事件 (入队，由 processInEvents 消费)
## 参数 _args 对应 signal 的参数列表
static func fireIn(_eventname:String, _args:Array = [])-> void:
	mutex.lock()
	firedEvents_in.append({"name": _eventname, "args": _args})
	mutex.unlock()

## 处理所有待处理的 in 事件
## 应在主线程的 _process 或 _physics_process 中调用
static func processInEvents()-> void:
	if firedEvents_in.is_empty():
		return
	mutex.lock()
	doingEvents_in = firedEvents_in.duplicate()
	firedEvents_in.clear()
	mutex.unlock()
	
	for _evt:Dictionary in doingEvents_in:
		if _hasSignal(_evt["name"]):
			Event.emit_signal.callv([_evt["name"]] + _evt["args"])
	doingEvents_in.clear()

## 触发 out 事件 (如果未暂停则立即执行，否则入队)
static func fireOut(_eventname:String, _args:Array = [])-> void:
	if _isPauseOut:
		mutex.lock()
		firedEvents_out.append({"name": _eventname, "args": _args})
		mutex.unlock()
	else:
		if _hasSignal(_eventname):
			Event.emit_signal.callv([_eventname] + _args)

## 处理 out 事件队列 (信号系统下通常立即执行，保留用于 pause/resume 场景)
static func processOutEvents()-> void:
	if _isPauseOut:
		return
	if firedEvents_out.is_empty():
		return
	mutex.lock()
	doingEvents_out = firedEvents_out.duplicate()
	firedEvents_out.clear()
	mutex.unlock()
	
	for _evt:Dictionary in doingEvents_out:
		if _hasSignal(_evt["name"]):
			Event.emit_signal.callv([_evt["name"]] + _evt["args"])
	doingEvents_out.clear()

# ============================================================================
# pause / resume / clearFiredEvents
# ============================================================================

## 暂停 out 事件 (fireOut 将不再立即调用监听者，只保留队列语义)
static func pause()-> void:
	_isPauseOut = true

## 恢复 out 事件，并处理暂停期间积累的 out 队列
static func resume()-> void:
	_isPauseOut = false
	processOutEvents()

## 当前是否处于暂停状态
static func isPause()-> bool:
	return _isPauseOut

## 清理已触发但尚未处理的事件队列
## 注意：不注销已注册的监听者（与 C# 版本语义一致）
static func clearFiredEvents()-> void:
	mutex.lock()
	firedEvents_out.clear()
	firedEvents_in.clear()
	mutex.unlock()
	doingEvents_out.clear()
	doingEvents_in.clear()
	_isPauseOut = false

# ============================================================================
# 完整清理 / 注销
# ============================================================================

## 清理所有事件注册和已触发队列（慎用！会断开所有信号连接）
static func clear()-> void:
	clearFiredEvents()
	# 断开所有信号连接
	mutex.lock()
	for _conn:Dictionary in Event.get_signal_list():
		var _signalName:StringName = _conn["name"]
		if Event.has_connections(_signalName):
			for _signalInfo:Dictionary in Event.get_signal_connection_list(_signalName):
				var cb:Callable = _signalInfo.get("callable")
				Event.disconnect(_signalName, cb)
	mutex.unlock()

## 遍历所有注册的信号，解除与_obj相关的连接，开销较大，慎用
static func deregister(_obj:Object)-> void:
	if not _obj:
		return
	mutex.lock()
	for _conn:Dictionary in Event.get_signal_list():
		var _signalName:StringName = _conn["name"]
		if Event.has_connections(_signalName):
			for _signalInfo:Dictionary in Event.get_signal_connection_list(_signalName):
				var cb:Callable = _signalInfo.get("callable")
				if _obj == cb.get_object():
					Event.disconnect(_signalName, cb)
	mutex.unlock()

# ============================================================================
# 内部工具
# ============================================================================

## 检查指定名称的信号是否存在
static func _hasSignal(_eventname:String)-> bool:
	for _conn:Dictionary in Event.get_signal_list():
		if _conn["name"] == _eventname:
			return true
	return false
