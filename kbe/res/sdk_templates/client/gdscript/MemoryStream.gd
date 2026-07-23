class_name MemoryStream extends RecyclableObject
## 二进制数据流模块
## 能够将一些基本类型序列化(writeXXX)成二进制流
## 同时也提供了反序列化(readXXX)等操作

class PackFloatXType:
	var fv:float
	var uv:int
	var iv:int

var buffer:PackedByteArray
var rpos:int
var wpos:int

func setBuffer(_buffer:PackedByteArray)-> PackedByteArray:
	var outBuf:PackedByteArray = self.buffer
	self.buffer = _buffer
	return outBuf

func swap(_stream:MemoryStream)-> void:
	var t_rpos:int = self.rpos
	var t_wpos:int = self.wpos
	self.rpos = _stream.rpos
	self.wpos = _stream.wpos
	_stream.rpos = t_rpos
	_stream.wpos = t_wpos
	self.buffer = _stream.setBuffer(self.buffer)

func data()-> PackedByteArray:
	return self.buffer

# TODO 这里有问题，用对象池创建
func _init(_size_or_buffer=KBEngine.PACKET_MAX_SIZE_TCP):
	self.rpos = 0
	self.wpos = 0
	if _size_or_buffer is PackedByteArray:
		self.buffer = _size_or_buffer
		self.wpos = _size_or_buffer.size()
	else:
		self.buffer = self.creatBuffer(_size_or_buffer)

#region 读写基本类型

func readInt8()-> int:
	var _bufVal:int = self.buffer.decode_s8(self.rpos)
	self.rpos += 1
	return _bufVal

func readInt16()-> int:
	var _bufVal:int = self.buffer.decode_s16(self.rpos)
	self.rpos += 2
	return _bufVal
	
func readInt32()-> int:
	var _bufVal:int = self.buffer.decode_s32(self.rpos)
	self.rpos += 4
	return _bufVal

func readInt64()-> int:
	var _bufVal:int = self.buffer.decode_s64(self.rpos)
	self.rpos += 8
	return _bufVal
	
func readUint8()-> int:
	var _bufVal:int = self.buffer.decode_u8(self.rpos)
	self.rpos += 1
	return _bufVal

func readUint16()-> int:
	var _bufVal:int = self.buffer.decode_u16(self.rpos)
	self.rpos += 2
	return _bufVal
	
func readUint32()-> int:
	var _bufVal:int = self.buffer.decode_u32(self.rpos)
	self.rpos += 4
	return _bufVal

func readUint64()-> int:
	var _bufVal:int = self.buffer.decode_u64(self.rpos)
	self.rpos += 8
	return _bufVal

func readFloat()-> float:
	var _bufVal:float = self.buffer.decode_float(self.rpos)
	self.rpos += 4
	return _bufVal

func readDouble()-> float:
	var _bufVal:float = self.buffer.decode_double(self.rpos)
	self.rpos += 8
	return _bufVal

func readString()-> String:
	var _buf:PackedByteArray = self.buffer.slice(self.rpos)
	var _idx:int = _buf.find(0)
	if _idx < 0:
		_idx = _buf.size()
		self.rpos += _idx
		return _buf.get_string_from_utf8()
	self.rpos += _idx + 1
	return _buf.slice(0, _idx).get_string_from_utf8()

func readUnicode()-> String:
	return readBlob().get_string_from_utf8()

func readBlob()-> PackedByteArray:
	var _size:int = self.readUint32()
	var _buf:PackedByteArray = self.buffer.slice(self.rpos, self.rpos + _size)
	self.rpos += _size
	return _buf

func readStream():
	var _buf:PackedByteArray = self.buffer.slice(self.rpos, self.wpos)
	self.rpos = self.wpos
	return MemoryStream.new(_buf)

func readEntitycall()-> PackedByteArray:
	readUint64()
	readInt32()
	readUint16()
	readUint16()
	return PackedByteArray()

func readVector2()-> Vector2:
	var x:float = readFloat()
	var y:float = readFloat()
	return Vector2(x, y)

func readVector3()-> Vector3:
	var x:float = readFloat()
	var y:float = readFloat()
	var z:float = readFloat()
	return Vector3(x, y, z)

func readVector4()-> Vector4:
	var x:float = readFloat()
	var y:float = readFloat()
	var z:float = readFloat()
	var w:float = readFloat()
	return Vector4(x, y, z, w)

func readPython()-> PackedByteArray:
	return readBlob()

func readPackXZ()-> Vector2:
	var _v1:int = self.readUint8()
	var _v2:int = self.readUint8()
	var _v3:int = self.readUint8()
	var _data:int = 0
	
	_data |= (_v1 << 16)
	_data |= (_v2 << 8)
	_data |= _v3

	var _x_bits:int = 0x40000000
	var _z_bits:int = 0x40000000
	_x_bits |= (_data & 0x7ff000) << 3
	_z_bits |= (_data & 0x0007ff) << 15
	var _x:float = _uint32_to_float(_x_bits) - 2.0
	var _z:float = _uint32_to_float(_z_bits) - 2.0
	if (_data & 0x800000) != 0:
		_x = -_x
	if (_data & 0x000800) != 0:
		_z = -_z
	return Vector2(_x, _z)

func readPackY()-> float:
	var _v:int = self.readUint16()
	var _y_bits:int = 0x40000000
	_y_bits |= (_v & 0x7fff) << 12
	var _y:float = _uint32_to_float(_y_bits) - 2.0
	if (_v & 0x8000) != 0:
		_y = -_y
	return _y

static func _uint32_to_float(_bits:int)-> float:
	var _bytes:PackedByteArray = PackedByteArray()
	_bytes.resize(4)
	_bytes.encode_u32(0, _bits)
	return _bytes.decode_float(0)

