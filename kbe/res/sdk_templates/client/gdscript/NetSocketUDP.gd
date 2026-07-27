class_name NetSocketUDP extends NetSocket
## Godot UDP 网络套接字封装
## Godot UDP socket wrapper.
## 使用 Godot 内置的 PacketPeerUDP
## Uses Godot's built-in PacketPeerUDP.

var socket:PacketPeerUDP
var m_messageReader:MessageReaderTCP
var _host:String = ""
var _port:int = 0

func _init()-> void:
	self.socket = PacketPeerUDP.new()
	# KCP 重组后的应用层消息采用与 TCP 相同的帧格式。
	# Application messages reassembled by KCP use the same framing as TCP.
	self.m_messageReader = MessageReaderTCP.new()
	self.isClose = false

func connectHost(_addr:String, _port:int)-> void:
	_host = _addr
	_port = _port
	# 先绑定临时本地端口，再固定远端 peer 以拒绝其他来源的数据报。
	# Bind an ephemeral local port, then pin the remote peer to reject datagrams from other sources.
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

# KCP output 回调发送原始 UDP 数据报，并把 Godot 错误返回给调用方。
# The KCP output callback sends a raw UDP datagram and returns Godot's error status to the caller.
func sendto(_data:PackedByteArray, _size:int)-> bool:
	return self.socket.put_packet(_data.slice(0, _size)) == OK

func isConnected()-> bool:
	return self.socket.is_socket_connected()

func process()-> void:
	# UDP 数据由 NetworkInterfaceKCP 读取，经 KCP 重组后再进入消息解析器。
	# NetworkInterfaceKCP reads UDP data and forwards KCP-reassembled messages to the message reader.
	pass
