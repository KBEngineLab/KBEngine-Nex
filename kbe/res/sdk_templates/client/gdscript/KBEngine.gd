class_name KBEngine
## [b]KBEgine服务器的客户端模块[/b][br]
## 这是KBEngine插件的核心模块
## 包括网络创建、持久化协议、entities的管理、以及引起对外可调用接口。
##
## @tutorial: https://kbengine.github.io//cn/docs/programming/clientsdkprogramming.html
## @tutorial: http://www.kbengine.org/docs/programming/clientsdkprogramming.html
## @tutorial: http://www.kbengine.org/docs/programming/kbe_message_format.html	
## @tutorial: http://www.kbengine.org/cn/docs/programming/clientsdkprogramming.html
## @tutorial: http://www.kbengine.org/cn/docs/programming/kbe_message_format.html
#region 全局变量

enum CLIENT_TYPE
{
	CLIENT_TYPE_MOBILE				= 1, # Mobile(Phone, Pad)
	CLIENT_TYPE_WIN					= 2, # Windows Application program
	CLIENT_TYPE_LINUX				= 3, # Linux Application program
	CLIENT_TYPE_MAC					= 4, # Mac Application program
	CLIENT_TYPE_BROWSER				= 5, # Web，HTML5，Flash
	CLIENT_TYPE_BOTS				= 6, # bots
	CLIENT_TYPE_MINI				= 7, # Mini-Client
}

enum NETWORK_ENCRYPT_TYPE  # 加密通信类型
{
	ENCRYPT_TYPE_NONE = 0,  # 无加密
	ENCRYPT_TYPE_BLOWFISH = 1,  # Blowfish
}

enum NETWORK_TYPE
{
	TCP = 0,
	KCP = 1,
	GODOT_WEB_SOCKET = 2,
}

const PACKET_MAX_SIZE:int          = 1460*4
const PACKET_MAX_SIZE_TCP:int      = 1460
const PACKET_MAX_SIZE_UDP:int      = 1472
const KBE_FLT_MAX:float            = INF
#-----------------------------------------------------------------------------------------
#										global property
#-----------------------------------------------------------------------------------------
var username:String = "kbengine"
var password:String = "123456"

# 服务端分配的baseapp地址
var baseappIP:String
var baseappTcpPort:int
var baseappUdpPort:int

# 当前状态
var currserver:String
var currstate:String

# 服务端下行以及客户端上行用于登录时处理的账号绑定的二进制信息
# 该信息由用户自己进行扩展
var m_serverdatas:PackedByteArray
var m_clientdatas:PackedByteArray

# 通信协议加密，blowfish协议
var m_encryptedKey:PackedByteArray

# 服务端与客户端的版本号以及协议MD5
var serverVersion:String
var clientVersion:String = "@{KBE_VERSION}"
var serverScriptVersion:String
var clientScriptVersion:String = "@{KBE_SCRIPT_VERSION}"
var serverProtocolMD5:String = "@{KBE_SERVER_PROTO_MD5}"
var serverEntitydefMD5:String = "@{KBE_SERVER_ENTITYDEF_MD5}"

# 当前玩家的实体id与实体类别
var entity_uuid:int
var entity_id:int
var entity_type:String

var m_controlledEntities:Array[Entity]
# 当前服务端最后一次同步过来的玩家位置
var m_entityServerPos:Vector3
# space的数据，具体看API手册关于spaceData
# @tutorial: https://github.com/kbengine/kbengine/tree/master/docs/api
var m_spacedatas:Dictionary[String, String]
# 所有实体都保存于这里， 请参看API手册关于entities部分
# @tutorial: https://github.com/kbengine/kbengine/tree/master/docs/api
var entities:Dictionary[int, Entity]

# 在玩家View范围小于256个实体时我们可以通过一字节索引来找到entity
var m_entityIDAliasIDList:Array[int]
var m_bufferedCreateEntityMessages:Dictionary[int, MemoryStream]

# 所有服务端错误码对应的错误描述
var m_serverErrs:ServerErrorDescrs = ServerErrorDescrs.new() 
var m_lastTickTime:int = Time.get_ticks_msec() # 毫秒
var m_lastTickCBTime:int = Time.get_ticks_msec() # 毫秒
var m_lastUpdateToServerTime:int = Time.get_ticks_msec() # 毫秒
#上传玩家信息到服务器间隔，单位毫秒
var m_updatePlayerToServerPeroid:float = 100.0
const _1MS_TO_100NS:int = 10000
#加密过滤器
var m_filter:EncryptionFilter
# 玩家当前所在空间的id， 以及空间对应的资源
var spaceID:int
var spaceResPath:String
var isLoadedGeometry:bool = false
# 按照标准，每个客户端部分都应该包含这个属性
const component:String = "client" 
#endregion

var m_networkInterface:NetworkInterfaceBase
var m_args:KBEngineArgs

static var m_app:KBEngine

static var loginappMessages:Dictionary[int, Messages.Message] = Messages.loginappMessages
static var baseappMessages:Dictionary[int, Messages.Message] = Messages.baseappMessages
static var clientMessages:Dictionary[int, Messages.Message] = Messages.clientMessages
static var messages:Dictionary[String, Messages.Message] = Messages.messages

static var app:KBEngine:
	get:
		return getSingleton()

static func getSingleton()-> KBEngine:
	if not m_app:
		Dbg.ERROR_MSG("KBEngineApp is null")
	return m_app

func _init(_args:KBEngineArgs) -> void:
	if m_app:
		print_stack()
		Dbg.ERROR_MSG("Only one instance of KBEngineApp!")
		return
	m_app = self
	initialize(_args)

func initialize(_args:KBEngineArgs)-> bool:
	self.m_args = _args
	self.m_updatePlayerToServerPeroid = _args.syncPlayerMS
	EntityDef.init()
	initNetwork()
	installEvents()
	return true

func initNetwork()-> void:
	self.m_filter = null
	Messages.init()
	if self.m_args.networkType == NETWORK_TYPE.GODOT_WEB_SOCKET:
		self.m_networkInterface = NetworkInterfaceWS.new()
	else:
		# TCP 和 KCP 都先用 TCP 连接 loginapp，KCP 在连接 baseapp 时切换
		self.m_networkInterface = NetworkInterfaceTCP.new()

func installEvents()-> void:
	KBEEvent.registerIn("createAccount", self, "createAccount")
	KBEEvent.registerIn("login", self, "login")
	KBEEvent.registerIn("logout", self, "logout")
	KBEEvent.registerIn("reloginBaseapp", self, "reloginBaseapp")
	KBEEvent.registerIn("resetPassword", self, "resetPassword")
	KBEEvent.registerIn("bindAccountEmail", self, "bindAccountEmail")
	KBEEvent.registerIn("newPassword", self, "newPassword")
	KBEEvent.registerIn("closeNetwork", self, "_closeNetwork")

func getInitArgs()-> KBEngineArgs:
	return self.m_args

func destroy()-> void:
	Dbg.WARNING_MSG("KBEngine::destroy()")
	if self.currserver == "baseapp":
		logout()
	reset()
	KBEEvent.deregister(self)
	resetMessages()
	KBEngine.m_app = null

func networkInterface()-> NetworkInterfaceBase:
	return self.m_networkInterface

func serverdatas()-> PackedByteArray:
	return self.m_serverdatas

func entityServerPos(_pos:Vector3)-> void:
	self.m_entityServerPos = _pos

func resetMessages()-> void:
	self.m_serverErrs.Clear()
	Messages.clear()
	EntityDef.reset()
	Dbg.DEBUG_MSG("KBEngine::resetMessages()")

func reset()-> void:
	KBEEvent.clearFiredEvents()
	clearEntities(true)
	self.currserver = ""
	self.currstate = ""
	self.m_serverdatas = PackedByteArray()
	self.serverVersion = ""
	self.serverScriptVersion = ""
	self.entity_uuid = 0
	self.entity_id = 0
	self.entity_type = ""
	self.m_entityIDAliasIDList.clear()
	self.m_bufferedCreateEntityMessages.clear()
	self.m_lastTickTime = Time.get_ticks_msec()
	self.m_lastTickCBTime = Time.get_ticks_msec()
	self.m_lastUpdateToServerTime = Time.get_ticks_msec()
	self.spaceID = 0
	self.spaceResPath = ""
	self.isLoadedGeometry = false
	if self.m_networkInterface:
		self.m_networkInterface.reset()
	self.m_filter = null
	if self.m_args.networkType == NETWORK_TYPE.GODOT_WEB_SOCKET:
		self.m_networkInterface = NetworkInterfaceWS.new()
	else:
		# TCP 和 KCP 都先用 TCP，KCP 在 _connectToBaseapp 时切换
		self.m_networkInterface = NetworkInterfaceTCP.new()
	self.m_spacedatas.clear()

## 插件的主循环处理函数
func process()-> void:
	# 处理网络
	if self.m_networkInterface:
		self.m_networkInterface.process()
	# 处理待处理的 in 事件
	KBEEvent.processInEvents()
	# 向服务端发送心跳以及同步角色信息到服务端
	sendTick()

## 当前玩家entity
func player()-> Entity:
	return self.entities.get(self.entity_id, null)

func _closeNetwork(_networkInterface:NetworkInterfaceBase)-> void:
	_networkInterface.close()

