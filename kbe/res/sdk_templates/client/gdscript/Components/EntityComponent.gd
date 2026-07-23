class_name EntityComponent
## 实体组件模块基础类

var entityComponentPropertyID:int
var componentType:int 
var ownerID:int
var owner:Entity
var name_:String

func onAttached(_ownerEntity:Entity)-> void:
	pass

func onDetached(_ownerEntity:Entity)-> void:
	pass

func onEnterworld()-> void:
	pass

func onLeaveworld()-> void:
	pass

func onGetBase()-> void:
	pass # 动态生成

func onGetCell()-> void:
	pass # 动态生成

func onLoseCell()-> void:
	pass # 动态生成

func getScriptModule()-> ScriptModule:
	return null # 动态生成

func onRemoteMethodCall(_methodUtype:int, _stream:MemoryStream)-> void:
	pass # 动态生成

func onUpdatePropertys(_propUtype:int, _stream:MemoryStream, _maxCount:int)-> void:
	pass # 动态生成

func callPropertysSetMethods()-> void:
	pass # 动态生成

func createFromStream(_stream:MemoryStream)-> void:
	self.componentType = _stream.readInt32()
	self.ownerID = _stream.readInt32()
	_stream.readUint16()  # UInt16 ComponentDescrsType
	var count:int = _stream.readUint16()
	if count > 0:
		onUpdatePropertys(0, _stream, count)
