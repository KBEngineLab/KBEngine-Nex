
class_name BlowfishFilter extends EncryptionFilter
const BLOCK_SIZE:int = 8
const MIN_PACKET_SIZE:int = 11  # sizeof(UInt16) + 1 + BLOCK_SIZE

var m_blowfish:Blowfish
var m_packet:MemoryStream
var m_enctyptStrem:MemoryStream
var m_padSize:int
var m_packLen:int

func _init()-> void:
	self.m_blowfish = Blowfish.new()
	self.m_packet = ObjectPool.createObject(MemoryStream)
	self.m_enctyptStrem = ObjectPool.createObject(MemoryStream)

func key()-> PackedByteArray:
	return self.m_blowfish.key()

func encrypt(_stream:MemoryStream)-> void:
	var padSize:int
	if _stream.length() % BLOCK_SIZE != 0:
		padSize = int(BLOCK_SIZE - (_stream.length() % BLOCK_SIZE))
		_stream.wpos += padSize
		if _stream.buffer.size() < _stream.wpos:
			_stream.buffer.resize(_stream.wpos)
		if _stream.wpos > KBEngine.PACKET_MAX_SIZE_TCP * 4:
			Dbg.ERROR_MSG("BlowfishFilter::encrypt: stream.wpos {0} > PACKET_MAX_SIZE_TCP {1}".format([_stream.wpos, KBEngine.PACKET_MAX_SIZE_TCP * 4]))

	self.m_blowfish.encipher(_stream.data(), _stream.length())
	var packLen:int = _stream.length() + 1
	self.m_enctyptStrem.writeUint16(packLen)
	self.m_enctyptStrem.writeUint8(padSize)
	self.m_enctyptStrem.append(_stream.data(), _stream.rpos, _stream.length())
	_stream.swap(self.m_enctyptStrem)
	self.m_enctyptStrem.clear()

func decrypt(_stream:MemoryStream)-> void:
	self.m_blowfish.decipher(_stream.data(), _stream.rpos, _stream.length())

func decryptData(_buffer:PackedByteArray, _startIndex:int, _length:int)-> void:
	self.m_blowfish.decipher(_buffer, _startIndex, _length)


func send(_socket:NetSocket, _stream:MemoryStream)-> bool:
	if not self.m_blowfish.isGood():
		Dbg.ERROR_MSG("BlowfishFilter::send: Dropping packet, due to invalid filter")
		return false
	encrypt(_stream)
	return _socket.send(_stream)

## 取出 2 个字节，组成一个 16 位无符号整数
func littleEndian(_buffer:PackedByteArray, _rpos:int)-> int:
	return _buffer.decode_u16(_rpos)  # 只要 2 字节

func recv(_reader:MessageReaderBase, _buffer:PackedByteArray, _rpos:int, _len:int)-> bool:
	if not self.m_blowfish.isGood():
		Dbg.ERROR_MSG("BlowfishFilter::recv: Dropping packet, due to invalid filter")
		return false

	if self.m_packet.length() == 0 and _len >= MIN_PACKET_SIZE \
			and _buffer.decode_u16(_rpos) - 1 == _len - 3:
	
		var packLen:int = _buffer.decode_u16(_rpos) - 1
		var padSize:int = _buffer[_rpos + 2]
		decryptData(_buffer, _rpos + 3, _len - 3)

		var length:int = packLen - padSize
		if _reader:
			_reader.process(_buffer, _rpos + 3, length)
		return true

	self.m_packet.append(_buffer, _rpos, _len)
	while self.m_packet.length() > 0:
		var currLen:int = 0
		var oldwpos:int = 0
		if self.m_packLen <= 0:
			# 如果满足一个最小包则尝试解包, 否则缓存这个包待与下一个包合并然后解包
			if self.m_packet.length() >= MIN_PACKET_SIZE:
				self.m_packLen = self.m_packet.readUint16()
				self.m_padSize = self.m_packet.readUint8()
				self.m_packLen -= 1
				if self.m_packet.length() > self.m_packLen:
					currLen = self.m_packet.rpos + self.m_packLen
					oldwpos = self.m_packet.wpos
					self.m_packet.wpos = currLen
				elif self.m_packet.length() < self.m_packLen:
					return false
			else:
				return false
		else:
			# 如果上一次有做过解包行为但包还没有完整则继续处理
			# 如果包是完整的下面流程会解密， 如果有多余的内容需要将其剪裁出来待与下一个包合并
			if self.m_packet.length() > self.m_packLen:
				currLen = self.m_packet.rpos + self.m_packLen
				oldwpos = self.m_packet.wpos
				self.m_packet.wpos = currLen
			elif self.m_packet.length() < self.m_packLen:
				return false
		decrypt(self.m_packet)
		self.m_packet.wpos -= self.m_padSize

		# 上面的流程能保证wpos之后不会有多余的包
		# 如果有多余的包数据会放在_recvStream
		if _reader:
			_reader.process(self.m_packet.data(), self.m_packet.rpos, self.m_packet.length())

		if currLen > 0:
			self.m_packet.rpos = currLen
			self.m_packet.wpos = oldwpos
		else:
			self.m_packet.clear()
		
		self.m_packLen = 0
		self.m_padSize = 0
	return true