## 向服务端发送心跳以及同步角色信息到服务端
func sendTick()-> void:
	if self.m_networkInterface == null or self.m_networkInterface.connected == false:
		return
	var span:int = Time.get_ticks_msec() - self.m_lastTickTime
	# 更新玩家的位置与朝向到服务端
	updatePlayerToServer()
	if self.m_args.serverHeartbeatTick > 0 \
			and span > self.m_args.serverHeartbeatTick * 1000:
		span = self.m_lastTickCBTime - self.m_lastTickTime
		# 如果心跳回调接收时间小于心跳发送时间，说明没有收到回调
		# 此时应该通知客户端掉线了
		if span < 0:
			Dbg.ERROR_MSG("sendTick: Receive appTick timeout!")
			self.m_networkInterface.close()
			return

		var Loginapp_onClientActiveTickMsg:Messages.Message = Messages.messages.get("Loginapp_onClientActiveTick")
		var Baseapp_onClientActiveTickMsg:Messages.Message = Messages.messages.get("Baseapp_onClientActiveTick")
		if self.currserver == "loginapp":
			if Loginapp_onClientActiveTickMsg:
				var bundle:Bundle = ObjectPool.createObject(Bundle)
				bundle.newMessage(Loginapp_onClientActiveTickMsg)
				bundle.send(self.m_networkInterface)
		else:
			if Baseapp_onClientActiveTickMsg:
				var bundle:Bundle = ObjectPool.createObject(Bundle)
				bundle.newMessage(Baseapp_onClientActiveTickMsg)
				bundle.send(self.m_networkInterface)
		self.m_lastTickTime = Time.get_ticks_msec()

## 服务器心跳回调
func Client_onAppActiveTickCB()-> void:
	self.m_lastTickCBTime = Time.get_ticks_msec()

## 与服务端握手，与任何一个进程连接之后应该第一时间进行握手
func hello()-> void:
	var bundle:Bundle = ObjectPool.createObject(Bundle)
	if self.currserver == "loginapp":
		bundle.newMessage(Messages.messages["Loginapp_hello"])
	else:
		bundle.newMessage(Messages.messages["Baseapp_hello"])
	self.m_filter = null
	if self.m_args.networkEncryptType == NETWORK_ENCRYPT_TYPE.ENCRYPT_TYPE_BLOWFISH:
		self.m_filter = BlowfishFilter.new()
		self.m_encryptedKey = self.m_filter.key()
		self.m_networkInterface.setFilter(null)
	bundle.writeString(clientVersion)
	bundle.writeString(clientScriptVersion)
	bundle.writeBlob(self.m_encryptedKey)
	bundle.send(self.m_networkInterface)

## 握手之后服务端的回调
func Client_onHelloCB(_stream:MemoryStream)-> void:
	var str_serverVersion:String = _stream.readString()
	self.serverScriptVersion = _stream.readString()
	var currentServerProtocolMD5:String = _stream.readString()
	var currentServerEntitydefMD5:String = _stream.readString()
	var ctype:int = _stream.readInt32()
	
	Dbg.DEBUG_MSG("KBEngine::Client_onHelloCB: verInfo(" + str_serverVersion
		+ "), scriptVersion("+ serverScriptVersion + "), srvProtocolMD5("+ serverProtocolMD5 
		+ "), srvEntitydefMD5("+ serverEntitydefMD5 + "), + ctype(" + str(ctype) + ")!")

	if str_serverVersion != "Getting":
		self.serverVersion = str_serverVersion
		#if self.serverProtocolMD5 != currentServerProtocolMD5:
			#if self.m_args.strictProtocolMD5:
				#Dbg.ERROR_MSG("Client_onHelloCB: digest not match! serverProtocolMD5=" + serverProtocolMD5 + "(server: " + currentServerProtocolMD5 + ")")
				#KBEEvent.Event.onVersionNotMatch.emit(self.clientVersion, self.serverVersion)
				#return
			#Dbg.DEBUG_MSG("Client_onHelloCB: protocol digest differs but strictProtocolMD5 is disabled. client=" + serverProtocolMD5 + ", server=" + currentServerProtocolMD5)
		
		if self.serverEntitydefMD5 != currentServerEntitydefMD5:
			Dbg.ERROR_MSG("Client_onHelloCB: digest not match! serverEntitydefMD5=" + serverEntitydefMD5 + "(server: " + currentServerEntitydefMD5 + ")")
			KBEEvent.Event.onVersionNotMatch.emit(self.clientVersion, self.serverVersion)
			return

	if self.m_args.networkEncryptType == NETWORK_ENCRYPT_TYPE.ENCRYPT_TYPE_BLOWFISH:
		self.m_networkInterface.setFilter(self.m_filter)
		self.m_filter = null

	onServerDigest()
	if currserver == "baseapp":
		onLogin_baseapp()
	else:
		onLogin_loginapp()

## 服务端错误描述导入了
func Client_onImportServerErrorsDescr(_stream:MemoryStream)-> void:
	pass  # 无需实现，已由插件生成静态代码

## 从服务端返回的二进制流导入客户端消息协议
func Client_onImportClientMessages(_stream:MemoryStream)-> void:
	pass  # 无需实现，已由插件生成静态代码

## 从服务端返回的二进制流导入客户端消息协议
func Client_onImportClientEntityDef(_stream:MemoryStream)-> void:
	pass  # 无需实现，已由插件生成静态代码

## 引擎版本不匹配
func Client_onVersionNotMatch(_stream:MemoryStream)-> void:
	self.serverVersion = _stream.readString()
	Dbg.ERROR_MSG("Client_onVersionNotMatch: verInfo=" + clientVersion + "(server: " + serverVersion + ")")
	KBEEvent.Event.onVersionNotMatch.emit(self.clientVersion, self.serverVersion)

## 脚本版本不匹配
func Client_onScriptVersionNotMatch(_stream:MemoryStream)-> void:
	self.serverScriptVersion = _stream.readString()
	Dbg.ERROR_MSG("Client_onScriptVersionNotMatch: verInfo=" + clientScriptVersion + "(server: " + serverScriptVersion + ")")
	KBEEvent.Event.onScriptVersionNotMatch.emit(self.clientScriptVersion, self.serverScriptVersion)

## 被服务端踢出
func Client_onKicked(_failedcode:int)-> void:
	Dbg.DEBUG_MSG("Client_onKicked: failedcode=" + str(_failedcode) + "(" + serverErr(_failedcode) + ")")
	KBEEvent.Event.onKicked.emit(_failedcode)

## 登录到服务端，必须登录完成loginapp与网关(baseapp)，登录流程才算完毕
func login(_username: String, _password: String, _datas: PackedByteArray)-> void:
	self.username = _username
	self.password = _password
	self.m_clientdatas = _datas
	login_loginapp(true)

## 登录到服务端(loginapp), 登录成功后还必须登录到网关(baseapp)登录流程才算完毕
func login_loginapp(_noconnect:bool)-> void:
	if _noconnect:
		reset()
		self.m_networkInterface.connectTo(self.m_args.ip, self.m_args.port, onConnectTo_loginapp_callback, null)
	else:
		Dbg.DEBUG_MSG("KBEngine::login_loginapp(): send login! username=" + username)
		var bundle:Bundle = ObjectPool.createObject(Bundle)
		bundle.newMessage(Messages.messages["Loginapp_login"])
		bundle.writeInt8(int(self.m_args.clientType))
		bundle.writeBlob(self.m_clientdatas)
		bundle.writeString(self.username)
		bundle.writeString(self.password)
		bundle.send(self.m_networkInterface)

func onConnectTo_loginapp_callback(_ip:String, _port:int, _success:bool, _userData:Object)-> void:
	self.m_lastTickCBTime = Time.get_ticks_msec()
	if not _success:
		Dbg.ERROR_MSG("KBEngine::login_loginapp(): connect %s:%s error!" % [_ip, _port])  
		return
	self.currserver = "loginapp"
	self.currstate = "login"
	Dbg.DEBUG_MSG("KBEngine::login_loginapp(): connect %s:%s success!" % [_ip, _port])
	hello()

func onLogin_loginapp()-> void:
	self.m_lastTickCBTime = Time.get_ticks_msec()
	login_loginapp(false)


## 登录到服务端，登录到网关(baseapp)
func login_baseapp(_noconnect:bool)-> void:
	if _noconnect:
		KBEEvent.Event.onLoginBaseapp.emit()
		self.m_networkInterface.reset()
		_connectToBaseapp(onConnectTo_baseapp_callback)
	else:
		var bundle:Bundle = ObjectPool.createObject(Bundle)
		bundle.newMessage(Messages.messages["Baseapp_loginBaseapp"])
		bundle.writeString(username)
		bundle.writeString(password)
		bundle.send(self.m_networkInterface)

func onConnectTo_baseapp_callback(_ip:String, _port:int, _success:bool, _userData:Object)-> void:
	self.m_lastTickCBTime = Time.get_ticks_msec()
	if not _success:
		Dbg.ERROR_MSG("KBEngine::login_baseapp(): connect {0}:{1} error!".format([_ip, _port]))
		return
	self.currserver = "baseapp"
	self.currstate = ""
	Dbg.DEBUG_MSG("KBEngine::login_baseapp(): connect {0}:{1} success!".format([_ip, _port]))
	hello()

func onLogin_baseapp()-> void:
	self.m_lastTickCBTime = Time.get_ticks_msec()
	login_baseapp(false)

## 重登录到网关(baseapp)
## 一些移动类应用容易掉线，可以使用该功能快速的重新与服务端建立通信
func reloginBaseapp()-> void:
	self.m_lastTickTime = Time.get_ticks_msec()
	self.m_lastTickCBTime = Time.get_ticks_msec()
	if self.m_networkInterface.valid():
		return
	KBEEvent.Event.onReloginBaseapp.emit()
	self.m_networkInterface.reset()
	_connectToBaseapp(onReConnectTo_baseapp_callback)

