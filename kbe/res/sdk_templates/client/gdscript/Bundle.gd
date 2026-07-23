class_name Bundle extends RecyclableObject

var stream:MemoryStream
var streamList:Array[MemoryStream]
var numMessage:int
var messageLength:int
var msgtype:Messages.Message
var curMsgStreamIndex:int

func _init() -> void:
	clear()

func clear()-> void:
	for _i:int in range(self.streamList.size()):
		if self.stream != self.streamList[_i]:
			self.streamList[_i].reclaimObject()
	self.streamList.clear()
	if self.stream:
		self.stream.clear()
	else:
		self.stream = ObjectPool.createObject(MemoryStream)
	self.numMessage = 0
	self.messageLength = 0
	self.msgtype = null
	self.curMsgStreamIndex = 0

func newMessage(_msgtype:Messages.Message)-> void:
	self.fini(false)
	self.msgtype = _msgtype
	self.numMessage += 1
	# Dbg.DEBUG_MSG("新创建一个Message" + _msgtype.name + ":: ID:" +str(_msgtype.id))
	self.writeUint16(_msgtype.id)
	if self.msgtype.msglen == -1:
		self.writeUint16(0)
		self.messageLength = 0
	self.curMsgStreamIndex = 0

func writeMsgLength()-> void:
	if self.msgtype.msglen != -1:
		return
	var _stream:MemoryStream = self.stream
	if self.curMsgStreamIndex > 0:
		_stream = self.streamList[self.streamList.size() - self.curMsgStreamIndex]
	# Dbg.DEBUG_MSG("self.messageLength::" + str(self.messageLength) + "::" + str(_stream.buffer))
	_stream.buffer.encode_u16(2, self.messageLength)

func fini(_issend:bool)-> void:
	if self.numMessage > 0:
		self.writeMsgLength()
		self.streamList.append(self.stream)
		self.stream = ObjectPool.createObject(MemoryStream)
	if _issend:
		self.numMessage = 0
		self.msgtype = null
	self.curMsgStreamIndex = 0

func send(_network:NetworkInterfaceBase)-> void:
	# Dbg.DEBUG_MSG("发送消息:::" + self.msgtype.name + ":::" + str(self.streamList))
	self.fini(true)
	if _network.valid():
		for i:int in range(self.streamList.size()):
			var tempStream:MemoryStream = self.streamList[i]
			_network.send(tempStream)
	else:
		Dbg.ERROR_MSG("Bundle::send: networkInterface invalid!")  
	self.reclaimObject()

func checkStream(_v:int)-> void:
	if _v > self.stream.space():
		self.streamList.append(self.stream)
		self.stream = ObjectPool.createObject(MemoryStream)
		self.curMsgStreamIndex += 1
	self.messageLength += _v

#---------------------------------------------------------------------------------

func writeInt8(_v:int)-> void:
	self.checkStream(1)
	self.stream.writeInt8(_v)

func writeInt16(_v:int)-> void:
	self.checkStream(2)
	self.stream.writeInt16(_v)
	
func writeInt32(_v:int)-> void:
	self.checkStream(4)
	self.stream.writeInt32(_v)

func writeInt64(_v:int)-> void:
	self.checkStream(8)
	self.stream.writeInt64(_v)

func writeUint8(_v:int)-> void:
	self.checkStream(1)
	self.stream.writeUint8(_v)

func writeUint16(_v:int)-> void:
	self.checkStream(2)
	self.stream.writeUint16(_v)
	
func writeUint32(_v:int)-> void:
	self.checkStream(4)
	self.stream.writeUint32(_v)

func writeUint64(_v:int)-> void:
	self.checkStream(8)
	self.stream.writeUint64(_v)

func writeFloat(_v:float)-> void:
	self.checkStream(4)
	self.stream.writeFloat(_v)

func writeDouble(_v:float)-> void:
	self.checkStream(8)
	self.stream.writeDouble(_v)

func writeString(_v:String)-> void:
	self.checkStream(_v.to_utf8_buffer().size() + 1)
	self.stream.writeString(_v)

func writeUnicode(_v:String)-> void:
	self.writeBlob(_v.to_utf8_buffer())

func writeBlob(_v:PackedByteArray)-> void:
	self.checkStream(_v.size() + 4)
	self.stream.writeBlob(_v)

func writePython(_v:PackedByteArray)-> void:
	self.writeBlob(_v)

func writeVector2(_v:Vector2)-> void:
	self.checkStream(8)
	self.stream.writeVector2(_v)

func writeVector3(_v:Vector3)-> void:
	self.checkStream(12)
	self.stream.writeVector3(_v)

func writeVector4(_v:Vector4)-> void:
	self.checkStream(16)
	self.stream.writeVector4(_v)

func writeEntitycall(_v:PackedByteArray)-> void:
	self.checkStream(16)
	var cid:int = 0
	var id:int = 0
	var type:int = 0
	var utype:int = 0
	self.stream.writeUint64(cid)
	self.stream.writeInt32(id)
	self.stream.writeUint16(type)
	self.stream.writeUint16(utype)
