// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

#include "kbcmd.h"
#include "client_sdk_gdscript.h"
#include "entitydef/entitydef.h"
#include "entitydef/scriptdef_module.h"
#include "entitydef/property.h"
#include "entitydef/method.h"
#include "entitydef/datatype.h"
#include "network/fixed_messages.h"
#include "resmgr/resmgr.h"

namespace KBEngine {

static std::string cap(std::string s)
{
	if (!s.empty())
		s[0] = (char)std::toupper(s[0]);
	return s;
}

static std::string gdType(const std::string& type)
{
	if (type == "FLOAT" || type == "DOUBLE")
		return "float";
	if (type == "STRING" || type == "UNICODE")
		return "String";
	if (type == "VECTOR2")
		return "Vector2";
	if (type == "VECTOR3")
		return "Vector3";
	if (type == "VECTOR4")
		return "Vector4";
	if (type == "PYTHON" || type == "PY_DICT" || type == "PY_TUPLE" || type == "PY_LIST" || type == "BLOB" || type == "ENTITYCALL")
		return "PackedByteArray";
	if (type == "ARRAY")
		return "Array";
	if (type == "FIXED_DICT")
		return "Dictionary";
	return "int";
}

static std::string readFunc(const std::string& type)
{
	if (type == "INT8") return "readInt8";
	if (type == "INT16") return "readInt16";
	if (type == "INT32") return "readInt32";
	if (type == "INT64") return "readInt64";
	if (type == "UINT8") return "readUint8";
	if (type == "UINT16") return "readUint16";
	if (type == "UINT32") return "readUint32";
	if (type == "UINT64") return "readUint64";
	if (type == "FLOAT") return "readFloat";
	if (type == "DOUBLE") return "readDouble";
	if (type == "STRING") return "readString";
	if (type == "UNICODE") return "readUnicode";
	if (type == "VECTOR2") return "readVector2";
	if (type == "VECTOR3") return "readVector3";
	if (type == "VECTOR4") return "readVector4";
	if (type == "PYTHON" || type == "PY_DICT" || type == "PY_TUPLE" || type == "PY_LIST") return "readPython";
	if (type == "BLOB" || type == "ENTITYCALL") return "readBlob";
	return "";
}

static std::string defaultValue(PropertyDescription* pDescr)
{
	std::string type = pDescr->getDataType()->getName();
	const char* raw = pDescr->getDefaultValStr();
	std::string def = raw && strlen(raw) > 0 ? raw : "";

	if (type == "STRING" || type == "UNICODE")
		return fmt::format("\"{}\"", def);
	if (type == "VECTOR2")
		return def.empty() ? "Vector2()" : def;
	if (type == "VECTOR3")
		return def.empty() ? "Vector3()" : def;
	if (type == "VECTOR4")
		return def.empty() ? "Vector4()" : def;
	if (type == "ARRAY")
		return "[]";
	if (type == "FIXED_DICT")
		return "{}";
	if (type == "PYTHON" || type == "PY_DICT" || type == "PY_TUPLE" || type == "PY_LIST" || type == "BLOB" || type == "ENTITYCALL")
		return "PackedByteArray()";
	return def.empty() ? "0" : def;
}

static uint16 dataTypeID(DataType* pDataType)
{
	uint16 typeID = pDataType->id();
	if (typeID == 0)
		typeID = datatype2id(pDataType->getName());
	return typeID;
}

static std::string gdDefTypeVarName(const DataType* pDataType)
{
	std::string typeName = pDataType->aliasName();
	if (!typeName.empty())
		return typeName;

	return fmt::format("{}_{}", pDataType->getName(), pDataType->id());
}

ClientSDKGDScript::ClientSDKGDScript():
	ClientSDK(),
	initBody_()
{
}

ClientSDKGDScript::~ClientSDKGDScript()
{
}

bool ClientSDKGDScript::create(const std::string& path)
{
	basepath_ = path;

	if (basepath_[basepath_.size() - 1] != '\\' && basepath_[basepath_.size() - 1] != '/')
		basepath_ += "/";

	currHeaderPath_ = currSourcePath_ = basepath_;

	std::string findpath = "sdk_templates/client/" + name();
	std::string getpath = Resmgr::getSingleton().matchPath(findpath);

	if (getpath.size() == 0 || findpath == getpath)
	{
		ERROR_MSG(fmt::format("ClientSDKGDScript::create(): not found path({})\n", findpath));
		return false;
	}

	if (!copyPluginsSourceToPath(getpath))
		return false;

	if (!writeServerErrorDescrsModule())
		return false;

	if (!writeEngineMessagesModule())
		return false;

	if (!writeTypes())
		return false;

	if (!writeEntityDefsModule())
		return false;

	const EntityDef::SCRIPT_MODULES& scriptModules = EntityDef::getScriptModules();
	EntityDef::SCRIPT_MODULES::const_iterator moduleIter = scriptModules.begin();
	for (; moduleIter != scriptModules.end(); ++moduleIter)
	{
		ScriptDefModule* pScriptDefModule = (*moduleIter).get();

		if (!writeEntityModule(pScriptDefModule))
			return false;
	}

	return true;
}

std::string ClientSDKGDScript::typeToType(const std::string& type)
{
	return gdType(type);
}

bool ClientSDKGDScript::writeTypes()
{
	return true;
}

bool ClientSDKGDScript::writeCustomDataTypes()
{
	return true;
}

bool ClientSDKGDScript::writeCustomDataTypesBegin()
{
	return true;
}

bool ClientSDKGDScript::writeCustomDataTypesEnd()
{
	return true;
}

bool ClientSDKGDScript::writeCustomDataType(const DataType* pDataType)
{
	return true;
}

bool ClientSDKGDScript::writeEntityCall(ScriptDefModule* pScriptDefModule)
{
	if (sourcefileBody_.empty())
		return true;

	if (!writeBaseEntityCallBegin(pScriptDefModule))
		return false;

	ScriptDefModule::METHODDESCRIPTION_MAP& baseMethods = pScriptDefModule->getBaseMethodDescriptions();
	for (ScriptDefModule::METHODDESCRIPTION_MAP::iterator it = baseMethods.begin(); it != baseMethods.end(); ++it)
	{
		MethodDescription* pMethodDescription = it->second;
		if (!pMethodDescription->isExposed())
			continue;

		std::string args;
		std::vector<DataType*>& argTypes = pMethodDescription->getArgTypes();
		for (size_t i = 0; i < argTypes.size(); ++i)
			args += fmt::format("arg{}:{}, ", i + 1, gdType(argTypes[i]->getName()));

		if (!args.empty())
			args.erase(args.size() - 2, 2);

		if (!writeEntityCallMethodBegin(pScriptDefModule, pMethodDescription, args.c_str(), "", BASEAPP_TYPE))
			return false;
		if (!writeEntityCallMethodEnd(pScriptDefModule, pMethodDescription))
			return false;
	}

	if (!writeBaseEntityCallEnd(pScriptDefModule))
		return false;

	if (!writeCellEntityCallBegin(pScriptDefModule))
		return false;

	ScriptDefModule::METHODDESCRIPTION_MAP& cellMethods = pScriptDefModule->getCellMethodDescriptions();
	for (ScriptDefModule::METHODDESCRIPTION_MAP::iterator it = cellMethods.begin(); it != cellMethods.end(); ++it)
	{
		MethodDescription* pMethodDescription = it->second;
		if (!pMethodDescription->isExposed())
			continue;

		std::string args;
		std::vector<DataType*>& argTypes = pMethodDescription->getArgTypes();
		for (size_t i = 0; i < argTypes.size(); ++i)
			args += fmt::format("arg{}:{}, ", i + 1, gdType(argTypes[i]->getName()));

		if (!args.empty())
			args.erase(args.size() - 2, 2);

		if (!writeEntityCallMethodBegin(pScriptDefModule, pMethodDescription, args.c_str(), "", CELLAPP_TYPE))
			return false;
		if (!writeEntityCallMethodEnd(pScriptDefModule, pMethodDescription))
			return false;
	}

	return writeCellEntityCallEnd(pScriptDefModule);
}

void ClientSDKGDScript::onCreateEntityModuleFileName(const std::string& moduleName)
{
	sourcefileName_ = moduleName + "Base.gd";
	headerfileName_ = "";
	currSourcePath_ = basepath_ + "Entities/";
}

void ClientSDKGDScript::onCreateServerErrorDescrsModuleFileName()
{
	sourcefileName_ = "ServerErrorDescrs.gd";
	headerfileName_ = "";
	currSourcePath_ = basepath_;
}

void ClientSDKGDScript::onCreateEngineMessagesModuleFileName()
{
	sourcefileName_ = "Messages.gd";
	headerfileName_ = "";
	currSourcePath_ = basepath_;
}

void ClientSDKGDScript::onCreateEntityDefsModuleFileName()
{
	sourcefileName_ = "EntityDef.gd";
	headerfileName_ = "";
	currSourcePath_ = basepath_;
}

void ClientSDKGDScript::onEntityCallModuleFileName(const std::string& moduleName)
{
	onCreateEntityModuleFileName(moduleName);
}

bool ClientSDKGDScript::writeServerErrorDescrsModuleBegin()
{
	sourcefileBody_ = "class_name ServerErrorDescrs\n\n";
	sourcefileBody_ += "var serverErrs:Dictionary[int, Dictionary] = {}\n\n";
	sourcefileBody_ += "func Clear()-> void:\n\tserverErrs.clear()\n\n";
	sourcefileBody_ += "func serverErrStr(_id:int)-> String:\n\tvar err:Dictionary = serverErrs.get(_id, {})\n\tif err.is_empty():\n\t\treturn \"\"\n\treturn str(err.get(\"name\", \"\")) + \"[\" + str(err.get(\"descr\", \"\")) + \"]\"\n\n";
	sourcefileBody_ += "func _init()-> void:\n\tself.serverErrs = {\n";
	return true;
}

bool ClientSDKGDScript::writeServerErrorDescrsModuleErrDescr(int errorID, const std::string& errname, const std::string& errdescr)
{
	sourcefileBody_ += fmt::format("\t\t{}: {{\"name\": \"{}\", \"descr\": \"{}\"}},\n", errorID, errname, errdescr);
	return true;
}

bool ClientSDKGDScript::writeServerErrorDescrsModuleEnd()
{
	if (sourcefileBody_.size() > 3)
		sourcefileBody_.erase(sourcefileBody_.size() - 2, 1);
	sourcefileBody_ += "\t}\n";
	return true;
}

bool ClientSDKGDScript::writeEngineMessagesModuleBegin()
{
	initBody_ = "";
	sourcefileBody_ = "class_name Messages\n\n";
	sourcefileBody_ += "class Message:\n\tvar handleMessageFunc:Callable\n\tvar id:int\n\tvar name:String\n\tvar msglen:int = -1\n\n";
	sourcefileBody_ += "\tfunc _init(_handle:Callable, _msgid:int, _msgname:String, _length:int)-> void:\n\t\tself.handleMessageFunc = _handle\n\t\tself.id = _msgid\n\t\tself.name = _msgname\n\t\tself.msglen = _length\n\n";
	sourcefileBody_ += "\tfunc handleMessage(_msgstream:MemoryStream)-> void:\n\t\tself.handleMessageFunc.call(_msgstream)\n\n";
	sourcefileBody_ += "static var loginappMessages:Dictionary[int, Message] = {}\nstatic var baseappMessages:Dictionary[int, Message] = {}\nstatic var clientMessages:Dictionary[int, Message] = {}\nstatic var messages:Dictionary[String, Message] = {}\n\n";
	sourcefileBody_ += "static func clear()-> void:\n\tloginappMessages = {}\n\tbaseappMessages = {}\n\tclientMessages = {}\n\tmessages = {}\n\tinit()\n";
	return true;
}

bool ClientSDKGDScript::writeEngineMessagesModuleMessage(Network::ExposedMessageInfo& messageInfos, COMPONENT_TYPE componentType)
{
	std::string argsBody;
	std::string callBody;

	sourcefileBody_ += fmt::format("\nstatic func Message_{}(_msgstream:MemoryStream)-> void:\n", messageInfos.name);

	if (messageInfos.argsTypes.empty())
	{
		if (componentType == CLIENT_TYPE)
		{
			if (messageInfos.argsType < 0)
				sourcefileBody_ += fmt::format("\tKBEngine.app.{}(_msgstream)\n", messageInfos.name);
			else
				sourcefileBody_ += fmt::format("\tKBEngine.app.{}()\n", messageInfos.name);
		}
		else
		{
			sourcefileBody_ += "\tpass\n";
		}
	}
	else
	{
		for (size_t i = 0; i < messageInfos.argsTypes.size(); ++i)
		{
			std::string nativeType = datatype2nativetype(messageInfos.argsTypes[i]);
			std::string fn = readFunc(nativeType);
			sourcefileBody_ += fmt::format("\tvar arg{}:{} = _msgstream.{}()\n", i + 1, gdType(nativeType), fn.empty() ? "readBlob" : fn);
			callBody += fmt::format("arg{}, ", i + 1);
		}

		if (!callBody.empty())
			callBody.erase(callBody.size() - 2, 2);

		if (componentType == CLIENT_TYPE)
			sourcefileBody_ += fmt::format("\tKBEngine.app.{}({})\n", messageInfos.name, callBody);
		else
			sourcefileBody_ += "\tpass\n";
	}

	const char* table = componentType == CLIENT_TYPE ? "clientMessages" : (componentType == LOGINAPP_TYPE ? "loginappMessages" : "baseappMessages");
	initBody_ += fmt::format("\tMessages.messages[\"{}\"] = Message.new(Message_{}, {}, \"{}\", {})\n", messageInfos.name, messageInfos.name, messageInfos.id, messageInfos.name, messageInfos.msgLen);
	initBody_ += fmt::format("\tMessages.{}[{}] = Messages.messages[\"{}\"]\n", table, messageInfos.id, messageInfos.name);
	return true;
}

bool ClientSDKGDScript::writeEngineMessagesModuleEnd()
{
	sourcefileBody_ += "\nstatic func init()-> bool:\n";
	sourcefileBody_ += initBody_.empty() ? "\treturn true\n" : initBody_;
	sourcefileBody_ += "\treturn true\n";
	return true;
}

bool ClientSDKGDScript::writeEntityDefsModuleBegin()
{
	sourcefileBody_ = "class_name EntityDef\n\n";
	sourcefileBody_ += "static var datatype2id:Dictionary[String, int] = {}\nstatic var datatypes:Dictionary[String, DataTypes.DATATYPE_BASE] = {}\nstatic var id2datatypes:Dictionary[int, DataTypes.DATATYPE_BASE] = {}\n";
	sourcefileBody_ += "static var entityclass:Dictionary[String, int] = {}\nstatic var moduledefs:Dictionary[String, ScriptModule] = {}\nstatic var idmoduledefs:Dictionary[int, ScriptModule] = {}\n\n";
	sourcefileBody_ += "static func init()-> bool:\n\tinitDataTypes()\n\tinitDefTypes()\n\tinitScriptModules()\n\treturn true\n\n";
	sourcefileBody_ += "static func reset()-> bool:\n\tclear()\n\treturn init()\n\n";
	sourcefileBody_ += "static func clear()-> void:\n\tdatatype2id.clear()\n\tdatatypes.clear()\n\tid2datatypes.clear()\n\tentityclass.clear()\n\tmoduledefs.clear()\n\tidmoduledefs.clear()\n\n";
	sourcefileBody_ += "static func initDataTypes()-> void:\n";
	const char* names[] = {"UINT8","UINT16","UINT32","UINT64","INT8","INT16","INT32","INT64","FLOAT","DOUBLE","STRING","VECTOR2","VECTOR3","VECTOR4","PYTHON","UNICODE","ENTITYCALL","BLOB"};
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
		sourcefileBody_ += fmt::format("\tdatatypes[\"{}\"] = DataTypes.DATATYPE_{}.new()\n", names[i], names[i]);
	return true;
}

bool ClientSDKGDScript::writeEntityDefsModuleEnd()
{
	return true;
}

bool ClientSDKGDScript::writeEntityDefsModuleInitScriptBegin()
{
	sourcefileBody_ += "\nstatic func initScriptModules()-> void:\n";
	return true;
}

bool ClientSDKGDScript::writeEntityDefsModuleInitScriptEnd()
{
	sourcefileBody_ += "\tpass\n";
	return true;
}

bool ClientSDKGDScript::writeEntityDefsModuleInitScript_ScriptModule(ScriptDefModule* pScriptDefModule)
{
	sourcefileBody_ += fmt::format("\tvar p{}Module:ScriptModule = ScriptModule.new(\"{}\")\n", pScriptDefModule->getName(), pScriptDefModule->getName());
	sourcefileBody_ += fmt::format("\tEntityDef.moduledefs[\"{}\"] = p{}Module\n", pScriptDefModule->getName(), pScriptDefModule->getName());
	sourcefileBody_ += fmt::format("\tEntityDef.idmoduledefs[{}] = p{}Module\n\n", pScriptDefModule->getUType(), pScriptDefModule->getName());
	return true;
}

bool ClientSDKGDScript::writeEntityDefsModuleInitScript_MethodDescr(ScriptDefModule* pScriptDefModule, MethodDescription* pDescr, COMPONENT_TYPE componentType)
{
	if (!pDescr)
	{
		if (componentType == CLIENT_TYPE)
			sourcefileBody_ += fmt::format("\tp{}Module.useMethodDescrAlias = true\n", pScriptDefModule->getName());
		return true;
	}

	sourcefileBody_ += fmt::format("\tvar p{}_{}_args:Array[DataTypes.DATATYPE_BASE] = []\n", pScriptDefModule->getName(), pDescr->getName());
	const std::vector<DataType*>& args = pDescr->getArgTypes();
	for (std::vector<DataType*>::const_iterator it = args.begin(); it != args.end(); ++it)
		sourcefileBody_ += fmt::format("\tp{}_{}_args.append(EntityDef.id2datatypes[{}])\n", pScriptDefModule->getName(), pDescr->getName(), dataTypeID(*it));

	sourcefileBody_ += fmt::format("\tvar p{}_{}:Method = Method.new()\n", pScriptDefModule->getName(), pDescr->getName());
	sourcefileBody_ += fmt::format("\tp{}_{}.name = \"{}\"\n", pScriptDefModule->getName(), pDescr->getName(), pDescr->getName());
	sourcefileBody_ += fmt::format("\tp{}_{}.methodUtype = {}\n", pScriptDefModule->getName(), pDescr->getName(), pDescr->getUType());
	sourcefileBody_ += fmt::format("\tp{}_{}.aliasID = {}\n", pScriptDefModule->getName(), pDescr->getName(), pDescr->aliasID());
	sourcefileBody_ += fmt::format("\tp{}_{}.args = p{}_{}_args\n", pScriptDefModule->getName(), pDescr->getName(), pScriptDefModule->getName(), pDescr->getName());

	const char* dict = componentType == BASEAPP_TYPE ? "base_methods" : (componentType == CELLAPP_TYPE ? "cell_methods" : "methods");
	const char* iddict = componentType == BASEAPP_TYPE ? "idbase_methods" : (componentType == CELLAPP_TYPE ? "idcell_methods" : "idmethods");
	sourcefileBody_ += fmt::format("\tp{}Module.{}[\"{}\"] = p{}_{}\n", pScriptDefModule->getName(), dict, pDescr->getName(), pScriptDefModule->getName(), pDescr->getName());

	if (pDescr->aliasID() != -1 && componentType == CLIENT_TYPE)
	{
		sourcefileBody_ += fmt::format("\tp{}Module.useMethodDescrAlias = true\n", pScriptDefModule->getName());
		sourcefileBody_ += fmt::format("\tp{}Module.{}[int(p{}_{}.aliasID)] = p{}_{}\n\n", pScriptDefModule->getName(), iddict, pScriptDefModule->getName(), pDescr->getName(), pScriptDefModule->getName(), pDescr->getName());
	}
	else
	{
		if (componentType == CLIENT_TYPE)
			sourcefileBody_ += fmt::format("\tp{}Module.useMethodDescrAlias = false\n", pScriptDefModule->getName());
		sourcefileBody_ += fmt::format("\tp{}Module.{}[p{}_{}.methodUtype] = p{}_{}\n\n", pScriptDefModule->getName(), iddict, pScriptDefModule->getName(), pDescr->getName(), pScriptDefModule->getName(), pDescr->getName());
	}

	return true;
}

bool ClientSDKGDScript::writeEntityDefsModuleInitScript_PropertyDescr(ScriptDefModule* pScriptDefModule, PropertyDescription* pDescr)
{
	sourcefileBody_ += fmt::format("\tvar p{}_{}:Property = Property.new()\n", pScriptDefModule->getName(), pDescr->getName());
	sourcefileBody_ += fmt::format("\tp{}_{}.name = \"{}\"\n", pScriptDefModule->getName(), pDescr->getName(), pDescr->getName());
	sourcefileBody_ += fmt::format("\tp{}_{}.properUtype = {}\n", pScriptDefModule->getName(), pDescr->getName(), pDescr->getUType());
	sourcefileBody_ += fmt::format("\tp{}_{}.properFlags = {}\n", pScriptDefModule->getName(), pDescr->getName(), pDescr->getFlags());
	sourcefileBody_ += fmt::format("\tp{}_{}.aliasID = {}\n", pScriptDefModule->getName(), pDescr->getName(), pDescr->aliasID());

	if (pDescr->getDataType()->type() != DATA_TYPE_ENTITY_COMPONENT)
	{
		if (strcmp(pDescr->getDataType()->getName(), "FIXED_DICT") == 0 || strcmp(pDescr->getDataType()->getName(), "ARRAY") == 0)
			sourcefileBody_ += fmt::format("\tp{}_{}.defaultVal = EntityDef.id2datatypes[{}].parseDefaultValStr(\"{}\")\n", pScriptDefModule->getName(), pDescr->getName(), dataTypeID(pDescr->getDataType()), pDescr->getDefaultValStr());
		else
			sourcefileBody_ += fmt::format("\tp{}_{}.defaultVal = {}\n", pScriptDefModule->getName(), pDescr->getName(), defaultValue(pDescr));
	}

	sourcefileBody_ += fmt::format("\tp{}Module.propertys[\"{}\"] = p{}_{}\n", pScriptDefModule->getName(), pDescr->getName(), pScriptDefModule->getName(), pDescr->getName());
	if (pDescr->aliasID() != -1)
	{
		sourcefileBody_ += fmt::format("\tp{}Module.usePropertyDescrAlias = true\n", pScriptDefModule->getName());
		sourcefileBody_ += fmt::format("\tp{}Module.idpropertys[int(p{}_{}.aliasID)] = p{}_{}\n\n", pScriptDefModule->getName(), pScriptDefModule->getName(), pDescr->getName(), pScriptDefModule->getName(), pDescr->getName());
	}
	else
	{
		sourcefileBody_ += fmt::format("\tp{}Module.usePropertyDescrAlias = false\n", pScriptDefModule->getName());
		sourcefileBody_ += fmt::format("\tp{}Module.idpropertys[p{}_{}.properUtype] = p{}_{}\n\n", pScriptDefModule->getName(), pScriptDefModule->getName(), pDescr->getName(), pScriptDefModule->getName(), pDescr->getName());
	}
	return true;
}

bool ClientSDKGDScript::writeEntityDefsModuleInitDefTypesBegin()
{
	generatedDefTypeIDs_.clear();
	sourcefileBody_ += "\nstatic func _initDefType(utype:int, typeName:String, name:String)-> void:\n\tEntityDef.datatypes[typeName] = EntityDef.datatypes.get(name)\n\tEntityDef.id2datatypes[utype] = EntityDef.datatypes[typeName]\n\tEntityDef.datatype2id[typeName] = utype\n\n";
	sourcefileBody_ += "static func initDefTypes()-> void:\n";
	return true;
}

bool ClientSDKGDScript::writeEntityDefsModuleInitDefType(const DataType* pDataType)
{
	return writeEntityDefsModuleInitDefTypeRecursive(pDataType);
}

bool ClientSDKGDScript::writeEntityDefsModuleInitDefTypeRecursive(const DataType* pDataType)
{
	std::string typeName = pDataType->aliasName();
	if (typeName.empty())
		typeName = pDataType->getName();
	if (strlen(pDataType->aliasName()) == 0 && (pDataType->type() == DATA_TYPE_FIXEDARRAY || pDataType->type() == DATA_TYPE_FIXEDDICT))
		typeName = gdDefTypeVarName(pDataType);

	uint16 typeID = pDataType->id();
	if (typeID > 0 && generatedDefTypeIDs_.find(typeID) != generatedDefTypeIDs_.end())
		return true;

	if (pDataType->type() == DATA_TYPE_FIXEDARRAY)
	{
		const FixedArrayType* pArray = static_cast<const FixedArrayType*>(pDataType);
		DataType* pChildDataType = const_cast<FixedArrayType*>(pArray)->getDataType();
		if (pChildDataType->type() == DATA_TYPE_FIXEDARRAY || pChildDataType->type() == DATA_TYPE_FIXEDDICT)
		{
			if (!writeEntityDefsModuleInitDefTypeRecursive(pChildDataType))
				return false;
		}

		std::string varName = gdDefTypeVarName(pDataType);
		sourcefileBody_ += fmt::format("\tvar dt_{}:DataTypes.DATATYPE_ARRAY = DataTypes.DATATYPE_ARRAY.new()\n", varName);
		sourcefileBody_ += fmt::format("\tdt_{}.type_id = {}\n", varName, dataTypeID(pChildDataType));
		sourcefileBody_ += fmt::format("\tEntityDef.datatypes[\"{}\"] = dt_{}\n\tEntityDef.id2datatypes[{}] = dt_{}\n\tEntityDef.datatype2id[\"{}\"] = {}\n", typeName, varName, pDataType->id(), varName, typeName, pDataType->id());
	}
	else if (pDataType->type() == DATA_TYPE_FIXEDDICT)
	{
		const FixedDictType* pDict = static_cast<const FixedDictType*>(pDataType);
		FixedDictType::FIXEDDICT_KEYTYPE_MAP& keyTypes = const_cast<FixedDictType*>(pDict)->getKeyTypes();
		for (FixedDictType::FIXEDDICT_KEYTYPE_MAP::iterator it = keyTypes.begin(); it != keyTypes.end(); ++it)
		{
			DataType* pChildDataType = it->second->dataType;
			if (pChildDataType->type() == DATA_TYPE_FIXEDARRAY || pChildDataType->type() == DATA_TYPE_FIXEDDICT)
			{
				if (!writeEntityDefsModuleInitDefTypeRecursive(pChildDataType))
					return false;
			}
		}

		std::string varName = gdDefTypeVarName(pDataType);
		sourcefileBody_ += fmt::format("\tvar dt_{}:DataTypes.DATATYPE_FIXED_DICT = DataTypes.DATATYPE_FIXED_DICT.new()\n", varName);
		for (FixedDictType::FIXEDDICT_KEYTYPE_MAP::iterator it = keyTypes.begin(); it != keyTypes.end(); ++it)
			sourcefileBody_ += fmt::format("\tdt_{}.dicttype[\"{}\"] = {}\n", varName, it->first, dataTypeID(it->second->dataType));
		sourcefileBody_ += fmt::format("\tEntityDef.datatypes[\"{}\"] = dt_{}\n\tEntityDef.id2datatypes[{}] = dt_{}\n\tEntityDef.datatype2id[\"{}\"] = {}\n", typeName, varName, pDataType->id(), varName, typeName, pDataType->id());
	}
	else
	{
		sourcefileBody_ += fmt::format("\t_initDefType({}, \"{}\", \"{}\")\n", pDataType->id(), typeName, pDataType->getName());
	}

	if (typeID > 0)
		generatedDefTypeIDs_.insert(typeID);

	return true;
}

bool ClientSDKGDScript::writeEntityDefsModuleInitDefTypesEnd()
{
	sourcefileBody_ += "\n\tfor datatypeStr:String in EntityDef.datatypes.keys():\n\t\tvar dataType:DataTypes.DATATYPE_BASE = EntityDef.datatypes.get(datatypeStr)\n\t\tif dataType:\n\t\t\tdataType.bind()\n";
	return true;
}

bool ClientSDKGDScript::writeEntityCallBegin(ScriptDefModule* pScriptDefModule)
{
	return true;
}

bool ClientSDKGDScript::writeEntityCallEnd(ScriptDefModule* pScriptDefModule)
{
	return true;
}

bool ClientSDKGDScript::writeBaseEntityCallBegin(ScriptDefModule* pScriptDefModule)
{
	sourcefileBody_ += fmt::format("class EntityBaseEntityCall_{}Base extends EntityCall:\n\n", pScriptDefModule->getName());
	if (pScriptDefModule->isComponentModule())
		sourcefileBody_ += fmt::format("\tfunc _init(_entitycomponentPropertyID:int, _ownerID:int):\n\t\tsuper._init(_ownerID, \"{}\", ENTITYCALL_TYPE.ENTITYCALL_TYPE_BASE)\n\t\tself.entitycomponentPropertyID = _entitycomponentPropertyID\n", pScriptDefModule->getName());
	else
		sourcefileBody_ += "\tfunc _init(_eid: int, _ename: String):\n\t\tsuper._init(_eid, _ename, ENTITYCALL_TYPE.ENTITYCALL_TYPE_BASE)\n";
	return true;
}

bool ClientSDKGDScript::writeBaseEntityCallEnd(ScriptDefModule* pScriptDefModule)
{
	sourcefileBody_ += "\n";
	return true;
}

bool ClientSDKGDScript::writeCellEntityCallBegin(ScriptDefModule* pScriptDefModule)
{
	sourcefileBody_ += fmt::format("class EntityCellEntityCall_{}Base extends EntityCall:\n\n", pScriptDefModule->getName());
	if (pScriptDefModule->isComponentModule())
		sourcefileBody_ += fmt::format("\tfunc _init(_entitycomponentPropertyID:int, _ownerID:int):\n\t\tsuper._init(_ownerID, \"{}\", ENTITYCALL_TYPE.ENTITYCALL_TYPE_CELL)\n\t\tself.entitycomponentPropertyID = _entitycomponentPropertyID\n", pScriptDefModule->getName());
	else
		sourcefileBody_ += "\tfunc _init(_eid: int, _ename: String):\n\t\tsuper._init(_eid, _ename, ENTITYCALL_TYPE.ENTITYCALL_TYPE_CELL)\n";
	return true;
}

bool ClientSDKGDScript::writeCellEntityCallEnd(ScriptDefModule* pScriptDefModule)
{
	sourcefileBody_ += "\n";
	return true;
}

bool ClientSDKGDScript::writeEntityCallMethodBegin(ScriptDefModule* pScriptDefModule, MethodDescription* pMethodDescription, const char* fillString1, const char* fillString2, COMPONENT_TYPE componentType)
{
	sourcefileBody_ += fmt::format("\n\tfunc {}({})-> void:\n", pMethodDescription->getName(), fillString1);
	sourcefileBody_ += fmt::format("\t\tvar bundle:Bundle = newCallByName(\"{}\", self.entitycomponentPropertyID)\n\t\tif not bundle:\n\t\t\treturn\n", pMethodDescription->getName());
	sourcefileBody_ += fmt::format("\t\tvar method:Method = EntityDef.moduledefs[self.className].{}[\"{}\"]\n", componentType == BASEAPP_TYPE ? "base_methods" : "cell_methods", pMethodDescription->getName());
	return true;
}

bool ClientSDKGDScript::writeEntityCallMethodEnd(ScriptDefModule* pScriptDefModule, MethodDescription* pMethodDescription)
{
	const std::vector<DataType*>& args = pMethodDescription->getArgTypes();
	for (size_t i = 0; i < args.size(); ++i)
		sourcefileBody_ += fmt::format("\t\tmethod.args[{}].addToStream(bundle, arg{})\n", i, i + 1);
	sourcefileBody_ += "\t\tsendCall(bundle)\n";
	return true;
}

bool ClientSDKGDScript::writeEntityModuleBegin(ScriptDefModule* pEntityScriptDefModule)
{
	currSourcePath_ = basepath_ + (pEntityScriptDefModule->isComponentModule() ? "Components/" : "Entities/");
	sourcefileName_ = std::string(pEntityScriptDefModule->getName()) + "Base.gd";
	sourcefileBody_ = fmt::format("class_name {}Base extends {}\n\n", pEntityScriptDefModule->getName(), pEntityScriptDefModule->isComponentModule() ? "EntityComponent" : "Entity");
	if (pEntityScriptDefModule->isComponentModule())
	{
		sourcefileBody_ += fmt::format("var baseEntityCall:EntityBaseEntityCall_{}Base = null\n", pEntityScriptDefModule->getName());
		sourcefileBody_ += fmt::format("var cellEntityCall:EntityCellEntityCall_{}Base = null\n\n", pEntityScriptDefModule->getName());
	}
	return writeEntityCall(pEntityScriptDefModule);
}

bool ClientSDKGDScript::writeEntityModuleEnd(ScriptDefModule* pEntityScriptDefModule)
{
	return true;
}

bool ClientSDKGDScript::writeEntityPropertyComponent(ScriptDefModule*, ScriptDefModule*, PropertyDescription* pPropertyDescription)
{
	EntityComponentType* pEntityComponentType = (EntityComponentType*)pPropertyDescription->getDataType();
	sourcefileBody_ += fmt::format("var {}:{}Base = null\n", pPropertyDescription->getName(), pEntityComponentType->pScriptDefModule()->getName());
	sourcefileBody_ += fmt::format("func on{}Changed(oldValue:{}Base)-> void:\n\tpass\n", cap(pPropertyDescription->getName()), pEntityComponentType->pScriptDefModule()->getName());
	return true;
}

bool ClientSDKGDScript::writeEntityPropertyCommon(PropertyDescription* pPropertyDescription)
{
	std::string type = pPropertyDescription->getDataType()->getName();
	sourcefileBody_ += fmt::format("var {}:{} = {}\n", pPropertyDescription->getName(), gdType(type), defaultValue(pPropertyDescription));
	sourcefileBody_ += fmt::format("func on{}Changed(oldValue:{})-> void:\n\tpass\n", cap(pPropertyDescription->getName()), gdType(type));
	return true;
}

#define KBE_GDSCRIPT_PROPERTY_IMPL(TYPE) bool ClientSDKGDScript::writeEntityProperty_##TYPE(ScriptDefModule*, ScriptDefModule*, PropertyDescription* p) { return writeEntityPropertyCommon(p); }
KBE_GDSCRIPT_PROPERTY_IMPL(INT8)
KBE_GDSCRIPT_PROPERTY_IMPL(INT16)
KBE_GDSCRIPT_PROPERTY_IMPL(INT32)
KBE_GDSCRIPT_PROPERTY_IMPL(INT64)
KBE_GDSCRIPT_PROPERTY_IMPL(UINT8)
KBE_GDSCRIPT_PROPERTY_IMPL(UINT16)
KBE_GDSCRIPT_PROPERTY_IMPL(UINT32)
KBE_GDSCRIPT_PROPERTY_IMPL(UINT64)
KBE_GDSCRIPT_PROPERTY_IMPL(FLOAT)
KBE_GDSCRIPT_PROPERTY_IMPL(DOUBLE)
KBE_GDSCRIPT_PROPERTY_IMPL(STRING)
KBE_GDSCRIPT_PROPERTY_IMPL(UNICODE)
KBE_GDSCRIPT_PROPERTY_IMPL(PYTHON)
KBE_GDSCRIPT_PROPERTY_IMPL(PY_DICT)
KBE_GDSCRIPT_PROPERTY_IMPL(PY_TUPLE)
KBE_GDSCRIPT_PROPERTY_IMPL(PY_LIST)
KBE_GDSCRIPT_PROPERTY_IMPL(BLOB)
KBE_GDSCRIPT_PROPERTY_IMPL(ARRAY)
KBE_GDSCRIPT_PROPERTY_IMPL(FIXED_DICT)
KBE_GDSCRIPT_PROPERTY_IMPL(VECTOR2)
KBE_GDSCRIPT_PROPERTY_IMPL(VECTOR3)
KBE_GDSCRIPT_PROPERTY_IMPL(VECTOR4)
KBE_GDSCRIPT_PROPERTY_IMPL(ENTITYCALL)
#undef KBE_GDSCRIPT_PROPERTY_IMPL

bool ClientSDKGDScript::writeEntityProcessMessagesMethod(ScriptDefModule* pEntityScriptDefModule)
{
	ScriptDefModule::PROPERTYDESCRIPTION_MAP& props = pEntityScriptDefModule->getClientPropertyDescriptions();
	ScriptDefModule::METHODDESCRIPTION_MAP& methods = pEntityScriptDefModule->getClientMethodDescriptions();

	if (!pEntityScriptDefModule->isComponentModule())
	{
		sourcefileBody_ += fmt::format("var baseEntityCall:EntityBaseEntityCall_{}Base = null\n", pEntityScriptDefModule->getName());
		sourcefileBody_ += fmt::format("var cellEntityCall:EntityCellEntityCall_{}Base = null\n\n", pEntityScriptDefModule->getName());
		sourcefileBody_ += "\nfunc _init():\n\tsuper._init()\n\n";
		for (ScriptDefModule::PROPERTYDESCRIPTION_MAP::iterator it = props.begin(); it != props.end(); ++it)
		{
			PropertyDescription* p = it->second;
			if (p->getDataType()->type() != DATA_TYPE_ENTITY_COMPONENT)
				continue;
			EntityComponentType* pEntityComponentType = (EntityComponentType*)p->getDataType();
			sourcefileBody_ += fmt::format("\tvar {}_script:Script = EntityDef.moduledefs[\"{}\"].entityScript\n", p->getName(), pEntityComponentType->pScriptDefModule()->getName());
			sourcefileBody_ += fmt::format("\tif {}_script == null:\n\t\tDbg.ERROR_MSG('Please inherit and implement, such as: \"class_name {} extends {}Base\"')\n\t\treturn\n", p->getName(), pEntityComponentType->pScriptDefModule()->getName(), pEntityComponentType->pScriptDefModule()->getName());
			sourcefileBody_ += fmt::format("\t{} = {}_script.new()\n\t{}.owner = self\n\t{}.entityComponentPropertyID = {}\n\t{}.name_ = \"{}\"\n", p->getName(), p->getName(), p->getName(), p->getName(), p->getUType(), p->getName(), pEntityComponentType->pScriptDefModule()->getName());
		}
		sourcefileBody_ += "\nfunc onComponentsEnterworld()-> void:\n";
		bool hasComponents = false;
		for (ScriptDefModule::PROPERTYDESCRIPTION_MAP::iterator it = props.begin(); it != props.end(); ++it)
		{
			PropertyDescription* p = it->second;
			if (p->getDataType()->type() != DATA_TYPE_ENTITY_COMPONENT)
				continue;
			hasComponents = true;
			sourcefileBody_ += fmt::format("\t{}.onEnterworld()\n", p->getName());
		}
		if (!hasComponents)
			sourcefileBody_ += "\tpass\n";
		sourcefileBody_ += "\nfunc onComponentsLeaveworld()-> void:\n";
		if (hasComponents)
		{
			for (ScriptDefModule::PROPERTYDESCRIPTION_MAP::iterator it = props.begin(); it != props.end(); ++it)
			{
				PropertyDescription* p = it->second;
				if (p->getDataType()->type() == DATA_TYPE_ENTITY_COMPONENT)
					sourcefileBody_ += fmt::format("\t{}.onLeaveworld()\n", p->getName());
			}
		}
		else
		{
			sourcefileBody_ += "\tpass\n";
		}
		sourcefileBody_ += "\n";
		if (hasComponents)
		{
			sourcefileBody_ += "func getComponents(_componentName: String, _all: bool) -> Array[EntityComponent]:\n\tvar founds:Array[EntityComponent] = []\n";
			for (ScriptDefModule::PROPERTYDESCRIPTION_MAP::iterator it = props.begin(); it != props.end(); ++it)
			{
				PropertyDescription* p = it->second;
				if (p->getDataType()->type() != DATA_TYPE_ENTITY_COMPONENT)
					continue;
				sourcefileBody_ += fmt::format("\tif {}.name_ == _componentName:\n\t\tfounds.append({})\n\t\tif not _all:\n\t\t\treturn founds\n", p->getName(), p->getName());
			}
			sourcefileBody_ += "\treturn founds\n\n";
		}
		sourcefileBody_ += fmt::format("func onGetBase()-> void:\n\tself.baseEntityCall = EntityBaseEntityCall_{}Base.new(self.id, self.className)\n", pEntityScriptDefModule->getName());
		if (hasComponents)
		{
			for (ScriptDefModule::PROPERTYDESCRIPTION_MAP::iterator it = props.begin(); it != props.end(); ++it)
			{
				PropertyDescription* p = it->second;
				if (p->getDataType()->type() == DATA_TYPE_ENTITY_COMPONENT)
					sourcefileBody_ += fmt::format("\t{}.onGetBase()\n", p->getName());
			}
		}
		sourcefileBody_ += "\n";
		sourcefileBody_ += fmt::format("func onGetCell()-> void:\n\tself.cellEntityCall = EntityCellEntityCall_{}Base.new(self.id, self.className)\n", pEntityScriptDefModule->getName());
		if (hasComponents)
		{
			for (ScriptDefModule::PROPERTYDESCRIPTION_MAP::iterator it = props.begin(); it != props.end(); ++it)
			{
				PropertyDescription* p = it->second;
				if (p->getDataType()->type() == DATA_TYPE_ENTITY_COMPONENT)
					sourcefileBody_ += fmt::format("\t{}.onGetCell()\n", p->getName());
			}
		}
		sourcefileBody_ += "\n";
		sourcefileBody_ += "func onLoseCell()-> void:\n\tself.cellEntityCall = null\n";
		if (hasComponents)
		{
			for (ScriptDefModule::PROPERTYDESCRIPTION_MAP::iterator it = props.begin(); it != props.end(); ++it)
			{
				PropertyDescription* p = it->second;
				if (p->getDataType()->type() == DATA_TYPE_ENTITY_COMPONENT)
					sourcefileBody_ += fmt::format("\t{}.onLoseCell()\n", p->getName());
			}
		}
		sourcefileBody_ += "\nfunc getBaseEntityCall()-> EntityCall:\n\treturn baseEntityCall\n\n";
		sourcefileBody_ += "func getCellEntityCall()-> EntityCall:\n\treturn cellEntityCall\n";
		sourcefileBody_ += "\nfunc attachComponents()-> void:\n";
		if (hasComponents)
		{
			for (ScriptDefModule::PROPERTYDESCRIPTION_MAP::iterator it = props.begin(); it != props.end(); ++it)
			{
				PropertyDescription* p = it->second;
				if (p->getDataType()->type() == DATA_TYPE_ENTITY_COMPONENT)
					sourcefileBody_ += fmt::format("\t{}.onAttached(self)\n", p->getName());
			}
		}
		else
		{
			sourcefileBody_ += "\tpass\n";
		}
		sourcefileBody_ += "\nfunc detachComponents()-> void:\n";
		if (hasComponents)
		{
			// 分离回调必须先观察有效宿主，再解除 RefCounted 双向强引用以保证实体可以确定性释放。
			// The detach callback must observe a valid owner before breaking the bidirectional RefCounted references for deterministic release.
			sourcefileBody_ += "\t# 分离回调完成后解除组件对宿主的强引用，避免实体与组件形成无法回收的引用环。\n";
			sourcefileBody_ += "\t# Break the component's strong owner reference after detachment to avoid an unreclaimable entity-component cycle.\n";
			for (ScriptDefModule::PROPERTYDESCRIPTION_MAP::iterator it = props.begin(); it != props.end(); ++it)
			{
				PropertyDescription* p = it->second;
				if (p->getDataType()->type() == DATA_TYPE_ENTITY_COMPONENT)
					sourcefileBody_ += fmt::format("\t{}.onDetached(self)\n\t{}.owner = null\n", p->getName(), p->getName());
			}
		}
		else
		{
			sourcefileBody_ += "\tpass\n";
		}
		sourcefileBody_ += "\n";
	}
	else
	{
		sourcefileBody_ += "\nfunc onGetBase()-> void:\n\townerID = owner.id\n\tbaseEntityCall = EntityBaseEntityCall_";
		sourcefileBody_ += pEntityScriptDefModule->getName();
		sourcefileBody_ += "Base.new(entityComponentPropertyID, ownerID)\n\n";
		sourcefileBody_ += "func onGetCell()-> void:\n\townerID = owner.id\n\tcellEntityCall = EntityCellEntityCall_";
		sourcefileBody_ += pEntityScriptDefModule->getName();
		sourcefileBody_ += "Base.new(entityComponentPropertyID, ownerID)\n\n";
		sourcefileBody_ += "func onLoseCell()-> void:\n\tcellEntityCall = null\n\n";
		sourcefileBody_ += fmt::format("func getScriptModule()-> ScriptModule:\n\treturn EntityDef.moduledefs[\"{}\"]\n\n", pEntityScriptDefModule->getName());
	}

	if (pEntityScriptDefModule->isComponentModule())
		sourcefileBody_ += "func onRemoteMethodCall(_methodUtype:int, _stream:MemoryStream)-> void:\n";
	else
		sourcefileBody_ += "func onRemoteMethodCall(_stream:MemoryStream)-> void:\n";
	sourcefileBody_ += fmt::format("\tvar sm:ScriptModule = EntityDef.moduledefs[\"{}\"]\n", pEntityScriptDefModule->getName());
	if (pEntityScriptDefModule->isComponentModule())
	{
		sourcefileBody_ += "\tvar method:Method = null\n\tif not sm.idmethods.has(_methodUtype):\n\t\tDbg.ERROR_MSG(\"unknown method utype \" + str(_methodUtype))\n\t\treturn\n\tmethod = sm.idmethods[_methodUtype]\n\tmatch (method.methodUtype):\n";
	}
	else
	{
		sourcefileBody_ += "\tvar componentPropertyUType:int = _stream.readUint8() if sm.usePropertyDescrAlias else _stream.readUint16()\n\tvar methodUtype:int = _stream.readUint8() if sm.useMethodDescrAlias else _stream.readUint16()\n\tvar method:Method = null\n";
		sourcefileBody_ += "\tif componentPropertyUType == 0:\n\t\tif not sm.idmethods.has(methodUtype):\n\t\t\tDbg.ERROR_MSG(\"unknown method utype \" + str(methodUtype))\n\t\t\treturn\n\t\tmethod = sm.idmethods[methodUtype]\n\t\tmatch (method.methodUtype):\n";
	}

	for (ScriptDefModule::METHODDESCRIPTION_MAP::iterator it = methods.begin(); it != methods.end(); ++it)
	{
		MethodDescription* pMethodDescription = it->second;
		sourcefileBody_ += fmt::format("{}{}:\n", pEntityScriptDefModule->isComponentModule() ? "\t\t" : "\t\t\t", pMethodDescription->getUType());
		std::string args;
		for (size_t i = 0; i < pMethodDescription->getArgTypes().size(); ++i)
		{
			sourcefileBody_ += fmt::format("{}var arg{} = method.args[{}].createFromStream(_stream)\n", pEntityScriptDefModule->isComponentModule() ? "\t\t\t" : "\t\t\t\t", i + 1, i);
			args += fmt::format("arg{}, ", i + 1);
		}
		if (!args.empty())
			args.erase(args.size() - 2, 2);
		sourcefileBody_ += fmt::format("{}{}({})\n", pEntityScriptDefModule->isComponentModule() ? "\t\t\t" : "\t\t\t\t", pMethodDescription->getName(), args);
	}
	if (pEntityScriptDefModule->isComponentModule())
	{
		sourcefileBody_ += "\t\t_:\n\t\t\tpass\n\n";
	}
	else
	{
		sourcefileBody_ += "\t\t\t_:\n\t\t\t\tpass\n\telse:\n\t\tvar componentProp:Property = sm.idpropertys.get(componentPropertyUType, null)\n\t\tif componentProp == null:\n\t\t\tDbg.ERROR_MSG(\"unknown component property utype \" + str(componentPropertyUType))\n\t\t\treturn\n\t\tmatch (componentProp.properUtype):\n";
		for (ScriptDefModule::PROPERTYDESCRIPTION_MAP::iterator it = props.begin(); it != props.end(); ++it)
		{
			PropertyDescription* p = it->second;
			if (p->getDataType()->type() != DATA_TYPE_ENTITY_COMPONENT)
				continue;
			sourcefileBody_ += fmt::format("\t\t\t{}:\n\t\t\t\t{}.onRemoteMethodCall(methodUtype, _stream)\n", p->getUType(), p->getName());
		}
		sourcefileBody_ += "\t\t\t_:\n\t\t\t\tpass\n\n";
	}

	if (pEntityScriptDefModule->isComponentModule())
		sourcefileBody_ += "func onUpdatePropertys(_propUtype:int, _stream:MemoryStream, _maxCount:int)-> void:\n";
	else
		sourcefileBody_ += "func onUpdatePropertys(_stream:MemoryStream)-> void:\n";
	sourcefileBody_ += fmt::format("\tvar sm:ScriptModule = EntityDef.moduledefs[\"{}\"]\n", pEntityScriptDefModule->getName());
	if (pEntityScriptDefModule->isComponentModule())
	{
		sourcefileBody_ += "\tvar pdatas:Dictionary[int, Property] = sm.idpropertys\n\twhile _stream.size() > 0 and _maxCount != 0:\n\t\tif _maxCount > 0:\n\t\t\t_maxCount -= 1\n\t\tvar _t_child_utype:int = _propUtype\n\t\tif _t_child_utype == 0:\n\t\t\tvar _t_utype:int = _stream.readUint8() if sm.usePropertyDescrAlias else _stream.readUint16()\n\t\t\t_t_child_utype = _stream.readUint8() if sm.usePropertyDescrAlias else _stream.readUint16()\n\t\tvar prop:Property = null\n\t\tif not pdatas.has(_t_child_utype):\n\t\t\tDbg.ERROR_MSG(\"unknown property utype \" + str(_t_child_utype))\n\t\t\tbreak\n\t\tprop = pdatas[_t_child_utype]\n\t\tmatch (prop.properUtype):\n";
	}
	else
	{
		sourcefileBody_ += "\tvar pdatas:Dictionary[int, Property] = sm.idpropertys\n\twhile _stream.size() > 0:\n\t\tvar _t_utype:int = _stream.readUint8() if sm.usePropertyDescrAlias else _stream.readUint16()\n\t\tvar _t_child_utype:int = _stream.readUint8() if sm.usePropertyDescrAlias else _stream.readUint16()\n\t\tvar prop:Property = null\n\t\tif _t_utype == 0:\n\t\t\tif not pdatas.has(_t_child_utype):\n\t\t\t\tDbg.ERROR_MSG(\"unknown property utype \" + str(_t_child_utype))\n\t\t\t\tbreak\n\t\t\tprop = pdatas[_t_child_utype]\n\t\telif pdatas.has(_t_utype):\n\t\t\tprop = pdatas[_t_utype]\n\t\t\tmatch (prop.properUtype):\n";
		for (ScriptDefModule::PROPERTYDESCRIPTION_MAP::iterator it = props.begin(); it != props.end(); ++it)
		{
			PropertyDescription* p = it->second;
			if (p->getDataType()->type() != DATA_TYPE_ENTITY_COMPONENT)
				continue;
			sourcefileBody_ += fmt::format("\t\t\t\t{}:\n\t\t\t\t\t{}.onUpdatePropertys(_t_child_utype, _stream, -1)\n\t\t\t\t\tcontinue\n", p->getUType(), p->getName());
		}
		sourcefileBody_ += "\t\t\t\t_:\n\t\t\t\t\t_stream.rpos -= 1 if sm.usePropertyDescrAlias else 2\n\t\telse:\n\t\t\tDbg.ERROR_MSG(\"unknown property utype \" + str(_t_utype))\n\t\t\tbreak\n\t\tmatch (prop.properUtype):\n";
		sourcefileBody_ += "\t\t\t40000:\n\t\t\t\tvar oldval_position = position\n\t\t\t\tposition = _stream.readVector3()\n\t\t\t\tonPositionChanged(oldval_position)\n\t\t\t40001:\n\t\t\t\tvar oldval_direction = direction\n\t\t\t\tdirection = _stream.readVector3()\n\t\t\t\tonDirectionChanged(oldval_direction)\n\t\t\t40002:\n\t\t\t\tvar oldval_spaceID = spaceID\n\t\t\t\tspaceID = _stream.readUint32()\n\t\t\t\tonSpaceIDChanged(oldval_spaceID)\n";
	}

	for (ScriptDefModule::PROPERTYDESCRIPTION_MAP::iterator it = props.begin(); it != props.end(); ++it)
	{
		PropertyDescription* p = it->second;
		if (!pEntityScriptDefModule->isComponentModule() && (p->getUType() == 40000 || p->getUType() == 40001 || p->getUType() == 40002))
			continue;
		sourcefileBody_ += fmt::format("\t\t\t{}:\n", p->getUType());
		if (p->getDataType()->type() == DATA_TYPE_ENTITY_COMPONENT)
		{
			sourcefileBody_ += fmt::format("\t\t\t\t{}.createFromStream(_stream)\n", p->getName());
		}
		else
		{
			sourcefileBody_ += fmt::format("\t\t\t\tvar oldval_{} = {}\n", p->getName(), p->getName());
			std::string fn = readFunc(p->getDataType()->getName());
			if (fn.empty())
				sourcefileBody_ += fmt::format("\t\t\t\t{} = prop.defaultVal\n", p->getName());
			else
				sourcefileBody_ += fmt::format("\t\t\t\t{} = _stream.{}()\n", p->getName(), fn);
			sourcefileBody_ += fmt::format("\t\t\t\ton{}Changed(oldval_{})\n", cap(p->getName()), p->getName());
		}
	}
	sourcefileBody_ += "\t\t\t_:\n\t\t\t\tpass\n\n";

	sourcefileBody_ += "func callPropertysSetMethods()-> void:\n";
	for (ScriptDefModule::PROPERTYDESCRIPTION_MAP::iterator it = props.begin(); it != props.end(); ++it)
	{
		PropertyDescription* p = it->second;
		if (p->getDataType()->type() == DATA_TYPE_ENTITY_COMPONENT)
			continue;
		sourcefileBody_ += fmt::format("\tvar oldval_{} = {}\n\ton{}Changed(oldval_{})\n", p->getName(), p->getName(), cap(p->getName()), p->getName());
	}
	if (!pEntityScriptDefModule->isComponentModule())
	{
		for (ScriptDefModule::PROPERTYDESCRIPTION_MAP::iterator it = props.begin(); it != props.end(); ++it)
		{
			PropertyDescription* p = it->second;
			if (p->getDataType()->type() == DATA_TYPE_ENTITY_COMPONENT)
				sourcefileBody_ += fmt::format("\t{}.callPropertysSetMethods()\n", p->getName());
		}
	}
	sourcefileBody_ += "\tpass\n";
	return true;
}

bool ClientSDKGDScript::writeEntityMethods(ScriptDefModule* pEntityScriptDefModule,
	ScriptDefModule* pCurrScriptDefModule)
{
	sourcefileBody_ += "\n";

	ScriptDefModule::METHODDESCRIPTION_MAP& clientMethods = pCurrScriptDefModule->getClientMethodDescriptions();
	ScriptDefModule::METHODDESCRIPTION_MAP::iterator methodIter = clientMethods.begin();
	for (; methodIter != clientMethods.end(); ++methodIter)
	{
		if (!writeEntityMethod(pEntityScriptDefModule, pCurrScriptDefModule, methodIter->second, ""))
			return false;
	}

	return true;
}

bool ClientSDKGDScript::writeEntityMethod(ScriptDefModule*, ScriptDefModule*, MethodDescription* pMethodDescription, const char*)
{
	std::string args;
	std::vector<DataType*>& argTypes = pMethodDescription->getArgTypes();
	for (size_t i = 0; i < argTypes.size(); ++i)
	{
		DataType* pDataType = argTypes[i];
		std::string argType;
		if (pDataType->type() == DATA_TYPE_FIXEDARRAY)
			argType = "Array";
		else if (pDataType->type() == DATA_TYPE_FIXEDDICT)
			argType = "Dictionary";
		else
			argType = gdType(pDataType->getName());
		args += fmt::format("arg{}:{}, ", i + 1, argType);
	}
	if (!args.empty())
		args.erase(args.size() - 2, 2);
	sourcefileBody_ += fmt::format("\nfunc {}({})-> void:\n\tpass\n", pMethodDescription->getName(), args);
	return true;
}

bool ClientSDKGDScript::writeEntityMethodArgs_ARRAY(FixedArrayType*, std::string& stackArgsTypeBody, const std::string&)
{
	stackArgsTypeBody += "Array";
	return true;
}

bool ClientSDKGDScript::writeEntityMethodArgs_Const_Ref(DataType*, std::string&)
{
	return true;
}

}
