class_name EntityCall

enum ENTITYCALL_TYPE
{
	ENTITYCALL_TYPE_CELL = 0,		# CELL_ENTITYCALL_TYPE
	ENTITYCALL_TYPE_BASE = 1		# BASE_ENTITYCALL_TYPE
}

var id: int
var className: String
var type: ENTITYCALL_TYPE
var entitycomponentPropertyID:int = 0
var bundle: Bundle

func _init(_id:int, _className:String, _type:ENTITYCALL_TYPE=ENTITYCALL_TYPE.ENTITYCALL_TYPE_BASE):
	self.id = _id
	self.className = _className
	self.type = _type
	self.bundle = null

func isBase()-> bool:
	return self.type == ENTITYCALL_TYPE.ENTITYCALL_TYPE_BASE

func isCell()-> bool:
	return self.type == ENTITYCALL_TYPE.ENTITYCALL_TYPE_CELL

func newCall()-> Bundle:
	if not self.bundle:
		self.bundle = ObjectPool.createObject(Bundle)
	if self.isCell():
		self.bundle.newMessage(KBEngine.messages["Baseapp_onRemoteCallCellMethodFromClient"])
	else:
		self.bundle.newMessage(KBEngine.messages["Entity_onRemoteMethodCall"])
	self.bundle.writeInt32(self.id)
	return self.bundle

func newCallByName(_methodName:String, _entitycomponentPropertyID:int=0)-> Bundle:
	if KBEngine.app.currserver == "loginapp":
		Dbg.ERROR_MSG(self.className + "::newCall(" + _methodName + "), currserver=!" + KBEngine.app.currserver)
		return null

	var module:ScriptModule = EntityDef.moduledefs.get(self.className, null)
	if not module:
		Dbg.ERROR_MSG(self.className + "::newCall: entity-module(" + self.className + ") error, can not find from EntityDef.moduledefs")
		return null

	var method:Method = module.cell_methods[_methodName] if isCell() else module.base_methods[_methodName]
	var methodID:int = method.methodUtype
	newCall()
	self.bundle.writeUint16(_entitycomponentPropertyID)
	self.bundle.writeUint16(methodID)
	return self.bundle

func sendCall(_bundle:Bundle)-> void:
	if not _bundle:
		_bundle = self.bundle
	_bundle.send(KBEngine.app.networkInterface())
	if self.bundle == _bundle:
		self.bundle = null
