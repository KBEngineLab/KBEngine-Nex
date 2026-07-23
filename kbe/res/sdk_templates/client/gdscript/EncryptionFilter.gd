class_name EncryptionFilter

func encrypt(_stream:MemoryStream)-> void:
	pass

func decrypt(_stream:MemoryStream)-> void:
	pass

func decryptData(_buffer:PackedByteArray, _startIndex:int, _length:int)-> void:
	pass

func send(_socket:NetSocket, _stream:MemoryStream)-> bool:
	return false

func recv(_reader:MessageReaderBase, _buffer:PackedByteArray, _rpos:int, _len:int)-> bool:
	return false
