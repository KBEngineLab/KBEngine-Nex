class_name NetworkInterfaceKCP extends NetworkInterfaceBase
## KCP 可靠 UDP 网络接口
## KCP reliable UDP network interface.
## 参考 C# NetworkInterfaceKCP.cs，使用 KCPProtocol.gd 实现
## Uses KCPProtocol.gd and follows the C# NetworkInterfaceKCP behavior.

const HANDSHAKE_TIMEOUT_MSEC:int = 30000
const HELLO_RETRY_MSEC:int = 1000

var kcp:KCPProtocol = null
var connID:int = 0
var nextTickKcpUpdate:int = 0
## 0 表示尚未开始，1 表示等待 ACK，2 表示握手完成。
## 0 means not started, 1 means waiting for ACK, and 2 means complete.
var _helloState:int = 0
var _helloStartTime:int = 0
var _helloSendTime:int = 0

func valid()-> bool:
	return self.m_socket and self.m_socket.isConnected() and kcp != null

func createSocket()-> NetSocket:
	return NetSocketUDP.new()

func reset()-> void:
	_helloState = 0
	_helloStartTime = 0
	_helloSendTime = 0
	finiKCP()
	super.reset()

func close()-> void:
	_helloState = 0
	_helloStartTime = 0
	_helloSendTime = 0
	finiKCP()
	super.close()

func initKCP()-> bool:
	kcp = KCPProtocol.new(connID, self)
	if kcp == null:
		return false
	kcp.set_output(_outputKCP)
	if kcp.set_mtu(1400) < 0:
		finiKCP()
		return false
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
	if not self.m_socket.sendto(_data, _size):
		Dbg.ERROR_MSG("NetworkInterfaceKCP::_outputKCP: failed to send UDP datagram")

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
	
	# UDP socket 已连接但 KCP 尚未初始化时，先完成 hello/ACK 协商。
	# Complete the hello/ACK negotiation after UDP connects and before KCP initialization.
	if self.m_socket.isConnected() and kcp == null:
		if _helloState == 0:
			_beginUDPHandshake()
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

func _beginUDPHandshake()-> void:
	_helloState = 1
	_helloStartTime = Time.get_ticks_msec()
	_helloSendTime = 0
	_sendUDPHello()

func _sendUDPHello()-> void:
	var _data:PackedByteArray = UDP_HELLO.to_ascii_buffer()
	if not self.m_socket.sendto(_data, _data.size()):
		_failUDPHandshake("failed to send KCP hello")
		return
	_helloSendTime = Time.get_ticks_msec()
	Dbg.DEBUG_MSG("NetworkInterfaceKCP::_sendUDPHello: sent hello to " + str(self.connectIP) + ":" + str(self.connectPort))

func _checkUDPHelloAck()-> void:
	if not self.m_socket or not self.m_socket.socket:
		return
	
	var udp_sock:PacketPeerUDP = self.m_socket.socket
	while udp_sock.get_available_packet_count() > 0:
		var _buffer:PackedByteArray = udp_sock.get_packet()
		var parsed:Dictionary = _parseHelloAck(_buffer)
		if not parsed.get("valid", false):
			_failUDPHandshake(str(parsed.get("error", "malformed KCP hello acknowledgement")))
			return

		var version_string:String = str(parsed["version"])
		if not KBEngine.app or version_string != KBEngine.app.serverVersion:
			var expected_version:String = KBEngine.app.serverVersion if KBEngine.app else ""
			_failUDPHandshake("version mismatch (%s!=%s)" % [version_string, expected_version])
			return

		connID = int(parsed["conv"])
		_helloState = 2

		Dbg.DEBUG_MSG("NetworkInterfaceKCP::_checkUDPHelloAck: received ack, conv=" + str(connID) + " version=" + version_string)
		
		if initKCP():
			self.connected = true
			KBEEvent.Event.onConnectionState.emit(true)
			if self.connectCB and self.connectCB.is_valid():
				self.connectCB.call(self.connectIP, self.connectPort, true, self.userData)
		else:
			_failUDPHandshake("failed to initialize KCP")
		return

	var now:int = Time.get_ticks_msec()
	if now - _helloStartTime >= HANDSHAKE_TIMEOUT_MSEC:
		_failUDPHandshake("KCP handshake timeout")
	elif now - _helloSendTime >= HELLO_RETRY_MSEC:
		# UDP 可能丢失首个 hello，每秒重发可恢复瞬时丢包而不延长总超时。
		# UDP may lose the first hello; retrying every second recovers transient loss without extending the total timeout.
		_sendUDPHello()

