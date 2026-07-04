class_name DataTypes


class DATATYPE_BASE:
	func _resolveDataType(_datatype):
		if _datatype is int and EntityDef.id2datatypes.has(_datatype):
			return EntityDef.id2datatypes[_datatype]
		return _datatype

	func _isDataType(_datatype)-> bool:
		return _datatype is Object and _datatype.has_method("createFromStream")

	func bind()-> void:
		pass
	
	func createFromStream(_stream:MemoryStream):
		return
	
	func addToStream(_bundle:Bundle, _v)-> void:
		_bundle.writeUint8(_v)
	
	func parseDefaultValStr(_v:String):
		return int(_v)

	func isSameType(_v)-> bool:
		return false


class DATATYPE_UINT8 extends DATATYPE_BASE:
	
	func createFromStream(_stream:MemoryStream)-> int:
		return _stream.readUint8()
	
	func addToStream(_bundle:Bundle, _v:int)-> void:
		_bundle.writeUint8(_v)
	
	func isSameType(_v)-> bool:
		if not (_v is int):
			return false
		if _v < 0 or _v > 0xff:
			return false
		return true


class DATATYPE_UINT16 extends DATATYPE_BASE:
	
	func createFromStream(_stream:MemoryStream)-> int:
		return _stream.readUint16()
	
	func addToStream(_bundle:Bundle, _v:int)-> void:
		_bundle.writeUint16(_v)
	
	func isSameType(_v)-> bool:
		if not (_v is int):
			return false
		if _v < 0 or _v > 0xffff:
			return false
		return true


class DATATYPE_UINT32 extends DATATYPE_BASE:

	func createFromStream(_stream:MemoryStream)-> int:
		return _stream.readUint32()
	
	func addToStream(_bundle:Bundle, _v:int)-> void:
		_bundle.writeUint32(_v)
	
	func isSameType(_v)-> bool:
		if not (_v is int):
			return false
		if _v < 0 or _v > 0xffffffff:
			return false
		return true


class DATATYPE_UINT64 extends DATATYPE_BASE:

	func createFromStream(_stream:MemoryStream)-> int:
		return _stream.readUint64()
	
	func addToStream(_bundle:Bundle, _v:int)-> void:
		_bundle.writeUint64(_v)
	
	func isSameType(_v)-> bool:
		return _v is int and _v >= 0


class DATATYPE_INT8 extends DATATYPE_BASE:
	
	func createFromStream(_stream:MemoryStream)-> int:
		return _stream.readInt8()
	
	func addToStream(_bundle:Bundle, _v:int)-> void:
		_bundle.writeInt8(_v)
	
	func isSameType(_v)-> bool:
		if not (_v is int):
			return false
		if _v < -0x80 or _v > 0x7f:
			return false
		return true


class DATATYPE_INT16 extends DATATYPE_BASE:
	
	func createFromStream(_stream:MemoryStream)-> int:
		return _stream.readInt16()
	
	func addToStream(_bundle:Bundle, _v:int)-> void:
		_bundle.writeInt16(_v)
	
	func isSameType(_v)-> bool:
		if not (_v is int):
			return false
		if _v < -0x8000 or _v > 0x7fff:
			return false
		return true


class DATATYPE_INT32 extends DATATYPE_BASE:
	
	func createFromStream(_stream:MemoryStream)-> int:
		return _stream.readInt32()
	
	func addToStream(_bundle:Bundle, _v:int)-> void:
		_bundle.writeInt32(_v)
	
	func isSameType(_v)-> bool:
		if not (_v is int):
			return false
		if _v < -0x80000000 or _v > 0x7fffffff:
			return false
		return true


class DATATYPE_INT64 extends DATATYPE_BASE:

	func createFromStream(_stream:MemoryStream)-> int:
		return _stream.readInt64()

	func addToStream(_bundle:Bundle, _v:int)-> void:
		_bundle.writeInt64(_v)

	func isSameType(_v)-> bool:
		return _v is int


class DATATYPE_FLOAT extends DATATYPE_BASE:

	func createFromStream(_stream:MemoryStream)-> float:
		return _stream.readFloat()

	func addToStream(_bundle:Bundle, _v:float)-> void:
		_bundle.writeFloat(_v)

	func parseDefaultValStr(_v:String)-> float:
		return float(_v)

	func isSameType(_v)-> bool:
		return _v is int or _v is float


class DATATYPE_DOUBLE extends DATATYPE_BASE:

	func createFromStream(_stream:MemoryStream)-> float:
		return _stream.readDouble()

	func addToStream(_bundle:Bundle, _v:float)-> void:
		_bundle.writeDouble(_v)
	
	func parseDefaultValStr(_v:String)-> float:
		return float(_v)

	func isSameType(_v)-> bool:
		return _v is int or _v is float


