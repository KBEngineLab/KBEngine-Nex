class_name Messages

class Message extends RefCounted:
	var handler:Callable
	var msglen:int

	func _init(_handler:Callable, _id:int, _name:String, _msglen:int)-> void:
		# ID 和名称属于生成消息表元数据；fixture 只保留解析器实际读取的长度与回调。
		# ID and name are generated-table metadata; the fixture retains only the length and callback consumed by the parser.
		handler = _handler
		msglen = _msglen

	func handleMessage(_stream:MemoryStream)-> void:
		handler.call(_stream)

static var clientMessages:Dictionary[int, Message] = {}
