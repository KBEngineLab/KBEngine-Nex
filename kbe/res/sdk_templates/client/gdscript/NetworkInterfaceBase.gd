class_name NetworkInterfaceBase

const UDP_HELLO:String = "62a559f3fa7748bc22f8e0766019d498"
const UDP_HELLO_ACK:String = "1432ad7c829170a76dd31982c3501eca"

var m_socket:NetSocket
var m_filter:EncryptionFilter
var connected:bool = false

var connectIP:String
var connectPort:int
var connectCB:Callable
var userData:Object

func _init()-> void:
	reset()

func OnDestroy()-> void:
	Dbg.DEBUG_MSG("NetworkInterfaceBase::~NetworkInterfaceBase(), destructed!!!")
	reset()

func reset()-> void:
	self.m_filter = null
	self.connected = false
	if self.m_socket != null:
		self.m_socket.close(0)
		self.m_socket = null

func close()-> void:
	if self.m_socket:
		self.m_socket.close(0)
		self.m_socket = null
		KBEEvent.Event.onDisconnected.emit()
	self.m_socket = null
	self.connected = false

func valid()-> bool:
	return self.m_socket and self.m_socket.isConnected()

func connectTo(_ip:String, _port:int, _callback:Callable, _userData:Object)-> void:
	if valid():
		Dbg.ERROR_MSG("Have already connected!")
		return
	if not KBETools.IsIpAddress(_ip):
		Dbg.ERROR_MSG("IpAddress is wrong!")
		return
	self.m_socket = createSocket()
	self.m_socket.networkInterface = self
	self.connectIP = _ip
	self.connectPort = _port
	self.userData = _userData
	self.connectCB = _callback
	
	self.m_socket.onopen = self.onConnectionState
	self.m_socket.onerror = self.close

	Dbg.DEBUG_MSG("connect to " + _ip + ":" + str(_port) + " ...")
	self.connected = false
	asyncConnect(_ip, _port)

func asyncConnect(_addr:String, _port:int)-> void:
	Dbg.DEBUG_MSG("NetworkInterfaceBase::asyncConnect(), will connect to %s:%s ..." % [_addr, _port])
	self.m_socket.connectHost(_addr, _port)

func send(_stream:MemoryStream)-> bool:
	if not valid():
		Dbg.ERROR_MSG("invalid NetSocket!")
		return false
	if self.m_filter:
		return self.m_filter.send(self.m_socket, _stream)
	else:
		return self.m_socket.send(_stream)

func process()-> void:
	if not self.m_socket:
		return
	self.m_socket.process()

func onConnectionState()-> void:
	var success:bool = (self.m_socket.error == Error.OK) and valid()
	if success:
		self.connected = true
	else:
		reset()
		Dbg.ERROR_MSG("NetworkInterfaceBase:_onConnectionState(), connect error! ip: %s:%d, err: %s" % [self.connectIP, str(self.connectPort), str(self.m_socket.error)])
	KBEEvent.Event.onConnectionState.emit(success)

	if self.connectCB and self.connectCB.is_valid():
		self.connectCB.call(self.connectIP, self.connectPort, success, self.userData)

func fileter()-> EncryptionFilter:
	return self.m_filter

func setFilter(_f:EncryptionFilter)-> void:
	self.m_filter = _f

func createSocket()-> NetSocket:
	return null