class DATATYPE_STRING extends DATATYPE_BASE:

	func createFromStream(_stream:MemoryStream)-> String:
		return _stream.readString()

	func addToStream(_bundle:Bundle, _v:String)-> void:
		_bundle.writeString(_v)

	func parseDefaultValStr(_v:String)-> String:
		if _v is String:
			return _v
		return str(_v)

	func isSameType(_v)-> bool:
		return _v is String


class DATATYPE_VECTOR2 extends DATATYPE_BASE:

	func createFromStream(_stream:MemoryStream)-> Vector2:
		var _x = _stream.readFloat()
		var _y = _stream.readFloat()
		return Vector2(_x, _y)

	func addToStream(_bundle:Bundle, _v:Vector2)-> void:
		_bundle.writeFloat(_v.x)
		_bundle.writeFloat(_v.y)

	func parseDefaultValStr(_v:String)-> Vector2:
		return Vector2()

	func isSameType(_v)-> bool:
		return _v is Vector2 or _v is Vector2i


class DATATYPE_VECTOR3 extends DATATYPE_BASE:
	
	func createFromStream(_stream:MemoryStream)-> Vector3:
		var _x = _stream.readFloat()
		var _y = _stream.readFloat()
		var _z = _stream.readFloat()
		return Vector3(_x, _y, _z)

	func addToStream(_bundle:Bundle, _v:Vector3)-> void:
		_bundle.writeFloat(_v.x)
		_bundle.writeFloat(_v.y)
		_bundle.writeFloat(_v.z)

	func parseDefaultValStr(_v:String)-> Vector3:
		return Vector3()

	func isSameType(_v)-> bool:
		return _v is Vector3 or _v is Vector3i


class DATATYPE_VECTOR4 extends DATATYPE_BASE:
	
	func createFromStream(_stream:MemoryStream)-> Vector4:
		var _x = _stream.readFloat()
		var _y = _stream.readFloat()
		var _z = _stream.readFloat()
		var _w = _stream.readFloat()
		return Vector4(_x, _y, _z, _w)
	
	func addToStream(_bundle:Bundle, _v:Vector4)-> void:
		_bundle.writeFloat(_v.x)
		_bundle.writeFloat(_v.y)
		_bundle.writeFloat(_v.z)
		_bundle.writeFloat(_v.w)
	
	func parseDefaultValStr(_v:String)-> Vector4:
		return Vector4()

	func isSameType(_v)-> bool:
		return _v is Vector4 or _v is Vector4i


class DATATYPE_PYTHON extends DATATYPE_BASE:

	func createFromStream(_stream:MemoryStream)-> PackedByteArray:
		return _stream.readBlob()
	
	func addToStream(_bundle:Bundle, _v:PackedByteArray)-> void:
		_bundle.writeBlob(_v)

	func parseDefaultValStr(_v:String)-> PackedByteArray:
		return PackedByteArray()

	func isSameType(_v)-> bool:
		return _v is PackedByteArray or _v == null


class DATATYPE_UNICODE extends DATATYPE_BASE:

	func createFromStream(_stream:MemoryStream)-> String:
		return KBETools.utf8ArrayToString(_stream.readBlob())

	func addToStream(_bundle:Bundle, _v:String)-> void:
		_bundle.writeBlob(KBETools.stringToUTF8Bytes(_v))

	func parseDefaultValStr(_v:String):
		if _v is String:
			return _v
		return str(_v)

	func isSameType(_v)-> bool:
		return _v is String or _v == null


class DATATYPE_ENTITYCALL extends DATATYPE_BASE:
	
	# var cid:int
	# var id:int
	# var vtype:int
	# var utype:int
	
	# func _init():
	# 	self.reset()
		
	# func reset():
	# 	self.cid = 0
	# 	self.id = 0
	# 	self.vtype = 0
	# 	self.utype = 0
	
	# func createFromStream(_stream:MemoryStream):
	# 	self.cid = _stream.readUint64()
	# 	self.id = _stream.readInt32()
	# 	self.vtype = _stream.readUint16()
	# 	self.utype = _stream.readUint16()
	
	# func addToStream(_bundle:Bundle, _v)-> void:
	# 	_stream.writeUint64(self.cid)
	# 	_stream.writeInt32(self.id)
	# 	_stream.writeUint16(self.vtype)
	# 	_stream.writeUint16(self.utype)
	# 	self.reset()
	
	func createFromStream(_stream:MemoryStream)-> PackedByteArray:
		return _stream.readBlob()
	
	func addToStream(_bundle:Bundle, _v:PackedByteArray)-> void:
		_bundle.writeBlob(_v)

	func parseDefaultValStr(_v:String)-> PackedByteArray:
		return PackedByteArray()

	func isSameType(_v)-> bool:
		return _v is PackedByteArray or _v == null