func onReConnectTo_baseapp_callback(_ip:String, _port:int, _success:bool, _userData:Object)-> void:
	if not _success:
		Dbg.ERROR_MSG("KBEngine::reloginBaseapp(): connect {0}:{1} error!".format([_ip, _port]))
		return
	Dbg.DEBUG_MSG("KBEngine::relogin_baseapp(): connect {0}:{1} success!".format([_ip, _port]))

	var bundle:Bundle = ObjectPool.createObject(Bundle)
	bundle.newMessage(Messages.messages["Baseapp_reloginBaseapp"])
	bundle.writeString(self.username)
	bundle.writeString(self.password)
	bundle.writeUint64(self.entity_uuid)
	bundle.writeInt32(self.entity_id)
	bundle.send(self.m_networkInterface)
	self.m_lastTickCBTime = Time.get_ticks_msec()

## 根据网络类型连接到baseapp
func _connectToBaseapp(_callback:Callable)-> void:
	if self.m_args.networkType == NETWORK_TYPE.GODOT_WEB_SOCKET:
		self.m_networkInterface = NetworkInterfaceWS.new()
		self.m_networkInterface.connectTo(self.baseappIP, self.baseappTcpPort, _callback, null)
	elif self.m_args.networkType == NETWORK_TYPE.KCP and self.baseappUdpPort != 0:
		self.m_networkInterface = NetworkInterfaceKCP.new()
		self.m_networkInterface.connectTo(self.baseappIP, self.baseappUdpPort, _callback, null)
	else:
		if self.m_args.networkType == NETWORK_TYPE.KCP:
			# 旧服务端可能不提供 UDP 端点；自动回退 TCP 保持首次登录与重登录可用。
			# Older servers may not expose UDP; automatically fall back to TCP for both login and relogin.
			Dbg.WARNING_MSG("KBEngine::_connectToBaseapp: UDP port is unavailable, falling back to TCP.")
		self.m_networkInterface = NetworkInterfaceTCP.new()
		self.m_networkInterface.connectTo(self.baseappIP, self.baseappTcpPort, _callback, null)

## 登出baseapp
func logout()-> void:
	var bundle:Bundle = ObjectPool.createObject(Bundle)
	bundle.newMessage(Messages.messages["Baseapp_logoutBaseapp"])
	bundle.writeUint64(self.entity_uuid)
	bundle.writeInt32(self.entity_id)
	bundle.send(self.m_networkInterface)

## 通过错误id得到错误描述
func serverErr(_id:int)-> String:
	return self.m_serverErrs.serverErrStr(_id)

func onOpenLoginapp_resetpassword()-> void:
	Dbg.DEBUG_MSG("KBEngine::onOpenLoginapp_resetpassword: successfully!")
	self.currserver = "loginapp"
	self.currstate = "resetpassword"
	self.m_lastTickCBTime = Time.get_ticks_msec()
	resetpassword_loginapp(false)

## 重置密码, 通过loginapp
func resetPassword(_username:String)-> void:
	self.username = _username
	resetpassword_loginapp(true)

## 重置密码, 通过loginapp
func resetpassword_loginapp(_noconnect:bool)-> void:
	if _noconnect:
		reset()
		self.m_networkInterface.connectTo(self.m_args.ip, self.m_args.port, onConnectTo_resetpassword_callback, null)
	else:
		var bundle:Bundle = ObjectPool.createObject(Bundle)
		bundle.newMessage(Messages.messages["Loginapp_reqAccountResetPassword"])
		bundle.writeString(self.username)
		bundle.send(self.m_networkInterface)

func onConnectTo_resetpassword_callback(_ip:String, _port:int, _success:bool, _userData:Object)-> void:
	self.m_lastTickCBTime = Time.get_ticks_msec()
	if not _success:
		Dbg.ERROR_MSG("KBEngine::resetpassword_loginapp(): connect {0}:{1} error!".format([_ip, _port]))
		return
	Dbg.DEBUG_MSG("KBEngine::resetpassword_loginapp(): connect {0}:{1} success!".format([_ip, _port]))
	onOpenLoginapp_resetpassword()

func Client_onReqAccountResetPasswordCB(_failcode:int)-> void:
	KBEEvent.Event.onResetPassword.emit(_failcode)
	if _failcode != 0:
		Dbg.ERROR_MSG("KBEngine::Client_onReqAccountResetPasswordCB: " + self.username + " failed! code=" + str(_failcode) + "(" + self.serverErr(_failcode) + ")!")
		return
	Dbg.DEBUG_MSG("KBEngine::Client_onReqAccountResetPasswordCB: " + self.username + " success!")

## 绑定Email，通过baseapp
func bindAccountEmail(_emailAddress:String)-> void:
	var bundle:Bundle = ObjectPool.createObject(Bundle)
	bundle.newMessage(Messages.messages["Baseapp_reqAccountBindEmail"])
	bundle.writeInt32(self.entity_id)
	bundle.writeString(self.password)
	bundle.writeString(_emailAddress)
	bundle.send(self.m_networkInterface)

func Client_onReqAccountBindEmailCB(_failcode:int)-> void:
	KBEEvent.Event.onBindAccountEmail.emit(_failcode)
	if _failcode != 0:
		Dbg.ERROR_MSG("KBEngine::Client_onReqAccountBindEmailCB: " + self.username + " failed! code=" + str(_failcode) + "(" + self.serverErr(_failcode) + ")!")
		return
	Dbg.DEBUG_MSG("KBEngine::Client_onReqAccountBindEmailCB: " + self.username + " success!")

## 设置新密码，通过baseapp， 必须玩家登录在线操作所以是baseapp。
func newPassword(old_password:String, new_password:String)-> void:
	var bundle:Bundle = ObjectPool.createObject(Bundle)
	bundle.newMessage(Messages.messages["Baseapp_reqAccountNewPassword"])
	bundle.writeInt32(self.entity_id)
	bundle.writeString(old_password)
	bundle.writeString(new_password)
	bundle.send(self.m_networkInterface)

func Client_onReqAccountNewPasswordCB(_failcode:int)-> void:
	KBEEvent.Event.onNewPassword.emit(_failcode)
	if _failcode != 0:
		Dbg.ERROR_MSG("KBEngine::Client_onReqAccountNewPasswordCB: " + self.username + " failed! code=" + str(_failcode) + "(" + self.serverErr(_failcode) + ")!")
		return
	Dbg.DEBUG_MSG("KBEngine::Client_onReqAccountNewPasswordCB: " + self.username + " success!")

func createAccount(_username:String, _password:String, _datas:PackedByteArray)-> void:
	self.username = _username
	self.password = _password
	self.m_clientdatas = _datas
	self.createAccount_loginapp(true)

## 创建账号，通过loginapp
func createAccount_loginapp(_noconnect:bool)-> void:
	if _noconnect:
		reset()
		self.m_networkInterface.connectTo(self.m_args.ip, self.m_args.port, onConnectTo_createAccount_callback, null)
	else:
		var bundle:Bundle = ObjectPool.createObject(Bundle)
		bundle.newMessage(Messages.messages["Loginapp_reqCreateAccount"])
		bundle.writeString(self.username)
		bundle.writeString(self.password)
		bundle.writeBlob(self.m_clientdatas)
		bundle.send(self.m_networkInterface)

func onOpenLoginapp_createAccount()-> void: 
	Dbg.DEBUG_MSG("KBEngine::onOpenLoginapp_createAccount: successfully!")
	self.currserver = "loginapp"
	self.currstate = "createAccount"
	self.m_lastTickCBTime = Time.get_ticks_msec()
	createAccount_loginapp(false)

func onConnectTo_createAccount_callback(_ip:String, _port:int, _success:bool, _userData:Object)-> void:
	self.m_lastTickCBTime = Time.get_ticks_msec()
	if not _success:
		Dbg.ERROR_MSG("KBEngine::createAccount_loginapp(): connect {0}:{1} error!".format([_ip, _port]))
		return
	Dbg.DEBUG_MSG("KBEngine::createAccount_loginapp(): connect {0}:{1} success!".format([_ip, _port])) 
	onOpenLoginapp_createAccount()

## 获得了服务端摘要信息， 摘要包括协议MD5， entitydefMD5
func onServerDigest()-> void:
	pass

## 登录loginapp失败了
func Client_onLoginFailed(_stream:MemoryStream)-> void:
	var _failedcode = _stream.readUint16()
	self.m_serverdatas = _stream.readBlob()
	Dbg.ERROR_MSG("KBEngine::Client_onLoginFailed: failedcode(" + str(_failedcode) + ":" + serverErr(_failedcode) + "), datas(" + str(self.m_serverdatas.size()) + ")!")
	KBEEvent.Event.onLoginFailed.emit(_failedcode, self.m_serverdatas)
	if self.m_networkInterface:
		self.m_networkInterface.close()
	self.currserver = ""
	self.currstate = ""

## 登录loginapp成功了
func Client_onLoginSuccessfully(_stream:MemoryStream)-> void:
	var accountName:String = _stream.readString()
	self.username = accountName
	self.baseappIP = _stream.readString()
	self.baseappTcpPort = _stream.readUint16()
	self.baseappUdpPort = _stream.readUint16()
	self.m_serverdatas = _stream.readBlob()
	Dbg.DEBUG_MSG("KBEngine::Client_onLoginSuccessfully: accountName(" + accountName + "), addr(" + 
			baseappIP + ":" + str(baseappTcpPort) + "|" + str(baseappUdpPort) + "), datas(" + str(self.m_serverdatas.size()) + ")!")
	login_baseapp(true)

