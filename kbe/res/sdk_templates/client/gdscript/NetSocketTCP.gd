class_name NetSocketTCP extends NetSocket

var socket:StreamPeerTCP

var m_messageReader:MessageReaderTCP
# socket向缓冲区写的起始位置
var m_wpos:int
# 主线程读取数据的起始位置
var m_rpos:int

func _init()-> void:
	self.state = StreamPeerTCP.STATUS_NONE
	self.socket = StreamPeerTCP.new()
	self.m_messageReader = MessageReaderTCP.new()
	self.socket.big_endian = true
	self.isClose = false

func connectHost(_addr:String, _port:int)-> void:
	self.error = self.socket.connect_to_host(_addr, _port)
	#self.socket.set_no_delay(true)

func close(_code:int=1000, _reason:String="")-> void:
	self.socket.disconnect_from_host()
	self.isClose = true

func send(_stream:MemoryStream)-> bool:
	var dataLength:int = _stream.length()
	if dataLength <= 0:
		return true
	elif dataLength <= KBEngine.PACKET_MAX_SIZE_TCP:
		var _buffer:PackedByteArray = _stream.getbuffer()
		var _rst:Array = self.socket.put_partial_data(_buffer)
		var _succ:Error = _rst[0]
		# print_rich("[color=olive]send::error[%d]::length[%d][/color]::" % [_succ, _stream.length()], _buffer)
		return _succ==Error.OK
	else:
		# var _buffer:PackedByteArray = _stream.getbuffer()
		# var _rst:Array = self.socket.put_partial_data(_buffer)
		# var _succ:Error = _rst[0]
		# print("是否成功发送::", _succ, "::实际长度::", _rst[1] ,"::", _buffer)
		Dbg.ERROR_MSG("NetSocketTCP::发送超出最大值, length:" + str(dataLength))
		return false

func recv()-> void:
	var _buffer:PackedByteArray
	while not self.isClose and self.socket.get_available_bytes():
		var _rst:Array = self.socket.get_partial_data(KBEngine.PACKET_MAX_SIZE_TCP * 4)
		var _succ:bool = _rst[0] == Error.OK
		_buffer = _rst[1]
		# print_rich("[color=teal]recv::error[%d]::length[%d][/color]::" % [_rst[0], _buffer.size()], _buffer)
		if not _succ:
			Dbg.ERROR_MSG("NetSocketTCP::异常的数据包:" + str(_buffer))
			if self.onerror and self.onerror.is_valid():
				self.onerror.call()
			break
		var _filter:EncryptionFilter = self.networkInterface.fileter() if self.networkInterface else null
		if _filter:
			_filter.recv(self.m_messageReader, _buffer, 0, _buffer.size())
		else:
			self.m_messageReader.process(_buffer, 0, _buffer.size())
		# if self.m_rpos < self.m_wpos:
		# 	self.m_messageReader.process(self.m_buffer, self.m_rpos, self.m_wpos - self.m_rpos)
		# elif self.m_wpos < self.m_rpos:
		# 	self.m_messageReader.process(self.m_buffer, self.m_rpos, self.m_buffer.size() - self.m_rpos)
		# 	self.m_messageReader.process(self.m_buffer, 0, self.m_wpos)
		# else:
		# 	pass # 没有可读数据

func isConnected()-> bool:
	return self.socket.get_status() == StreamPeerTCP.STATUS_CONNECTED

func process()-> void:
	self.error = self.socket.poll()
	var _state = self.socket.get_status()
	if _state == StreamPeerTCP.STATUS_CONNECTED:
		if self.state !=  StreamPeerTCP.STATUS_CONNECTED:
			if self.onopen and self.onopen.is_valid():
				self.onopen.call()
		self.recv()
	elif _state == StreamPeerTCP.STATUS_CONNECTING:
		# 继续轮询才能正确关闭。
		pass
	elif _state == StreamPeerTCP.STATUS_ERROR:
		Dbg.ERROR_MSG("NetSocketTCP: 连接已关闭, Error：" + str(self.error))
		if self.onerror and self.onerror.is_valid():
			self.onerror.call()
	else:
		# 未链接状态，也是断开连接后的状态
		pass
	self.state = _state
