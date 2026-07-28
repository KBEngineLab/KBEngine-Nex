extends SceneTree

var _dispatchCount:int = 0
var _checksum:int = 0

func _initialize()-> void:
	Messages.clientMessages.clear()
	Messages.clientMessages[7] = Messages.Message.new(Callable(self, "_onLargeMessage"), 7, "large", -1)
	Messages.clientMessages[1] = Messages.Message.new(Callable(self, "_onAtomicMessage"), 1, "fixed-three", 3)
	Messages.clientMessages[2] = Messages.Message.new(Callable(self, "_onAtomicMessage"), 2, "fixed-two", 2)

	if not _verifyLargeMessage() or not _verifyExtendedMessage() or not _verifyAtomicValidation() or not _verifyMalformedFrames():
		quit(1)
		return

	print("GDSCRIPT_WEBSOCKET_FRAME_TEST_PASS large=true bytes=60000 extended=true atomic=true unknown=true truncated=true overrun=true")
	quit(0)

func _verifyLargeMessage()-> bool:
	var body_size:int = 60000
	var frame:PackedByteArray = PackedByteArray()
	frame.resize(4 + body_size)
	frame.encode_u16(0, 7)
	frame.encode_u16(2, body_size)
	var expected_checksum:int = 0
	for index:int in range(body_size):
		var value:int = index % 251
		frame[4 + index] = value
		expected_checksum += value

	_dispatchCount = 0
	_checksum = 0
	var reader:MessageReaderWS = MessageReaderWS.new()
	reader.process(frame, 0, frame.size())
	var error:String = reader.finishFrame()
	return _expect(error.is_empty() and _dispatchCount == 1 and _checksum == expected_checksum,
		"valid 60 KiB message failed: " + error)

func _verifyAtomicValidation()-> bool:
	var frame:PackedByteArray = PackedByteArray([1, 0, 10, 11, 12, 2, 0, 9])
	_dispatchCount = 0
	var reader:MessageReaderWS = MessageReaderWS.new()
	reader.process(frame, 0, frame.size())
	var error:String = reader.finishFrame()
	return _expect(not error.is_empty() and _dispatchCount == 0,
		"malformed tail dispatched a valid prefix")

func _verifyExtendedMessage()-> bool:
	var body_size:int = 70000
	var frame:PackedByteArray = PackedByteArray()
	frame.resize(8 + body_size)
	frame.encode_u16(0, 7)
	frame.encode_u16(2, 65535)
	frame.encode_u32(4, body_size)
	_dispatchCount = 0
	_checksum = 0
	var reader:MessageReaderWS = MessageReaderWS.new()
	reader.process(frame, 0, frame.size())
	var error:String = reader.finishFrame()
	return _expect(error.is_empty() and _dispatchCount == 1,
		"valid extended-length message failed: " + error)

func _verifyMalformedFrames()-> bool:
	return (_expectRejected(PackedByteArray(), "empty frame")
		and _expectRejected(PackedByteArray([1]), "truncated id")
		and _expectRejected(PackedByteArray([99, 0]), "unknown id")
		and _expectRejected(PackedByteArray([7, 0, 1]), "truncated length")
		and _expectRejected(PackedByteArray([7, 0, 255, 255, 1, 2]), "truncated extended length")
		and _expectRejected(PackedByteArray([1, 0, 10]), "body overrun"))

func _expectRejected(_frame:PackedByteArray, _label:String)-> bool:
	var reader:MessageReaderWS = MessageReaderWS.new()
	reader.process(_frame, 0, _frame.size())
	return _expect(not reader.finishFrame().is_empty(), _label + " was accepted")

func _onLargeMessage(_stream:MemoryStream)-> void:
	_dispatchCount += 1
	while _stream.rpos < _stream.wpos:
		_checksum += _stream.readUint8()

func _onAtomicMessage(_stream:MemoryStream)-> void:
	_dispatchCount += 1
	_stream.rpos = _stream.wpos

func _expect(_condition:bool, _message:String)-> bool:
	if _condition:
		return true
	printerr("GDSCRIPT_WEBSOCKET_FRAME_TEST_FAIL error=" + _message)
	return false