## 登录baseapp失败了
func Client_onLoginBaseappFailed(_failedcode:int)-> void:
	Dbg.ERROR_MSG("KBEngine::Client_onLoginBaseappFailed: failedcode=" + str(_failedcode) + "("+ serverErr(_failedcode) + ")!")
	KBEEvent.Event.onLoginBaseappFailed.emit(_failedcode)

## 重登录baseapp失败了
func Client_onReloginBaseappFailed(_failedcode:int)-> void:
	Dbg.ERROR_MSG("KBEngine::Client_onReloginBaseappFailed: failedcode=" + str(_failedcode) + "(" + serverErr(_failedcode) + ")!")
	KBEEvent.Event.onReloginBaseappFailed.emit(_failedcode)

## 登录baseapp成功了
func Client_onReloginBaseappSuccessfully(_stream:MemoryStream)-> void:
	self.entity_uuid = _stream.readUint64()
	Dbg.DEBUG_MSG("KBEngine::Client_onReloginBaseappSuccessfully: name(" + username + ")!")
	KBEEvent.Event.onReloginBaseappSuccessfully.emit()

## 服务端通知创建一个角色
func Client_onCreatedProxies(_rndUUID:int, _eid:int, _entityType:String)-> void:
	Dbg.DEBUG_MSG("KBEngine::Client_onCreatedProxies: eid[{0}], entityType[{1}]!".format([_eid, _entityType]))
	self.entity_uuid = _rndUUID
	self.entity_id = _eid
	self.entity_type = _entityType
	
	if not self.entities.has(_eid):
		var module:ScriptModule = EntityDef.moduledefs.get(_entityType)
		if not module:
			Dbg.ERROR_MSG("KBEngine::Client_onCreatedProxies: not found module(" + str(_entityType) + ")!")
			return

		var runclass:Script = module.entityScript
		if not runclass:
			Dbg.ERROR_MSG("KBEngine::Client_onCreatedProxies: not found entityScript in type" + str(_entityType))
			return
		
		var _entity:Entity = CreateEntityByCls(runclass)
		_entity.id = _eid
		_entity.className = _entityType
		_entity.onGetBase()
		self.entities[_eid] = _entity
		var entityMessage:MemoryStream = self.m_bufferedCreateEntityMessages.get(_eid)
		if entityMessage:
			Client_onUpdatePropertys(entityMessage)
			self.m_bufferedCreateEntityMessages.erase(_eid)
			entityMessage.reclaimObject()
		_entity.__init__()
		_entity.attachComponents()
		_entity.inited = true
		
		if self.m_args.isOnInitCallPropertysSetMethods:
			_entity.callPropertysSetMethods()
	else:
		var entityMessage:MemoryStream = self.m_bufferedCreateEntityMessages.get(_eid)
		if entityMessage:
			Client_onUpdatePropertys(entityMessage)
			self.m_bufferedCreateEntityMessages.erase(_eid)
			entityMessage.reclaimObject()

func findEntity(_entityID:int)-> Entity:
	return self.entities.get(_entityID, null)

## 通过流数据获得View实体的ID
func getViewEntityIDFromStream(_stream:MemoryStream)-> int:
	if not self.m_args.useAliasEntityID:
		return _stream.readInt32()

	var _id:int = 0
	if self.m_entityIDAliasIDList.size() > 255:
		_id = _stream.readInt32()
	else:
		var aliasID:int = _stream.readUint8()
		# 如果为0且客户端上一步是重登陆或者重连操作并且服务端entity在断线期间一直处于在线状态
		# 则可以忽略这个错误, 因为cellapp可能一直在向baseapp发送同步消息， 当客户端重连上时未等
		# 服务端初始化步骤开始则收到同步信息, 此时这里就会出错。
		if self.m_entityIDAliasIDList.size() <= aliasID:
			return 0
		_id = self.m_entityIDAliasIDList[aliasID]
	return _id

## 服务端使用优化的方式更新实体属性数据
func Client_onUpdatePropertysOptimized(_stream:MemoryStream)-> void:
	var _eid:int = getViewEntityIDFromStream(_stream)
	onUpdatePropertys_(_eid, _stream, -1)

## 服务端更新实体属性数据
func Client_onUpdatePropertys(_stream:MemoryStream)-> void:
	var _messageStart:int = _stream.rpos
	var _eid:int = _stream.readInt32()
	onUpdatePropertys_(_eid, _stream, _messageStart)

func onUpdatePropertys_(_eid:int, _stream:MemoryStream, _messageStart:int)-> void:
	var _entity:Entity = self.entities.get(_eid)
	if not _entity:
		var entityMessage:MemoryStream = self.m_bufferedCreateEntityMessages.get(_eid)
		if entityMessage:
			Dbg.ERROR_MSG("KBEngine::Client_onUpdatePropertys: entity(" + str(_eid) + ") not found!")
			return
		if _messageStart < 0:
			Dbg.ERROR_MSG("KBEngine::Client_onUpdatePropertys: optimized update for entity(" + str(_eid) + ") arrived before entity creation; dropping buffered-incompatible message.")
			return
		var stream1:MemoryStream = ObjectPool.createObject(MemoryStream)
		stream1.wpos = _stream.wpos
		stream1.rpos = _messageStart
		stream1.setBuffer(_stream.data().slice(0, _stream.wpos)) # Array.Copy(_stream.data(), stream1.data(), _stream.wpos)
		self.m_bufferedCreateEntityMessages[_eid] = stream1
		return
	_entity.onUpdatePropertys(_stream)

## 服务端使用优化的方式调用实体方法
func Client_onRemoteMethodCallOptimized(_stream:MemoryStream)-> void:
	var _eid:int = getViewEntityIDFromStream(_stream)
	onRemoteMethodCall_(_eid, _stream)

## 服务端调用实体方法
func Client_onRemoteMethodCall(_stream:MemoryStream)-> void:
	var _eid:int = _stream.readInt32()
	onRemoteMethodCall_(_eid, _stream)

func onRemoteMethodCall_(_eid:int, _stream:MemoryStream)-> void:
	var _entity:Entity = self.entities.get(_eid)
	if not _entity:
		Dbg.ERROR_MSG("KBEngine::Client_onRemoteMethodCall: entity(" + str(_eid) + ") not found!")
		return
	_entity.onRemoteMethodCall(_stream)

func CreateEntityByCls(_cls:Script)-> Entity:
	if KBETools.is_subclass_of(_cls, Entity):
		return _cls.new()
	else:
		Dbg.ERROR_MSG("KBEngine::CreateEntityByCls: not found entityScript in type" + str(_cls))
		return null

## 服务端通知一个实体进入了世界(如果实体是当前玩家则玩家第一次在一个space中创建了， 如果是其他实体则是其他实体进入了玩家的View)
func Client_onEntityEnterWorld(_stream:MemoryStream)-> void:
	var _eid:int = _stream.readInt32()
	if self.entity_id > 0 and self.entity_id != _eid:
		self.m_entityIDAliasIDList.append(_eid)
	var uentityType:int
	if EntityDef.idmoduledefs.size() > 255:
		uentityType = _stream.readUint16()
	else:
		uentityType = _stream.readUint8()
	var isOnGround:int = 1
	if _stream.length() > 0:
		isOnGround = _stream.readInt8()
	var entityModule:ScriptModule = EntityDef.idmoduledefs.get(uentityType, null)
	if not entityModule:
		Dbg.ERROR_MSG("KBEngine::Client_onEntityEnterWorld: unknown entity utype " + str(uentityType))
		return
	var entityType:String = entityModule.name
	# Dbg.DEBUG_MSG("KBEngine::Client_onEntityEnterWorld: " + entityType + "(" + str(_eid) + "), spaceID(" + KBEngineApp.app.spaceID + ")!")
	var _entity:Entity = self.entities.get(_eid)
	if not _entity:
		var entityMessage:MemoryStream = self.m_bufferedCreateEntityMessages.get(_eid)
		if not entityMessage:
			Dbg.ERROR_MSG("KBEngine::Client_onEntityEnterWorld: entity(" + str(_eid) + ") not found!")
			return
		var module:ScriptModule = EntityDef.moduledefs.get(entityType)
		if not module:
			Dbg.ERROR_MSG("KBEngine::Client_onEntityEnterWorld: not found module(" + entityType + ")!")
		var runclass:Script = module.entityScript
		if not runclass:
			Dbg.WARNING_MSG("KBEngine::Client_onEntityEnterWorld: not found entityScript in type" + entityType)
			return
		_entity = CreateEntityByCls(runclass)
		_entity.id = _eid
		_entity.className = entityType
		_entity.onGetCell()
		self.entities[_eid] = _entity
		Client_onUpdatePropertys(entityMessage)
		self.m_bufferedCreateEntityMessages.erase(_eid)
		entityMessage.reclaimObject()
		_entity.isOnGround = isOnGround > 0
		_entity.onDirectionChanged(_entity.direction)
		_entity.onPositionChanged(_entity.position)
		_entity.__init__()
		_entity.attachComponents()
		_entity.inited = true
		_entity.inWorld = true
		_entity.enterWorld()
		if self.m_args.isOnInitCallPropertysSetMethods:
			_entity.callPropertysSetMethods()
	else:
		if not _entity.inWorld:
			# 安全起见， 这里清空一下
			# 如果服务端上使用giveClientTo切换控制权
			# 之前的实体已经进入世界， 切换后的实体也进入世界， 这里可能会残留之前那个实体进入世界的信息
			self.m_entityIDAliasIDList.clear()
			clearEntities(false)
			self.entities[_entity.id] = _entity
			_entity.onGetCell()
			_entity.onDirectionChanged(_entity.direction)
			_entity.onPositionChanged(_entity.position)				
			self.m_entityServerPos = _entity.position
			_entity.isOnGround = isOnGround > 0
			_entity.inWorld = true
			_entity.enterWorld()
			if self.m_args.isOnInitCallPropertysSetMethods:
				_entity.callPropertysSetMethods()

