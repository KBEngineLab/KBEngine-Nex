class_name NetSocketWS extends NetSocket
## Godot WebSocket 网络套接字封装
## 使用 Godot 内置的 WebSocketPeer

var socket:WebSocketPeer
var m_messageReader:MessageReaderWS
var _url:String = ""

func _init()-> void:
	self.socket = WebSocketPeer.new()
	self.m_messageReader = MessageReaderWS.new()
	self.isClose = false

func connectHost(_addr:String, _port:int)-> void:
	_url = _addr  # 此时 _addr 已经是完整的 ws://host:port 或 wss://host:port 格式
	self.error = self.socket.connect_to_url(_url)

func close(_code:int=1000, _reason:String="")-> void:
	if self.isClose:
		return
	self.isClose = true
	self.socket.close(_code, _reason)

func send(_stream:MemoryStream)-> bool:
	var dataLength:int = _stream.length()
	if dataLength <= 0:
		return true
	var _buffer:PackedByteArray = _stream.getbuffer()
	var _succ:int = self.socket.put_packet(_buffer)
	return _succ == OK

func isConnected()-> bool:
	var state:int = self.socket.get_ready_state()
	return state == WebSocketPeer.STATE_OPEN

func process()-> void:
	self.socket.poll()
	var state:int = self.socket.get_ready_state()
	
	if state == WebSocketPeer.STATE_OPEN:
		if self.state != WebSocketPeer.STATE_OPEN:
			Dbg.DEBUG_MSG("NetSocketWS: WebSocket connected!")
			if self.onopen and self.onopen.is_valid():
				self.onopen.call()
		# 接收数据
		while self.socket.get_available_packet_count() > 0:
			var _buffer:PackedByteArray = self.socket.get_packet()
			var packet_error:Error = self.socket.get_packet_error()
			if packet_error != Error.OK:
				_rejectProtocolFrame("WebSocket packet error " + str(packet_error))
				break
			if self.socket.was_string_packet():
				_rejectProtocolFrame("expected binary WebSocket message")
				break

			var _filter:EncryptionFilter = self.networkInterface.fileter() if self.networkInterface else null
			var receive_ok:bool = true
			if _filter:
				receive_ok = _filter.recv(self.m_messageReader, _buffer, 0, _buffer.size())
			else:
				self.m_messageReader.process(_buffer, 0, _buffer.size())

			if not receive_ok:
				_rejectProtocolFrame("incomplete or invalid encrypted WebSocket message")
				break
			var frame_error:String = self.m_messageReader.finishFrame()
			if not frame_error.is_empty():
				_rejectProtocolFrame(frame_error)
				break
	elif state == WebSocketPeer.STATE_CONNECTING:
		pass  # 继续等待
	elif state == WebSocketPeer.STATE_CLOSED:
		if not self.isClose:
			Dbg.ERROR_MSG("NetSocketWS: WebSocket 已关闭")
			if self.onerror and self.onerror.is_valid():
				self.onerror.call()
	self.state = state

func _rejectProtocolFrame(_reason:String)-> void:
	if self.isClose:
		return

	# 在回调业务关闭逻辑前标记旧 socket，保证同一畸形 message 只产生一次断线通知。
	# Mark the old socket before invoking application close logic so one malformed message produces only one disconnect notification.
	self.isClose = true
	self.error = Error.ERR_INVALID_DATA
	Dbg.ERROR_MSG("NetSocketWS: rejected malformed WebSocket message: " + _reason)
	self.socket.close(1002, "Malformed KBEngine frame")
	if self.onerror and self.onerror.is_valid():
		self.onerror.call()
