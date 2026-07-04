extends Node
class_name KBEMain

var gameapp:KBEngine


@export var debugLevel:Dbg.DEBUGLEVEL = Dbg.DEBUGLEVEL.DEBUG
@export var ip:String = "127.0.0.1"
@export var port:int = @{KBE_LOGIN_PORT}
@export var clientType:KBEngine.CLIENT_TYPE = KBEngine.CLIENT_TYPE.CLIENT_TYPE_MINI
@export var networkType:KBEngine.NETWORK_TYPE = KBEngine.NETWORK_TYPE.TCP
@export var enableWSS:bool = false
@export var networkEncryptType:KBEngine.NETWORK_ENCRYPT_TYPE = KBEngine.NETWORK_ENCRYPT_TYPE.ENCRYPT_TYPE_BLOWFISH
@export var syncPlayerMS:int = 1000 / @{KBE_UPDATEHZ}
@export var threadUpdateHZ:int = @{KBE_UPDATEHZ}

@export var serverHeartbeatTick:int = @{KBE_SERVER_EXTERNAL_TIMEOUT}
@export var TCP_SEND_BUFFER_MAX:int = KBEngine.PACKET_MAX_SIZE_TCP
@export var TCP_RECV_BUFFER_MAX:int = KBEngine.PACKET_MAX_SIZE_TCP
@export var UDP_SEND_BUFFER_MAX:int = KBEngine.PACKET_MAX_SIZE_UDP
@export var UDP_RECV_BUFFER_MAX:int = KBEngine.PACKET_MAX_SIZE_UDP
@export var useAliasEntityID:bool = @{KBE_USE_ALIAS_ENTITYID}
@export var isOnInitCallPropertysSetMethods:bool = true

func _ready()-> void:
	Dbg.INFO_MSG("clientapp::start()")
	initKBEngine()

func initKBEngine()-> void:
	Dbg.debugLevel = debugLevel
	var args:KBEngineArgs = KBEngineArgs.new()
	args.ip = ip
	args.port = port
	args.clientType = clientType
	args.networkType = networkType
	args.enableWSS = enableWSS
	args.networkEncryptType = networkEncryptType
	args.syncPlayerMS = syncPlayerMS
	args.threadUpdateHZ = threadUpdateHZ
	args.serverHeartbeatTick = serverHeartbeatTick
	args.TCP_SEND_BUFFER_MAX = TCP_SEND_BUFFER_MAX
	args.TCP_RECV_BUFFER_MAX = TCP_RECV_BUFFER_MAX
	args.UDP_SEND_BUFFER_MAX = UDP_SEND_BUFFER_MAX
	args.UDP_RECV_BUFFER_MAX = UDP_RECV_BUFFER_MAX
	args.useAliasEntityID = useAliasEntityID
	args.isOnInitCallPropertysSetMethods = isOnInitCallPropertysSetMethods
	gameapp = KBEngine.new(args)

func OnDestroy()-> void:
	Dbg.INFO_MSG("clientapp::OnDestroy(): begin")
	if gameapp:
		gameapp.destroy()
		gameapp = null
	KBEEvent.clearFiredEvents()
	Dbg.INFO_MSG("clientapp::OnDestroy(): end")

func _physics_process(_delta:float)-> void:
	KBEUpdate()

func KBEUpdate()-> void:
	if gameapp:
		gameapp.process()

func _exit_tree()-> void:
	KBEEvent.Event.logout.emit()
	