## 服务端使用优化的方式通知一个实体离开了世界(如果实体是当前玩家则玩家离开了space， 如果是其他实体则是其他实体离开了玩家的View)
func Client_onEntityLeaveWorldOptimized(_stream:MemoryStream)-> void:
	var _eid:int = getViewEntityIDFromStream(_stream)
	Client_onEntityLeaveWorld(_eid)

## 服务端通知一个实体离开了世界(如果实体是当前玩家则玩家离开了space， 如果是其他实体则是其他实体离开了玩家的View)
func Client_onEntityLeaveWorld(_eid:int)-> void:
	var _entity:Entity = self.entities.get(_eid)
	if not _entity:
		Dbg.ERROR_MSG("KBEngine::Client_onEntityLeaveWorld: entity(" + str(_eid) + ") not found!")
		return
	if _entity.inWorld:
		_entity.leaveWorld()
	if self.entity_id == _eid:
		clearSpace(false)
		_entity.onLoseCell()
	else:
		if self.m_controlledEntities.has(_entity):
			self.m_controlledEntities.erase(_entity)
			KBEEvent.Event.onLoseControlledEntity.emit(_entity)
		self.entities.erase(_eid)
		_entity.destroy()
		self.m_entityIDAliasIDList.erase(_eid)

## 服务端通知当前玩家进入了一个新的space
func Client_onEntityEnterSpace(_stream:MemoryStream)-> void:
	var _eid:int = _stream.readInt32()
	self.spaceID = _stream.readUint32()
	var isOnGround:int = 1
	if _stream.length() > 0:
		isOnGround = _stream.readInt8()
	
	var _entity:Entity = self.entities.get(_eid)
	if not _entity:
		Dbg.ERROR_MSG("KBEngine::Client_onEntityEnterSpace: entity(" + str(_eid) + ") not found!")
		return
	_entity.isOnGround = isOnGround > 0
	self.m_entityServerPos = _entity.position
	_entity.enterSpace()

## 服务端通知当前玩家离开了space
func Client_onEntityLeaveSpace(_eid:int)-> void:
	var _entity:Entity = self.entities.get(_eid)
	if not _entity:
		Dbg.ERROR_MSG("KBEngine::Client_onEntityLeaveSpace: entity(" + str(_eid) + ") not found!")
		return
	_entity.leaveSpace()
	clearSpace(false)

## 账号创建返回结果
func Client_onCreateAccountResult(_stream:MemoryStream)-> void:
	var retcode:int = _stream.readUint16()
	var datas:PackedByteArray = _stream.readBlob()
	KBEEvent.Event.onCreateAccountResult.emit(retcode, datas)
	if retcode != 0:
		Dbg.WARNING_MSG("KBEngine::Client_onCreateAccountResult: " + username + " create is failed! code=" + str(retcode) + "(" + serverErr(retcode)+ ")!")
		return
	Dbg.DEBUG_MSG("KBEngine::Client_onCreateAccountResult: " + username + " create is successfully!")

## 告诉客户端：你当前负责（或取消）控制谁的位移同步
func Client_onControlEntity(_eid:int, _isControlled:int)-> void:
	var _entity:Entity = self.entities.get(_eid)
	if not _entity:
		Dbg.ERROR_MSG("KBEngine::Client_onControlEntity: entity(" + str(_eid) + ") not found!")
		return
	var isCont:bool = _isControlled != 0
	if isCont:
		# 如果被控制者是玩家自己，那表示玩家自己被其它人控制了
		# 所以玩家自己不应该进入这个被控制列表
		var playerEntity:Entity = player()
		if playerEntity:
			if playerEntity.id != _entity.id:
				self.m_controlledEntities.append(_entity)
		else:
			Dbg.ERROR_MSG("KBEngine::Client_onControlEntity: player entity is null!")
	else:
		self.m_controlledEntities.erase(_entity)
	_entity.isControlled = isCont
	_entity.onControlled(isCont)
	KBEEvent.Event.onControlled.emit(_entity, isCont)

## 更新当前玩家的位置与朝向到服务端， 可以通过开关_syncPlayerMS关闭这个机制
func updatePlayerToServer()-> void:
	if self.m_updatePlayerToServerPeroid <= 0.01 or self.spaceID == 0:  # TODO 为何这里是小于0.01，感觉逻辑有误
		return
	var now:int = Time.get_ticks_msec()
	var span:int = now - self.m_lastUpdateToServerTime
	if span < self.m_updatePlayerToServerPeroid:
		return
	var playerEntity:Entity = player()
	if playerEntity == null or playerEntity.inWorld == false or playerEntity.isControlled:
		return

	self.m_lastUpdateToServerTime = now - (span - self.m_updatePlayerToServerPeroid)
	var position:Vector3 = playerEntity.position
	var direction:Vector3 = playerEntity.direction
	var posHasChanged:bool = playerEntity.entityLastLocalPos.distance_to(position) > 0.001
	var dirHasChanged:bool = playerEntity.entityLastLocalDir.distance_to(direction) > 0.001
	
	if posHasChanged or dirHasChanged:
		playerEntity.entityLastLocalPos = position
		playerEntity.entityLastLocalDir = direction
		var bundle:Bundle = ObjectPool.createObject(Bundle)
		bundle.newMessage(Messages.messages["Baseapp_onUpdateDataFromClient"])
		bundle.writeFloat(position.x)
		bundle.writeFloat(position.y)
		bundle.writeFloat(position.z)
		var x:float = direction.x / 360.0 * (PI * 2)
		var y:float = direction.y / 360.0 * (PI * 2)
		var z:float = direction.z / 360.0 * (PI * 2)
		# 根据弧度转角度公式会出现负数
		# unity会自动转化到0~360度之间，这里需要做一个还原
		# TODO:Godot这边可以考虑使用Vector3.angle_to节省性能
		if x - PI > 0.0:
			x -= PI * 2
		if y - PI > 0.0:
			y -= PI * 2
		if z - PI > 0.0:
			z -= PI * 2
		bundle.writeFloat(x)
		bundle.writeFloat(y)
		bundle.writeFloat(z)
		bundle.writeUint8(1 if playerEntity.isOnGround else 0)
		bundle.writeUint32(self.spaceID)
		bundle.send(self.m_networkInterface)

	# 开始同步所有被控制了的entity的位置
	for i:int in range(self.m_controlledEntities.size()):
		var _entity = self.m_controlledEntities[i]
		position = _entity.position
		direction = _entity.direction
		posHasChanged = _entity.entityLastLocalPos.distance_to(position) > 0.001
		dirHasChanged = _entity.entityLastLocalDir.distance_to(direction) > 0.001
		if posHasChanged or dirHasChanged:
			_entity.entityLastLocalPos = position
			_entity.entityLastLocalDir = direction
			var bundle:Bundle = ObjectPool.createObject(Bundle)
			bundle.newMessage(Messages.messages["Baseapp_onUpdateDataFromClientForControlledEntity"])
			bundle.writeInt32(_entity.id)
			bundle.writeFloat(position.x)
			bundle.writeFloat(position.y)
			bundle.writeFloat(position.z)
			var x:float = direction.x / 360.0 * (PI * 2)
			var y:float = direction.y / 360.0 * (PI * 2)
			var z:float = direction.z / 360.0 * (PI * 2)
			# 根据弧度转角度公式会出现负数
			# unity会自动转化到0~360度之间，这里需要做一个还原
			if x - PI > 0.0:
				x -= PI * 2
			if y - PI > 0.0:
				y -= PI * 2
			if z - PI > 0.0:
				z -= PI * 2
			bundle.writeFloat(x)
			bundle.writeFloat(y)
			bundle.writeFloat(z)
			bundle.writeUint8(1 if _entity.isOnGround else 0)
			bundle.writeUint32(self.spaceID)
			bundle.send(self.m_networkInterface)

## 当前space添加了关于几何等信息的映射资源
## 客户端可以通过这个资源信息来加载对应的场景
func addSpaceGeometryMapping(_uspaceID:int, _respath:String)-> void:
	Dbg.DEBUG_MSG("KBEngine::addSpaceGeometryMapping: spaceID(" + str(_uspaceID) + "), respath(" + _respath + ")!")
	self.isLoadedGeometry = true
	self.spaceID = _uspaceID
	self.spaceResPath = _respath
	KBEEvent.Event.addSpaceGeometryMapping.emit(self.spaceResPath)

func clearSpace(_isall:bool)-> void:
	self.m_entityIDAliasIDList.clear()
	self.m_spacedatas.clear()
	clearEntities(_isall)
	self.isLoadedGeometry = false
	self.spaceID = 0

