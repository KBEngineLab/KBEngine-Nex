class_name NetworkInterfaceKCP extends NetworkInterfaceBase
## KCP 可靠 UDP 网络接口
## 参考 C# NetworkInterfaceKCP.cs，使用 KCPProtocol.gd 实现

var kcp:KCPProtocol = null
var connID:int = 0
var nextTickKcpUpdate:int = 0
var _helloState:int = 0  # 0=未连接, 1=等待hello回包
var _helloSendTime:int = 0

func valid()-> bool:
	return self.m_socket and self.m_socket.isConnected() and kcp != null

func createSocket()-> NetSocket:
	return NetSocketUDP.new()

func reset()-> void:
	_helloState = 0
	finiKCP()
	super.reset()

func close()-> void:
	_helloState = 0
	finiKCP()
	super.close()

func initKCP()-> bool:
	kcp = KCPProtocol.new(connID, self)
	kcp.set_output(_outputKCP)
	kcp.set_mtu(1400)
	if KBEngine.app:
		var args:KBEngineArgs = KBEngine.app.getInitArgs()
		kcp.wnd_size(args.getUDPSendBufferSize(), args.getUDPRecvBufferSize())
	else:
		kcp.wnd_size(128, 128)
	kcp.no_delay(1, 10, 2, 1)
	kcp.set_minrto(10)
	nextTickKcpUpdate = 0
	Dbg.DEBUG_MSG("NetworkInterfaceKCP::initKCP: KCP initialized, conv=" + str(connID))
	return true

func finiKCP()-> bool:
	if kcp != null:
		kcp.set_output(Callable())
		kcp.release()
		kcp = null
	connID = 0
	nextTickKcpUpdate = 0
	return true

func _outputKCP(_data:PackedByteArray, _size:int, _user:Object)-> void:
	if not valid():
		return
	if _size <= 0:
		return
	self.m_socket.sendto(_data, _size)

func send(_stream:MemoryStream)-> bool:
	if not valid():
		Dbg.ERROR_MSG("NetworkInterfaceKCP::send: invalid socket!")
		return false
	
	if m_filter:
		m_filter.encrypt(_stream)
	
	nextTickKcpUpdate = 0
	var _data:PackedByteArray = _stream.getbuffer()
	return kcp.send(_data, 0, _data.size()) >= 0

func process()-> void:
	if not self.m_socket:
		return
	
	self.m_socket.process()
	
	# UDP socket 已连接但 KCP 还未初始化 → 发送 hello 握手
	if self.m_socket.isConnected() and kcp == null:
		if _helloState == 0:
			_sendUDPHello()
		elif _helloState == 1:
			_checkUDPHelloAck()
		return
	
	if not valid():
		return
	
	var current:int = KCPProtocol.TimeUtils.iclock()
	if current >= nextTickKcpUpdate:
		kcp.update(current)
		nextTickKcpUpdate = kcp.check(current)
	
	_recvFromSocket()

func _sendUDPHello()-> void:
	var _data:PackedByteArray = UDP_HELLO.to_ascii_buffer()
	self.m_socket.sendto(_data, _data.size())
	_helloSendTime = Time.get_ticks_msec()
	_helloState = 1
	Dbg.DEBUG_MSG("NetworkInterfaceKCP::_sendUDPHello: sent hello to " + str(self.connectIP) + ":" + str(self.connectPort))

func _checkUDPHelloAck()-> void:
	if not self.m_socket or not self.m_socket.socket:
		return
	
	var udp_sock:PacketPeerUDP = self.m_socket.socket
	while udp_sock.get_available_packet_count() > 0:
		var _buffer:PackedByteArray = udp_sock.get_packet()
		if _buffer.size() < 16:
			continue
		
		var _stream:MemoryStream = ObjectPool.createObject(MemoryStream)
		_stream.buffer = _buffer
		_stream.wpos = _buffer.size()
		var helloAck:String = _stream.readString()
		var versionString:String = _stream.readString()
		var _conv:int = _stream.readUint32()
		
		if helloAck != UDP_HELLO_ACK:
			Dbg.ERROR_MSG("NetworkInterfaceKCP::_checkUDPHelloAck: hello-ack mismatch! got='" + helloAck + "' expected='" + UDP_HELLO_ACK + "'")
			continue
		
		connID = _conv
		_helloState = 2
		
		Dbg.DEBUG_MSG("NetworkInterfaceKCP::_checkUDPHelloAck: received ack, conv=" + str(connID) + " version=" + versionString)
		
		if initKCP():
			self.connected = true
			KBEEvent.Event.onConnectionState.emit(true)
			if self.connectCB and self.connectCB.is_valid():
				self.connectCB.call(self.connectIP, self.connectPort, true, self.userData)
		return
	
	# 超时检查 (3秒)
	if Time.get_ticks_msec() - _helloSendTime > 3000:
		Dbg.ERROR_MSG("NetworkInterfaceKCP::_checkUDPHelloAck: hello timeout!")
		_helloState = 0
		KBEEvent.Event.onConnectionState.emit(false)
		if self.connectCB and self.connectCB.is_valid():
			self.connectCB.call(self.connectIP, self.connectPort, false, self.userData)

func _recvFromSocket()-> void:
	if not self.m_socket or not self.m_socket.isConnected():
		return
	var udp_sock:PacketPeerUDP = self.m_socket.socket
	if udp_sock == null:
		return
	while udp_sock.get_available_packet_count() > 0:
		var _buffer:PackedByteArray = udp_sock.get_packet()
		if _buffer.size() > 0:
			# 喂给 KCP 协议层
			kcp.input(_buffer, 0, _buffer.size())
			# 从 KCP 提取可用数据
			_readFromKCP()

func _readFromKCP()-> void:
	var recv_buf:PackedByteArray = PackedByteArray()
	recv_buf.resize(KBEngine.PACKET_MAX_SIZE_UDP * 4)
	while true:
		var _len:int = kcp.recv(recv_buf, 0, recv_buf.size())
		if _len <= 0:
			break
		if self.m_socket and self.m_socket.m_messageReader:
			if m_filter:
				m_filter.recv(self.m_socket.m_messageReader, recv_buf, 0, _len)
			else:
				self.m_socket.m_messageReader.process(recv_buf, 0, _len)
