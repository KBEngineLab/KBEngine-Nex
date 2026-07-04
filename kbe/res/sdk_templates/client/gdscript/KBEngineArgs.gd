class_name KBEngineArgs

# 登录ip和端口
var ip:String = "127.0.0.1"
var port:int = @{KBE_LOGIN_PORT}

# 客户端类型
# Reference: http://www.kbengine.org/docs/programming/clientsdkprogramming.html, client types
var clientType:KBEngine.CLIENT_TYPE = KBEngine.CLIENT_TYPE.CLIENT_TYPE_MINI

# 加密通信类型
var networkEncryptType:KBEngine.NETWORK_ENCRYPT_TYPE = KBEngine.NETWORK_ENCRYPT_TYPE.ENCRYPT_TYPE_NONE

# 网络类型
var networkType:KBEngine.NETWORK_TYPE = KBEngine.NETWORK_TYPE.TCP

# 是否启用WSS（wss:// vs ws://），仅WebSocket时有效
var enableWSS:bool = false

# 域名映射表，主要为wss提供支持
var domainMapping:Dictionary = {}

# 端口映射表，主要为wss提供支持
var portMapping:Dictionary = {}

# Allow synchronization role position information to the server
# 是否开启自动同步玩家信息到服务端，信息包括位置与方向，毫秒
# 非高实时类游戏不需要开放这个选项
var syncPlayerMS:int = 1000 / @{KBE_UPDATEHZ}

# 是否使用别名机制
# 这个参数的选择必须与kbengine_defs.xml::cellapp/aliasEntityID的参数保持一致
var useAliasEntityID:bool = @{KBE_USE_ALIAS_ENTITYID}

# 在Entity初始化时是否触发属性的set_*事件(callPropertysSetMethods)
var isOnInitCallPropertysSetMethods:bool = true

# 是否严格校验协议MD5。C# SDK默认忽略该检查，只强制校验entitydef。
var strictProtocolMD5:bool = false

# 发送缓冲大小
var TCP_SEND_BUFFER_MAX:int = KBEngine.PACKET_MAX_SIZE_TCP
var UDP_SEND_BUFFER_MAX:int = 128

# 接收缓冲区大小
var TCP_RECV_BUFFER_MAX:int = KBEngine.PACKET_MAX_SIZE_TCP
var UDP_RECV_BUFFER_MAX:int = 128

# 是否多线程启动(TODO: 尚未实现多线程)
# var isMultiThreads:bool = false
# 只在多线程模式启用
# 线程主循环处理频率
var threadUpdateHZ:int = @{KBE_UPDATEHZ}
# 心跳频率（tick数）
var serverHeartbeatTick:int = @{KBE_SERVER_EXTERNAL_TIMEOUT}

func getTCPRecvBufferSize()-> int:
	return TCP_RECV_BUFFER_MAX

func getTCPSendBufferSize()-> int:
	return TCP_SEND_BUFFER_MAX

func getUDPRecvBufferSize()-> int:
	return UDP_RECV_BUFFER_MAX

func getUDPSendBufferSize()-> int:
	return UDP_SEND_BUFFER_MAX