func clearEntities(_isall:bool)-> void:
	self.m_controlledEntities.clear()
	if not _isall:
		var playerEntity:Entity = player()
		if playerEntity == null:
			for _eid:int in self.entities.keys():
				var _entity:Entity = self.entities[_eid]
				if _entity.inWorld:
					_entity.leaveWorld()
				_entity.destroy()
			self.entities.clear()
			return
		for _eid:int in self.entities.keys(): 
			if _eid == playerEntity.id:
				continue
			var _entity:Entity = self.entities[_eid]
			if _entity.inWorld:
				_entity.leaveWorld()
			_entity.destroy()
		self.entities.clear()
		self.entities[playerEntity.id] = playerEntity
	else:
		for _eid:int in self.entities.keys(): 
			var _entity:Entity = self.entities[_eid]
			if _entity.inWorld:
				_entity.leaveWorld()
			_entity.destroy()
		self.entities.clear()

## 服务端初始化客户端的spacedata， spacedata请参考API
func Client_initSpaceData(_stream:MemoryStream)-> void:
	clearSpace(false)
	self.spaceID = _stream.readUint32()
	while _stream.length() > 0:
		var key:String = _stream.readString()
		var val:String = _stream.readString()
		Client_setSpaceData(self.spaceID, key, val)
	Dbg.DEBUG_MSG("KBEngine::Client_initSpaceData: spaceID({0}), size({1})!".format([self.spaceID, self.m_spacedatas.size()]))

## 服务端设置客户端的spacedata， spacedata请参考API
func Client_setSpaceData(_spaceID:int, _key:String, _value:String)-> void:
	Dbg.DEBUG_MSG("KBEngine::Client_setSpaceData: spaceID(" + str(_spaceID) + "), key(" + _key + "), value(" + _value + ")!")
	self.m_spacedatas[_key] = _value
	if _key == "_mapping":
		addSpaceGeometryMapping(_spaceID, _value)
		KBEEvent.Event.onSetSpaceData.emit(_spaceID, _key, _value)

## 服务端删除客户端的spacedata， spacedata请参考API
func Client_delSpaceData(_spaceID:int, _key:String)-> void:
	Dbg.DEBUG_MSG("KBEngine::Client_delSpaceData: spaceID(" + str(_spaceID) + "), key(" + _key + ")")
	self.m_spacedatas.erase(_key)
	KBEEvent.Event.onDelSpaceData.emit(_spaceID, _key)

func getSpaceData(_key:String)-> String:
	return self.m_spacedatas.get(_key, "")


## 服务端通知强制销毁一个实体
func Client_onEntityDestroyed(_eid:int)-> void:
	Dbg.DEBUG_MSG("KBEngine::Client_onEntityDestroyed: entity(%s)" % _eid)
	var _entity:Entity = self.entities.get(_eid)
	if not _entity:
		Dbg.ERROR_MSG("KBEngine::Client_onEntityDestroyed: entity(%s) not found!" % _eid)
		return
	if _entity.inWorld:
		if self.entity_id == _eid:
			clearSpace(false)
		_entity.leaveWorld()
	if self.m_controlledEntities.has(_entity):
		self.m_controlledEntities.erase(_entity)
		KBEEvent.Event.onLoseControlledEntity.emit(_entity)
	self.entities.erase(_eid)
	_entity.destroy()

## 服务端更新玩家的基础位置， 客户端以这个基础位置加上便宜值计算出玩家周围实体的坐标
func Client_onUpdateBasePos(_x:float, _y:float, _z:float)-> void:
	self.m_entityServerPos = Vector3(_x, _y, _z)
	var playerEntity:Entity = player()
	if playerEntity and playerEntity.isControlled:
		playerEntity.position =self.m_entityServerPos
		KBEEvent.Event.updatePosition.emit(playerEntity)
		playerEntity.onUpdateVolatileData()

func Client_onUpdateBasePosXZ(_x:float, _z:float)-> void:
	self.m_entityServerPos.x = _x
	self.m_entityServerPos.z = _z
	var playerEntity:Entity = player()
	if playerEntity and playerEntity.isControlled:
		playerEntity.position.x = _x
		playerEntity.position.z = _z
		KBEEvent.Event.updatePosition.emit(playerEntity)
		playerEntity.onUpdateVolatileData()

func Client_onUpdateBaseDir(_stream:MemoryStream)-> void:
	var yaw:float = _stream.readFloat() * 360 / (PI * 2)
	var pitch:float = _stream.readFloat() * 360 / (PI * 2)
	var roll:float = _stream.readFloat() * 360 / (PI * 2)
	var playerEntity:Entity = player()
	if playerEntity and playerEntity.isControlled:
		playerEntity.direction = Vector3(roll, pitch, yaw)
		KBEEvent.Event.set_direction.emit(playerEntity)
		playerEntity.onUpdateVolatileData()

func Client_onUpdateData(_stream:MemoryStream)-> void:
	var _eid:int = getViewEntityIDFromStream(_stream)
	var _entity:Entity = self.entities.get(_eid)
	# TODO 这里有点奇怪，为什么更新数据的时候只判断是否存在就return了
	if not _entity:
		Dbg.ERROR_MSG("KBEngine::Client_onUpdateData: entity(" + str(_eid) + ") not found!")
		return

## 服务端强制设置了玩家的坐标 
## 例如：在服务端使用avatar.position=(0,0,0), 或者玩家位置与速度异常时会强制拉回到一个位置
func Client_onSetEntityPosAndDir(_stream:MemoryStream)-> void:
	var _eid:int = _stream.readInt32()
	var _entity:Entity = self.entities.get(_eid)
	if not _entity:
		Dbg.ERROR_MSG("KBEngine::Client_onSetEntityPosAndDir: entity(" + str(_eid) + ") not found!")
		return
	var old_position:Vector3 = _entity.position
	var old_direction:Vector3 = _entity.direction
	_entity.position.x = _stream.readFloat()
	_entity.position.y = _stream.readFloat()
	_entity.position.z = _stream.readFloat()
	_entity.direction.x = _stream.readFloat()
	_entity.direction.y = _stream.readFloat()
	_entity.direction.z = _stream.readFloat()
	_entity.entityLastLocalPos = _entity.position
	_entity.entityLastLocalDir = _entity.direction
	_entity.onDirectionChanged(old_direction)
	_entity.onPositionChanged(old_position)

func Client_onUpdateData_ypr(_stream:MemoryStream)-> void:
	var _eid:int = getViewEntityIDFromStream(_stream)
	var y:float = _stream.readFloat()
	var p:float = _stream.readFloat()
	var r:float = _stream.readFloat()
	_updateVolatileData(_eid, KBE_FLT_MAX, KBE_FLT_MAX, KBE_FLT_MAX, y, p, r, -1, false)

func Client_onUpdateData_yp(_stream:MemoryStream)-> void:
	var _eid:int = getViewEntityIDFromStream(_stream)
	var y:float = _stream.readFloat()
	var p:float = _stream.readFloat()
	_updateVolatileData(_eid, KBE_FLT_MAX, KBE_FLT_MAX, KBE_FLT_MAX, y, p, KBE_FLT_MAX, -1, false)

func Client_onUpdateData_yr(_stream:MemoryStream)-> void:
	var _eid:int = getViewEntityIDFromStream(_stream)
	var y:float = _stream.readFloat()
	var r:float = _stream.readFloat()
	_updateVolatileData(_eid, KBE_FLT_MAX, KBE_FLT_MAX, KBE_FLT_MAX, y, KBE_FLT_MAX, r, -1, false)

func Client_onUpdateData_pr(_stream:MemoryStream)-> void:
	var _eid:int = getViewEntityIDFromStream(_stream)
	var p:float = _stream.readFloat()
	var r:float = _stream.readFloat()
	_updateVolatileData(_eid, KBE_FLT_MAX, KBE_FLT_MAX, KBE_FLT_MAX, KBE_FLT_MAX, p, r, -1, false)

func Client_onUpdateData_y(_stream:MemoryStream)-> void:
	var _eid:int = getViewEntityIDFromStream(_stream)
	var y:float = _stream.readFloat()
	_updateVolatileData(_eid, KBE_FLT_MAX, KBE_FLT_MAX, KBE_FLT_MAX, y, KBE_FLT_MAX, KBE_FLT_MAX, -1, false)

func Client_onUpdateData_p(_stream:MemoryStream)-> void:
	var _eid:int = getViewEntityIDFromStream(_stream)
	var p:float = _stream.readFloat()
	_updateVolatileData(_eid, KBE_FLT_MAX, KBE_FLT_MAX, KBE_FLT_MAX, KBE_FLT_MAX, p, KBE_FLT_MAX, -1, false)

func Client_onUpdateData_r(_stream:MemoryStream)-> void:
	var _eid:int = getViewEntityIDFromStream(_stream)
	var r:float = _stream.readFloat()
	_updateVolatileData(_eid, KBE_FLT_MAX, KBE_FLT_MAX, KBE_FLT_MAX, KBE_FLT_MAX, KBE_FLT_MAX, r, -1, false)

func Client_onUpdateData_xz(_stream:MemoryStream)-> void:
	var _eid:int = getViewEntityIDFromStream(_stream)
	var x:float = _stream.readFloat()
	var z:float = _stream.readFloat()
	_updateVolatileData(_eid, x, KBE_FLT_MAX, z, KBE_FLT_MAX, KBE_FLT_MAX, KBE_FLT_MAX, 1, false)


func Client_onUpdateData_xz_ypr(_stream:MemoryStream)-> void:
	var _eid:int = getViewEntityIDFromStream(_stream)
	var x:float = _stream.readFloat()
	var z:float = _stream.readFloat()
	var y:float = _stream.readFloat()
	var p:float = _stream.readFloat()
	var r:float = _stream.readFloat()
	_updateVolatileData(_eid, x, KBE_FLT_MAX, z, y, p, r, 1, false)

