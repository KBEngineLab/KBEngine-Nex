class_name NetworkInterfaceWS extends NetworkInterfaceBase
## Godot WebSocket 网络接口
## 参考 C# NetworkInterfaceUnityWS.cs 实现，使用 Godot 原生 WebSocketPeer

func valid()-> bool:
	return self.m_socket and self.m_socket.isConnected()

func createSocket()-> NetSocket:
	return NetSocketWS.new()

func connectTo(_ip:String, _port:int, _callback:Callable, _userData:Object)-> void:
	if valid():
		Dbg.ERROR_MSG("Have already connected!")
		return
	
	# 构建 ws:// 或 wss:// URL
	var url:String = ""
	if KBEngine.app and KBEngine.app.getInitArgs().enableWSS:
		url = "wss://"
	else:
		url = "ws://"
	
	# 域名映射和端口映射
	var _ip_mapped:String = _ip
	var _port_mapped:int = _port
	if KBEngine.app:
		var _args:KBEngineArgs = KBEngine.app.getInitArgs()
		if _args.domainMapping.has(_ip):
			_ip_mapped = _args.domainMapping[_ip]
		if _args.portMapping.has(_port):
			_port_mapped = _args.portMapping[_port]
	
	url += _ip_mapped + ":" + str(_port_mapped)
	
	self.m_socket = createSocket()
	self.m_socket.networkInterface = self
	self.connectIP = url
	self.connectPort = _port_mapped
	self.userData = _userData
	self.connectCB = _callback
	
	self.m_socket.onopen = self.onConnectionState
	self.m_socket.onerror = self.close
	
	Dbg.DEBUG_MSG("NetworkInterfaceWS::connectTo: connecting to " + url + " ...")
	self.connected = false
	asyncConnect(url, _port_mapped)

func asyncConnect(_addr:String, _port:int)-> void:
	Dbg.DEBUG_MSG("NetworkInterfaceWS::asyncConnect(), will connect to %s ..." % _addr)
	self.m_socket.connectHost(_addr, _port)

func reset()-> void:
	self.m_filter = null
	self.connected = false
	if self.m_socket != null:
		self.m_socket.close(1000, "reset")
		self.m_socket = null

func close()-> void:
	if self.m_socket:
		self.m_socket.close(1000, "close")
		self.m_socket = null
		KBEEvent.Event.onDisconnected.emit()
	self.m_socket = null
	self.connected = false