#---------------------------------------------------------------------------------
# TODO 可以优化性能
func writeInt8(_v:int)-> void:
	if self.space() <= 0:
		self.buffer.append(_v)
	else:
		self.buffer.encode_s8(self.wpos, _v)
	self.wpos += 1

func writeInt16(_v:int)-> void:
	self.writeInt8(_v & 0xff)
	self.writeInt8(_v >> 8 & 0xff)

func writeInt32(_v:int)-> void:
	for _i:int in range(4):
		self.writeInt8(_v >> _i * 8 & 0xff)

func writeInt64(_v:int)-> void:
	for _i:int in range(8):
		self.writeInt8(_v >> _i * 8 & 0xff)

func writeUint8(_v:int)-> void:
	if self.space() <= 0:
		self.buffer.append(_v)
	else:
		self.buffer.encode_u8(self.wpos, _v)
	self.wpos += 1

func writeUint16(_v:int)-> void:
	self.writeUint8(_v & 0xff)
	self.writeUint8(_v >> 8 & 0xff)

func writeUint32(_v:int)-> void:
	for _i:int in range(4):
		self.writeUint8(_v >> _i * 8 & 0xff)

func writeUint64(_v:int)-> void:
	for _i:int in range(8):
		self.writeUint8(_v >> _i * 8 & 0xff)

func writeFloat(_v:float)-> void:
	var _byteSize:int = 4
	if self.space() < _byteSize:
		self.buffer.append_array(self.creatBuffer(_byteSize - self.space()))
	self.buffer.encode_float(self.wpos, _v)
	self.wpos += _byteSize

func writeDouble(_v:float)-> void:
	var _byteSize:int = 8
	if self.space() < _byteSize:
		self.buffer.append_array(self.creatBuffer(_byteSize - self.space()))
	self.buffer.encode_double(self.wpos, _v)
	self.wpos += _byteSize

func writeBlob(_v:PackedByteArray)-> void:
	var _size:int = _v.size()
	self.writeUint32(_size)
	self.ensureWriteSpace(_size)
	for _i:int in range(_size):
		self.buffer.encode_u8(self.wpos + _i, _v[_i])
	self.wpos += _size

func writeString(_str:String)-> void:
	var _buffer:PackedByteArray = _str.to_utf8_buffer()
	_buffer.append(0)
	var _size:int = _buffer.size()
	self.ensureWriteSpace(_size)
	for _i:int in range(_size):
		self.buffer.encode_u8(self.wpos + _i, _buffer[_i])
	self.wpos += _size

func writeVector2(_v:Vector2)-> void:
	writeFloat(_v.x)
	writeFloat(_v.y)

func writeVector3(_v:Vector3)-> void:
	writeFloat(_v.x)
	writeFloat(_v.y)
	writeFloat(_v.z)

func writeVector4(_v:Vector4)-> void:
	writeFloat(_v.x)
	writeFloat(_v.y)
	writeFloat(_v.z)
	writeFloat(_v.w)

func writeEntitycall(_v:PackedByteArray)-> void:
	var cid:int = 0
	var id:int = 0
	var type:int = 0
	var utype:int = 0
	writeUint64(cid)
	writeInt32(id)
	writeUint16(type)
	writeUint16(utype)

#---------------------------------------------------------------------------------
# endregion

func overWriteBuffer(_datas:PackedByteArray, _offset:int, _startIndex:int, _size:int)-> void:
	## print_debug("offset[" + str(_offset) + "]startIndex[" + str(_startIndex) +"]size[" + str(_size) + "]", _datas)
	var _buffer:PackedByteArray = _datas.slice(_offset, _offset + _size)
	if _startIndex + _size >= self.buffer.size():
		self.buffer = self.buffer.slice(0, _startIndex) + _buffer
	else:
		self.buffer = self.buffer.slice(0, _startIndex) + _buffer + self.buffer.slice(_startIndex + _size, self.buffer.size())

func append(_datas:PackedByteArray, _offset:int, _size:int)-> void:
	self.overWriteBuffer(_datas, _offset, self.wpos, _size)
	self.wpos += _size

#---------------------------------------------------------------------------------
func readSkip(_v:int)-> void:
	rpos += _v

#---------------------------------------------------------------------------------
func space()-> int:
	return self.buffer.size() - self.wpos

func creatBuffer(_num:int)-> PackedByteArray:
	var _buffer:PackedByteArray = PackedByteArray()
	if _num > 0:
		_buffer.resize(_num)
	return _buffer

func ensureWriteSpace(_size:int)-> void:
	if self.space() < _size:
		self.buffer.resize(self.wpos + _size)
	
func resizeForWrite()-> void:
	self.buffer.resize(self.wpos)

func length()-> int:
	return self.wpos - self.rpos

func readEOF()-> bool:
	return self.wpos - self.rpos <= 0

func done()-> void:
	self.rpos = self.wpos

func getbuffer()-> PackedByteArray:
	return self.buffer.slice(self.rpos, self.wpos)

func size()-> int:
	return self.wpos - self.rpos

func clear()-> void:
	self.rpos = 0
	self.wpos = 0
	if self.buffer.size() != KBEngine.PACKET_MAX_SIZE_TCP:
		self.buffer.resize(KBEngine.PACKET_MAX_SIZE_TCP)

func toString()-> String:
	var s:String = ""
	var ii:int = 0
	var buf:PackedByteArray = getbuffer()
	for i:int in range(buf.size()):
		ii += 1
		if ii >= 200:
			s = ""
			ii = 0
		s += char(buf[i])
		s += " "
	return s