func Client_onUpdateData_xz_yp(_stream:MemoryStream)-> void:
	var _eid:int = getViewEntityIDFromStream(_stream)
	var x:float = _stream.readFloat()
	var z:float = _stream.readFloat()
	var y:float = _stream.readFloat()
	var p:float = _stream.readFloat()
	_updateVolatileData(_eid, x, KBE_FLT_MAX, z, y, p, KBE_FLT_MAX, 1, false)

func Client_onUpdateData_xz_yr(_stream:MemoryStream)-> void:
	var _eid:int = getViewEntityIDFromStream(_stream)
	var x:float = _stream.readFloat()
	var z:float = _stream.readFloat()
	var y:float = _stream.readFloat()
	var r:float = _stream.readFloat()
	_updateVolatileData(_eid, x, KBE_FLT_MAX, z, y, KBE_FLT_MAX, r, 1, false)

func Client_onUpdateData_xz_pr(_stream:MemoryStream)-> void:
	var _eid:int = getViewEntityIDFromStream(_stream)
	var x:float = _stream.readFloat()
	var z:float = _stream.readFloat()
	var p:float = _stream.readFloat()
	var r:float = _stream.readFloat()
	_updateVolatileData(_eid, x, KBE_FLT_MAX, z, KBE_FLT_MAX, p, r, 1, false)

func Client_onUpdateData_xz_y(_stream:MemoryStream)-> void:
	var _eid:int = getViewEntityIDFromStream(_stream)
	var x:float = _stream.readFloat()
	var z:float = _stream.readFloat()
	var yaw:float = _stream.readFloat()
	_updateVolatileData(_eid, x, KBE_FLT_MAX, z, yaw, KBE_FLT_MAX, KBE_FLT_MAX, 1, false)

func Client_onUpdateData_xz_p(_stream:MemoryStream)-> void:
	var _eid:int = getViewEntityIDFromStream(_stream)
	var x:float = _stream.readFloat()
	var z:float = _stream.readFloat()
	var p:float = _stream.readFloat()
	_updateVolatileData(_eid, x, KBE_FLT_MAX, z, KBE_FLT_MAX, p, KBE_FLT_MAX, 1, false)

func Client_onUpdateData_xz_r(_stream:MemoryStream)-> void:
	var _eid:int = getViewEntityIDFromStream(_stream)
	var x:float = _stream.readFloat()
	var z:float = _stream.readFloat()
	var r:float = _stream.readFloat()
	_updateVolatileData(_eid, x, KBE_FLT_MAX, z, KBE_FLT_MAX, KBE_FLT_MAX, r, 1, false)

func Client_onUpdateData_xyz(_stream:MemoryStream)-> void:
	var _eid:int = getViewEntityIDFromStream(_stream)
	var x:float = _stream.readFloat()
	var y:float = _stream.readFloat()
	var z:float = _stream.readFloat()
	_updateVolatileData(_eid, x, y, z, KBE_FLT_MAX, KBE_FLT_MAX, KBE_FLT_MAX, 0, false)

func Client_onUpdateData_xyz_ypr(_stream:MemoryStream)-> void:
	var _eid:int = getViewEntityIDFromStream(_stream)
	var x:float = _stream.readFloat()
	var y:float = _stream.readFloat()
	var z:float = _stream.readFloat()
	var yaw:float = _stream.readFloat()
	var p:float = _stream.readFloat()
	var r:float = _stream.readFloat()
	_updateVolatileData(_eid, x, y, z, yaw, p, r, 0, false)

func Client_onUpdateData_xyz_yp(_stream:MemoryStream)-> void:
	var _eid:int = getViewEntityIDFromStream(_stream)
	var x:float = _stream.readFloat()
	var y:float = _stream.readFloat()
	var z:float = _stream.readFloat()
	var yaw:float = _stream.readFloat()
	var p:float = _stream.readFloat()
	_updateVolatileData(_eid, x, y, z, yaw, p, KBE_FLT_MAX, 0, false)

func Client_onUpdateData_xyz_yr(_stream:MemoryStream)-> void:
	var _eid:int = getViewEntityIDFromStream(_stream)
	var x:float = _stream.readFloat()
	var y:float = _stream.readFloat()
	var z:float = _stream.readFloat()
	var yaw:float = _stream.readFloat()
	var r:float = _stream.readFloat()
	_updateVolatileData(_eid, x, y, z, yaw, KBE_FLT_MAX, r, 0, false)

func Client_onUpdateData_xyz_pr(_stream:MemoryStream)-> void:
	var _eid:int = getViewEntityIDFromStream(_stream)
	var x:float = _stream.readFloat()
	var y:float = _stream.readFloat()
	var z:float = _stream.readFloat()
	var p:float = _stream.readFloat()
	var r:float = _stream.readFloat()
	_updateVolatileData(_eid, x, y, z, KBE_FLT_MAX, p, r, 0, false)

func Client_onUpdateData_xyz_y(_stream:MemoryStream)-> void:
	var _eid:int = getViewEntityIDFromStream(_stream)
	var x:float = _stream.readFloat()
	var y:float = _stream.readFloat()
	var z:float = _stream.readFloat()
	var yaw:float = _stream.readFloat()
	_updateVolatileData(_eid, x, y, z, yaw, KBE_FLT_MAX, KBE_FLT_MAX, 0, false)

func Client_onUpdateData_xyz_p(_stream:MemoryStream)-> void:
	var _eid:int = getViewEntityIDFromStream(_stream)
	var x:float = _stream.readFloat()
	var y:float = _stream.readFloat()
	var z:float = _stream.readFloat()
	var p:float = _stream.readFloat()
	_updateVolatileData(_eid, x, y, z, KBE_FLT_MAX, p, KBE_FLT_MAX, 0, false)

func Client_onUpdateData_xyz_r(_stream:MemoryStream)-> void:
	var _eid:int = getViewEntityIDFromStream(_stream)
	var x:float = _stream.readFloat()
	var y:float = _stream.readFloat()
	var z:float = _stream.readFloat()
	var r:float = _stream.readFloat()
	_updateVolatileData(_eid, x, y, z, KBE_FLT_MAX, KBE_FLT_MAX, r, 0, false)

func Client_onUpdateData_ypr_optimized(_stream:MemoryStream)-> void:
	var _eid:int = getViewEntityIDFromStream(_stream)
	var y:int = _stream.readInt8()
	var p:int = _stream.readInt8()
	var r:int = _stream.readInt8()
	_updateVolatileData(_eid, KBE_FLT_MAX, KBE_FLT_MAX, KBE_FLT_MAX, y, p, r, -1, true)

func Client_onUpdateData_yp_optimized(_stream:MemoryStream)-> void:
	var _eid:int = getViewEntityIDFromStream(_stream)
	var y:int = _stream.readInt8()
	var p:int = _stream.readInt8()
	_updateVolatileData(_eid, KBE_FLT_MAX, KBE_FLT_MAX, KBE_FLT_MAX, y, p, KBE_FLT_MAX, -1, true)

func Client_onUpdateData_yr_optimized(_stream:MemoryStream)-> void:
	var _eid:int = getViewEntityIDFromStream(_stream)
	var y:int = _stream.readInt8()
	var r:int = _stream.readInt8()
	_updateVolatileData(_eid, KBE_FLT_MAX, KBE_FLT_MAX, KBE_FLT_MAX, y, KBE_FLT_MAX, r, -1, true)

func Client_onUpdateData_pr_optimized(_stream:MemoryStream)-> void:
	var _eid:int = getViewEntityIDFromStream(_stream)
	var p:int = _stream.readInt8()
	var r:int = _stream.readInt8()
	_updateVolatileData(_eid, KBE_FLT_MAX, KBE_FLT_MAX, KBE_FLT_MAX, KBE_FLT_MAX, p, r, -1, true)

func Client_onUpdateData_y_optimized(_stream:MemoryStream)-> void:
	var _eid:int = getViewEntityIDFromStream(_stream)
	var y:int = _stream.readInt8()
	_updateVolatileData(_eid, KBE_FLT_MAX, KBE_FLT_MAX, KBE_FLT_MAX, y, KBE_FLT_MAX, KBE_FLT_MAX, -1, true)

func Client_onUpdateData_p_optimized(_stream:MemoryStream)-> void:
	var _eid:int = getViewEntityIDFromStream(_stream)
	var p:int = _stream.readInt8()
	_updateVolatileData(_eid, KBE_FLT_MAX, KBE_FLT_MAX, KBE_FLT_MAX, KBE_FLT_MAX, p, KBE_FLT_MAX, -1, true)

func Client_onUpdateData_r_optimized(_stream:MemoryStream)-> void:
	var _eid:int = getViewEntityIDFromStream(_stream)
	var r:int = _stream.readInt8()
	_updateVolatileData(_eid, KBE_FLT_MAX, KBE_FLT_MAX, KBE_FLT_MAX, KBE_FLT_MAX, KBE_FLT_MAX, r, -1, true)

func Client_onUpdateData_xz_optimized(_stream:MemoryStream)-> void:
	var _eid:int = getViewEntityIDFromStream(_stream)
	var xz:Vector2  = _stream.readPackXZ()
	_updateVolatileData(_eid, xz.x, KBE_FLT_MAX, xz.y, KBE_FLT_MAX, KBE_FLT_MAX, KBE_FLT_MAX, 1, true)

