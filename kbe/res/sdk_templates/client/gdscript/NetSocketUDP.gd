class_name NetSocketUDP extends NetSocket
## Godot UDP 网络套接字封装
## 使用 Godot 内置的 PacketPeerUDP

var socket:PacketPeerUDP
var m_messageReader:MessageReaderTCP
var _host:String = ""
var _port:int = 0

func _init()-> void:
	self.socket = PacketPeerUDP.new()
	self.m_messageReader = MessageReaderTCP.new()  # KCP 消息格式与 TCP 相同
	self.isClose = false

func connectHost(_addr:String, _port:int)-> void:
	_host = _addr
	_port = _port
	# UDP 是无连接协议，直接绑定本地端口即可
	self.error = self.socket.bind(0)
	if self.error == OK:
		self.error = self.socket.connect_to_host(_addr, _port)

func close(_code:int=1000, _reason:String="")-> void:
	self.socket.close()
	self.isClose = true

func send(_stream:MemoryStream)-> bool:
	var dataLength:int = _stream.length()
	if dataLength <= 0:
		return true
	elif dataLength <= KBEngine.PACKET_MAX_SIZE_UDP * 4:
		var _buffer:PackedByteArray = _stream.getbuffer()
		var _succ:int = self.socket.put_packet(_buffer)
		return _succ == OK
	else:
		Dbg.ERROR_MSG("NetSocketUDP::发送超出最大值, length:" + str(dataLength))
		return false

# UDP raw send-to (used by KCP output callback)
func sendto(_data:PackedByteArray, _size:int)-> void:
	self.socket.put_packet(_data.slice(0, _size))

func isConnected()-> bool:
	return self.socket.is_socket_connected()

func process()-> void:
	# UDP 数据读取由 NetworkInterfaceKCP.process() → _recvFromSocket() 处理
	# 通过 KCP 协议层解码后再调用 m_messageReader.process()
	pass