func _failUDPHandshake(_reason:String)-> void:
	Dbg.ERROR_MSG("NetworkInterfaceKCP::_failUDPHandshake: " + _reason)
	reset()
	KBEEvent.Event.onConnectionState.emit(false)
	if self.connectCB and self.connectCB.is_valid():
		self.connectCB.call(self.connectIP, self.connectPort, false, self.userData)

static func _parseHelloAck(_buffer:PackedByteArray)-> Dictionary:
	var expected_ack:PackedByteArray = UDP_HELLO_ACK.to_ascii_buffer()
	var minimum_length:int = expected_ack.size() + 1 + 1 + 4
	if _buffer.size() < minimum_length:
		return {"valid": false, "error": "malformed KCP hello acknowledgement length"}

	for index:int in range(expected_ack.size()):
		if _buffer[index] != expected_ack[index]:
			return {"valid": false, "error": "KCP hello acknowledgement mismatch"}

	if _buffer[expected_ack.size()] != 0:
		return {"valid": false, "error": "KCP hello acknowledgement is not NUL terminated"}

	var version_begin:int = expected_ack.size() + 1
	var version_end:int = version_begin
	while version_end < _buffer.size() and _buffer[version_end] != 0:
		version_end += 1

	if version_end == version_begin or version_end >= _buffer.size() or _buffer.size() - version_end - 1 != 4:
		return {"valid": false, "error": "malformed KCP version or conv field"}

	var version_string:String = _buffer.slice(version_begin, version_end).get_string_from_ascii()
	var conv_offset:int = version_end + 1
	# 握手协议固定使用小端 uint32，逐字节解码以避免依赖运行平台的字节序。
	# The handshake uses a little-endian uint32; byte-wise decoding avoids depending on runtime platform endianness.
	var conv:int = (_buffer[conv_offset] |
		(_buffer[conv_offset + 1] << 8) |
		(_buffer[conv_offset + 2] << 16) |
		(_buffer[conv_offset + 3] << 24)) & 0xFFFFFFFF
	if conv == 0:
		return {"valid": false, "error": "KCP conv is zero"}

	return {"valid": true, "version": version_string, "conv": conv}

func _recvFromSocket()-> void:
	if not self.m_socket or not self.m_socket.isConnected():
		return
	var udp_sock:PacketPeerUDP = self.m_socket.socket
	if udp_sock == null:
		return
	while udp_sock.get_available_packet_count() > 0:
		var _buffer:PackedByteArray = udp_sock.get_packet()
		if _buffer.size() > 0:
			# 无效数据报不能继续进入消息分发，否则会把损坏输入误认为应用层消息。
			# Invalid datagrams must not reach message dispatch where corrupted input could be treated as application data.
			if kcp.input(_buffer, 0, _buffer.size()) < 0:
				Dbg.ERROR_MSG("NetworkInterfaceKCP::_recvFromSocket: rejected malformed KCP datagram")
				continue
			# 从 KCP 提取已经完成重组的消息。
			# Extract messages that KCP has fully reassembled.
			_readFromKCP()

func _readFromKCP()-> void:
	while true:
		var message_size:int = kcp.peek_size()
		if message_size < 0:
			break
		# KCP 会重组跨 UDP 数据报的完整消息，接收缓冲区必须按 peek_size 动态分配。
		# KCP reassembles complete messages across UDP datagrams, so allocate the receive buffer from peek_size.
		var recv_buf:PackedByteArray = PackedByteArray()
		recv_buf.resize(message_size)
		var _len:int = kcp.recv(recv_buf, 0, recv_buf.size())
		if _len < 0:
			Dbg.ERROR_MSG("NetworkInterfaceKCP::_readFromKCP: KCP recv failed with " + str(_len))
			break
		if self.m_socket and self.m_socket.m_messageReader:
			if m_filter:
				m_filter.recv(self.m_socket.m_messageReader, recv_buf, 0, _len)
			else:
				self.m_socket.m_messageReader.process(recv_buf, 0, _len)