func Client_onUpdateData_xz_ypr_optimized(_stream:MemoryStream)-> void:
	var _eid:int = getViewEntityIDFromStream(_stream)
	var xz:Vector2  = _stream.readPackXZ()
	var y:int = _stream.readInt8()
	var p:int = _stream.readInt8()
	var r:int = _stream.readInt8()
	_updateVolatileData(_eid, xz.x, KBE_FLT_MAX, xz.y, y, p, r, 1, true)

func Client_onUpdateData_xz_yp_optimized(_stream:MemoryStream)-> void:
	var _eid:int = getViewEntityIDFromStream(_stream)
	var xz:Vector2  = _stream.readPackXZ()
	var y:int = _stream.readInt8()
	var p:int = _stream.readInt8()
	_updateVolatileData(_eid, xz.x, KBE_FLT_MAX, xz.y, y, p, KBE_FLT_MAX, 1, true)

func Client_onUpdateData_xz_yr_optimized(_stream:MemoryStream)-> void:
	var _eid:int = getViewEntityIDFromStream(_stream)
	var xz:Vector2  = _stream.readPackXZ()
	var y:int = _stream.readInt8()
	var r:int = _stream.readInt8()
	_updateVolatileData(_eid, xz.x, KBE_FLT_MAX, xz.y, y, KBE_FLT_MAX, r, 1, true)

func Client_onUpdateData_xz_pr_optimized(_stream:MemoryStream)-> void:
	var _eid:int = getViewEntityIDFromStream(_stream)
	var xz:Vector2  = _stream.readPackXZ()
	var p:int = _stream.readInt8()
	var r:int = _stream.readInt8()
	_updateVolatileData(_eid, xz.x, KBE_FLT_MAX, xz.y, KBE_FLT_MAX, p, r, 1, true)

func Client_onUpdateData_xz_y_optimized(_stream:MemoryStream)-> void:
	var _eid:int = getViewEntityIDFromStream(_stream)
	var xz:Vector2  = _stream.readPackXZ()
	var yaw:int = _stream.readInt8()
	_updateVolatileData(_eid, xz.x, KBE_FLT_MAX, xz.y, yaw, KBE_FLT_MAX, KBE_FLT_MAX, 1, true)

func Client_onUpdateData_xz_p_optimized(_stream:MemoryStream)-> void:
	var _eid:int = getViewEntityIDFromStream(_stream)
	var xz:Vector2  = _stream.readPackXZ()
	var p:int = _stream.readInt8()
	_updateVolatileData(_eid, xz.x, KBE_FLT_MAX, xz.y, KBE_FLT_MAX, p, KBE_FLT_MAX, 1, true)

func Client_onUpdateData_xz_r_optimized(_stream:MemoryStream)-> void:
	var _eid:int = getViewEntityIDFromStream(_stream)
	var xz:Vector2  = _stream.readPackXZ()
	var r:int = _stream.readInt8()
	_updateVolatileData(_eid, xz.x, KBE_FLT_MAX, xz.y, KBE_FLT_MAX, KBE_FLT_MAX, r, 1, true)

func Client_onUpdateData_xyz_optimized(_stream:MemoryStream)-> void:
	var _eid:int = getViewEntityIDFromStream(_stream)
	var xz:Vector2  = _stream.readPackXZ()
	var y:float = _stream.readPackY()
	_updateVolatileData(_eid, xz.x, y, xz.y, KBE_FLT_MAX, KBE_FLT_MAX, KBE_FLT_MAX, 0, true)

func Client_onUpdateData_xyz_ypr_optimized(_stream:MemoryStream)-> void:
	var _eid:int = getViewEntityIDFromStream(_stream)
	var xz:Vector2  = _stream.readPackXZ()
	var y:float = _stream.readPackY()
	var yaw:int = _stream.readInt8()
	var p:int = _stream.readInt8()
	var r:int = _stream.readInt8()
	_updateVolatileData(_eid, xz.x, y, xz.y, yaw, p, r, 0, true)

func Client_onUpdateData_xyz_yp_optimized(_stream:MemoryStream)-> void:
	var _eid:int = getViewEntityIDFromStream(_stream)
	var xz:Vector2  = _stream.readPackXZ()
	var y:float = _stream.readPackY()
	var yaw:int = _stream.readInt8()
	var p:int = _stream.readInt8()
	_updateVolatileData(_eid, xz.x, y, xz.y, yaw, p, KBE_FLT_MAX, 0, true)

func Client_onUpdateData_xyz_yr_optimized(_stream:MemoryStream)-> void:
	var _eid:int = getViewEntityIDFromStream(_stream)
	var xz:Vector2  = _stream.readPackXZ()
	var y:float = _stream.readPackY()
	var yaw:int = _stream.readInt8()
	var r:int = _stream.readInt8()
	_updateVolatileData(_eid, xz.x, y, xz.y, yaw, KBE_FLT_MAX, r, 0, true)

func Client_onUpdateData_xyz_pr_optimized(_stream:MemoryStream)-> void:
	var _eid:int = getViewEntityIDFromStream(_stream)
	var xz:Vector2  = _stream.readPackXZ()
	var y:float = _stream.readPackY()
	var p:int = _stream.readInt8()
	var r:int = _stream.readInt8()
	_updateVolatileData(_eid, xz.x, y, xz.y, KBE_FLT_MAX, p, r, 0, true)

func Client_onUpdateData_xyz_y_optimized(_stream:MemoryStream)-> void:
	var _eid:int = getViewEntityIDFromStream(_stream)
	var xz:Vector2  = _stream.readPackXZ()
	var y:float = _stream.readPackY()
	var yaw:int = _stream.readInt8()
	_updateVolatileData(_eid, xz.x, y, xz.y, yaw, KBE_FLT_MAX, KBE_FLT_MAX, 0, true)

func Client_onUpdateData_xyz_p_optimized(_stream:MemoryStream)-> void:
	var _eid:int = getViewEntityIDFromStream(_stream)
	var xz:Vector2  = _stream.readPackXZ()
	var y:float = _stream.readPackY()
	var p:int = _stream.readInt8()
	_updateVolatileData(_eid, xz.x, y, xz.y, KBE_FLT_MAX, p, KBE_FLT_MAX, 0, true)

func Client_onUpdateData_xyz_r_optimized(_stream:MemoryStream)-> void:
	var _eid:int = getViewEntityIDFromStream(_stream)
	var xz:Vector2  = _stream.readPackXZ()
	var y:float = _stream.readPackY()
	var r:int = _stream.readInt8()
	_updateVolatileData(_eid, xz.x, y, xz.y, KBE_FLT_MAX, KBE_FLT_MAX, r, 0, true)

func _updateVolatileData(_entityID, _x:float, _y:float, _z:float, _yaw:float, _pitch:float, _roll:float, _isOnGround:int, _isOptimized:bool)-> void:
	var _entity:Entity = self.entities.get(_entityID)
	if not _entity:
		# 如果为0且客户端上一步是重登陆或者重连操作并且服务端entity在断线期间一直处于在线状态
		# 则可以忽略这个错误, 因为cellapp可能一直在向baseapp发送同步消息， 当客户端重连上时未等
		# 服务端初始化步骤开始则收到同步信息, 此时这里就会出错。
		Dbg.ERROR_MSG("KBEngine::_updateVolatileData: entity(" + str(_entityID) + ") not found!")
		return
	# 小于0不设置
	if _isOnGround >= 0:
		_entity.isOnGround = (_isOnGround > 0)
	var changeDirection:bool = false
	
	if is_finite(_roll):  # 判定是否为有限值，即不是 @GDScript.NAN、正无穷大或负无穷大。另见 is_inf() 和 is_nan()。
		changeDirection = true
		_entity.direction.x = (KBETools.int82angle(_roll, false) if _isOptimized else _roll) * 360 / (PI * 2)
	if is_finite(_pitch):
		changeDirection = true
		_entity.direction.y = (KBETools.int82angle(_pitch, false) if _isOptimized else _pitch) * 360 / (PI * 2)
	if is_finite(_yaw):
		changeDirection = true
		_entity.direction.z = (KBETools.int82angle(_yaw, false) if _isOptimized else _yaw) * 360 / (PI * 2)
	var done:bool = false
	if changeDirection:
		KBEEvent.Event.set_direction.emit(_entity)
		done = true
	
	var positionChanged:bool = is_finite(_x) or is_finite(_y) or is_finite(_z)
	if not is_finite(_x):
		_x = 0.0 if _isOptimized else _entity.position.x
	if not is_finite(_y):
		_y = 0.0 if _isOptimized else _entity.position.y
	if not is_finite(_z):
		_z = 0.0 if _isOptimized else _entity.position.z
	if positionChanged:
		var old_position:Vector3 = _entity.position
		var pos:Vector3 = Vector3(_x + self.m_entityServerPos.x, _y + self.m_entityServerPos.y, _z + self.m_entityServerPos.z) if _isOptimized else Vector3(_x, _y, _z)
			
		_entity.position = pos
		done = true
		_entity.onSmoothPositionChanged(old_position)
	if done:
		_entity.onUpdateVolatileData()

## 服务端通知流数据下载开始
## 请参考API手册关于onStreamDataStarted
func Client_onStreamDataStarted(_id:int, _datasize:int, _descr:String)-> void:
	KBEEvent.Event.onStreamDataStarted.emit(_id, _datasize, _descr)

func Client_onStreamDataRecv(_stream:MemoryStream)-> void:
	var resID:int = _stream.readInt16()
	var _datas:PackedByteArray = _stream.readBlob()
	KBEEvent.Event.onStreamDataRecv.emit(resID, _datas)

func Client_onStreamDataCompleted(_id:int)-> void:
	KBEEvent.Event.onStreamDataCompleted.emit(_id)
