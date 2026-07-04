class_name ScriptModule

var name: String
var usePropertyDescrAlias: bool
var useMethodDescrAlias: bool

var propertys: Dictionary[String, Property] = {}
var idpropertys: Dictionary[int, Property] = {}

var methods: Dictionary[String, Method] = {}
var base_methods: Dictionary[String, Method] = {}
var cell_methods: Dictionary[String, Method] = {}

var idmethods: Dictionary[int, Method] = {}
var idbase_methods: Dictionary[int, Method] = {}
var idcell_methods: Dictionary[int, Method] = {}

var entityScript: Script = null

func _init(_modulename: String):
	self.name = _modulename
	self.usePropertyDescrAlias = false
	self.useMethodDescrAlias = false

	for global_class:Dictionary in ProjectSettings.get_global_class_list():
		if global_class.get("class", "") != _modulename:
			continue
		var script_path:String = global_class.get("path", "")
		if script_path.is_empty():
			continue
		self.entityScript = load(script_path) as Script
		if self.entityScript:
			break
	if not self.entityScript:
		Dbg.ERROR_MSG("can't load(KBEngine." + _modulename + ")!")
