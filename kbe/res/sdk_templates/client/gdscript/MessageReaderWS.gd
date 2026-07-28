class_name MessageReaderWS extends MessageReaderBase
## WebSocket 运行时会先重组完整 message，本读取器在边界内收集解密片段并原子验证 KBE 消息序列。
## The WebSocket runtime first reassembles a complete message; this reader collects decrypted chunks within that boundary and validates the KBE message sequence atomically.

var _frameBuffer:PackedByteArray = PackedByteArray()
var _appendError:String = ""

func process(_datas:PackedByteArray, _offset:int, _length:int)-> void:
	if not _appendError.is_empty():
		return
	if _offset < 0 or _length < 0 or _offset > _datas.size() - _length:
		_appendError = "invalid decrypted frame slice"
		return

	_frameBuffer.append_array(_datas.slice(_offset, _offset + _length))

func finishFrame()-> String:
	var error:String = _appendError
	if error.is_empty():
		# 先验证整条 WebSocket message，再调用任何 handler，避免畸形尾部留下部分实体或事件状态。
		# Validate the whole WebSocket message before invoking handlers so a malformed tail cannot leave partial entity or event state.
		error = _walkFrame(false)
	if error.is_empty():
		error = _walkFrame(true)

	_frameBuffer.clear()
	_appendError = ""
	return error

func _walkFrame(_dispatch:bool)-> String:
	if _frameBuffer.is_empty():
		return "empty WebSocket message"

	var offset:int = 0
	var stream:MemoryStream = MemoryStream.new(_frameBuffer) if _dispatch else null
	while offset < _frameBuffer.size():
		if _frameBuffer.size() - offset < 2:
			return "truncated message id at offset " + str(offset)

		var message_id:int = _frameBuffer.decode_u16(offset)
		offset += 2
		var message:Messages.Message = Messages.clientMessages.get(message_id, null)
		if message == null:
			return "unknown message id " + str(message_id) + " at offset " + str(offset - 2)

		var body_length:int = message.msglen
		if body_length == -1:
			if _frameBuffer.size() - offset < 2:
				return "truncated message length for id " + str(message_id)
			body_length = _frameBuffer.decode_u16(offset)
			offset += 2
			if body_length == 65535:
				if _frameBuffer.size() - offset < 4:
					return "truncated extended message length for id " + str(message_id)
				body_length = _frameBuffer.decode_u32(offset)
				offset += 4

		if body_length < 0:
			return "invalid message length for id " + str(message_id)
		if body_length > _frameBuffer.size() - offset:
			return "message body exceeds WebSocket message for id " + str(message_id)

		if _dispatch:
			stream.rpos = offset
			stream.wpos = offset + body_length
			message.handleMessage(stream)

		offset += body_length

	return ""