class DATATYPE_BLOB extends DATATYPE_BASE:

	# func createFromStream(_stream:MemoryStream):
	# 	var _size = _stream.readUint32()
	# 	var _buf = _stream.buffer.slice(_stream.rpos, _stream.rpos + _size)
	# 	_stream.rpos += _size
	# 	return _buf

	func createFromStream(_stream:MemoryStream)-> PackedByteArray:
		return _stream.readBlob()
	
	func addToStream(_bundle:Bundle, _v:PackedByteArray)-> void:
		_bundle.writeBlob(_v)
	
	func parseDefaultValStr(_v:String)-> PackedByteArray:
		return PackedByteArray()

	func isSameType(_v)-> bool:
		return _v is PackedByteArray or _v == null


class DATATYPE_ARRAY extends DATATYPE_BASE:

	var vtype
	var type_id:int = -2
	
	func bind()-> void:  # TODO 这里的复合类型的bind是存疑的
		self.vtype = _resolveDataType(self.vtype)
		if not self.vtype and EntityDef.id2datatypes.has(self.type_id):
			self.vtype = EntityDef.id2datatypes[self.type_id]
		if self.vtype is Object and self.vtype.has_method("bind"):
			self.vtype.bind()

	func createFromStream(_stream:MemoryStream)-> Array:
		var _size:int = _stream.readUint32()
		var _datas:Array = []
		self.vtype = _resolveDataType(self.vtype)
		if not _isDataType(self.vtype):
			Dbg.ERROR_MSG("DATATYPE_ARRAY.createFromStream: unresolved child datatype " + str(self.vtype))
			return _datas
		while _size > 0:
			_size -= 1
			_datas.append(self.vtype.createFromStream(_stream))
		return _datas
	
	func addToStream(_bundle:Bundle, _v:Array)-> void:
		self.vtype = _resolveDataType(self.vtype)
		if not _isDataType(self.vtype):
			Dbg.ERROR_MSG("DATATYPE_ARRAY.addToStream: unresolved child datatype " + str(self.vtype))
			return
		_bundle.writeUint32(_v.size())
		for _item in _v:
			self.vtype.addToStream(_bundle, _item)
	
	func parseDefaultValStr(_v:String):
		return [0]
	
	func isSameType(_v)-> bool:
		if not (_v is Array):
			return false
		self.vtype = _resolveDataType(self.vtype)
		if not _isDataType(self.vtype):
			return false
		for _item in _v:
			if not self.vtype.isSameType(_item):
				return false
		return true


class DATATYPE_FIXED_DICT extends DATATYPE_BASE:
	var implementedBy:String = ""
	var dicttype:Dictionary = {}

	func bind()-> void:  # TODO 这里的复合类型的bind是存疑的
		for _itemkey in self.dicttype:
			self.dicttype[_itemkey] = _resolveDataType(self.dicttype[_itemkey])
			var _datatype = self.dicttype[_itemkey]
			if _datatype is Object and _datatype.has_method("bind"):
				_datatype.bind()

	func createFromStream(_stream:MemoryStream)-> Dictionary:
		var _datas:Dictionary = {}
		for _itemkey:String in self.dicttype:
			self.dicttype[_itemkey] = _resolveDataType(self.dicttype[_itemkey])
			var _datatype = self.dicttype[_itemkey]
			if not _isDataType(_datatype):
				Dbg.ERROR_MSG("DATATYPE_FIXED_DICT.createFromStream: unresolved child datatype " + str(_datatype) + " for key " + _itemkey)
				return _datas
			_datas[_itemkey] = _datatype.createFromStream(_stream)
		return _datas
	
	func addToStream(_bundle:Bundle, _v:Dictionary)-> void:
		for _itemkey:String in self.dicttype:
			self.dicttype[_itemkey] = _resolveDataType(self.dicttype[_itemkey])
			var _datatype = self.dicttype[_itemkey]
			if not _isDataType(_datatype):
				Dbg.ERROR_MSG("DATATYPE_FIXED_DICT.addToStream: unresolved child datatype " + str(_datatype) + " for key " + _itemkey)
				return
			_datatype.addToStream(_bundle, _v[_itemkey])
	
	func parseDefaultValStr(_v:String)-> Dictionary:
		var _datas:Dictionary = {}
		for _itemkey:String in self.dicttype:
			self.dicttype[_itemkey] = _resolveDataType(self.dicttype[_itemkey])
			var _datatype = self.dicttype[_itemkey]
			if not (_datatype is Object and _datatype.has_method("parseDefaultValStr")):
				_datas[_itemkey] = null
			else:
				_datas[_itemkey] = _datatype.parseDefaultValStr(_v)
		return _datas

	func isSameType(_v)-> bool:
		if not (_v is Dictionary):
			return false
		for _itemkey in self.dicttype:
			if not _v.has(_itemkey):
				return false
			self.dicttype[_itemkey] = _resolveDataType(self.dicttype[_itemkey])
			var _datatype = self.dicttype[_itemkey]
			if not (_datatype is Object and _datatype.has_method("isSameType")):
				return false
			if not _datatype.isSameType(_v[_itemkey]):
				return false
		return true
