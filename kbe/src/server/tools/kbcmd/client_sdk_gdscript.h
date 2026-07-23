// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

#ifndef KBE_CLIENT_SDK_GDSCRIPT_H
#define KBE_CLIENT_SDK_GDSCRIPT_H

#include "client_sdk.h"
#include <set>

namespace KBEngine{

class ClientSDKGDScript : public ClientSDK
{
public:
	ClientSDKGDScript();
	virtual ~ClientSDKGDScript();

	virtual std::string name() const { return "gdscript"; }

	virtual bool create(const std::string& path);
	virtual bool writeTypes();
	virtual bool writeCustomDataTypes();
	virtual bool writeEntityCall(ScriptDefModule* pScriptDefModule);

	virtual void onCreateEntityModuleFileName(const std::string& moduleName);
	virtual void onCreateServerErrorDescrsModuleFileName();
	virtual void onCreateEngineMessagesModuleFileName();
	virtual void onCreateEntityDefsModuleFileName();
	virtual void onEntityCallModuleFileName(const std::string& moduleName);

	virtual bool writeServerErrorDescrsModuleBegin();
	virtual bool writeServerErrorDescrsModuleErrDescr(int errorID, const std::string& errname, const std::string& errdescr);
	virtual bool writeServerErrorDescrsModuleEnd();

	virtual bool writeEngineMessagesModuleBegin();
	virtual bool writeEngineMessagesModuleMessage(Network::ExposedMessageInfo& messageInfos, COMPONENT_TYPE componentType);
	virtual bool writeEngineMessagesModuleEnd();

	virtual bool writeEntityDefsModuleBegin();
	virtual bool writeEntityDefsModuleEnd();
	virtual bool writeEntityDefsModuleInitScriptBegin();
	virtual bool writeEntityDefsModuleInitScriptEnd();
	virtual bool writeEntityDefsModuleInitScript_ScriptModule(ScriptDefModule* pScriptDefModule);
	virtual bool writeEntityDefsModuleInitScript_MethodDescr(ScriptDefModule* pScriptDefModule, MethodDescription* pDescr, COMPONENT_TYPE componentType);
	virtual bool writeEntityDefsModuleInitScript_PropertyDescr(ScriptDefModule* pScriptDefModule, PropertyDescription* pDescr);
	virtual bool writeEntityDefsModuleInitDefTypesBegin();
	virtual bool writeEntityDefsModuleInitDefTypesEnd();
	virtual bool writeEntityDefsModuleInitDefType(const DataType* pDataType);
	virtual bool writeCustomDataTypesBegin();
	virtual bool writeCustomDataTypesEnd();
	virtual bool writeCustomDataType(const DataType* pDataType);

	virtual bool writeEntityCallBegin(ScriptDefModule* pScriptDefModule);
	virtual bool writeEntityCallEnd(ScriptDefModule* pScriptDefModule);
	virtual bool writeBaseEntityCallBegin(ScriptDefModule* pScriptDefModule);
	virtual bool writeBaseEntityCallEnd(ScriptDefModule* pScriptDefModule);
	virtual bool writeCellEntityCallBegin(ScriptDefModule* pScriptDefModule);
	virtual bool writeCellEntityCallEnd(ScriptDefModule* pScriptDefModule);
	virtual bool writeEntityCallMethodBegin(ScriptDefModule* pScriptDefModule, MethodDescription* pMethodDescription,
		const char* fillString1, const char* fillString2, COMPONENT_TYPE componentType);
	virtual bool writeEntityCallMethodEnd(ScriptDefModule* pScriptDefModule, MethodDescription* pMethodDescription);

	virtual std::string typeToType(const std::string& type);
	virtual bool writeEntityModuleBegin(ScriptDefModule* pEntityScriptDefModule);
	virtual bool writeEntityModuleEnd(ScriptDefModule* pEntityScriptDefModule);
	virtual bool writeEntityPropertyComponent(ScriptDefModule* pEntityScriptDefModule,
		ScriptDefModule* pCurrScriptDefModule, PropertyDescription* pPropertyDescription);
	virtual bool writeEntityProcessMessagesMethod(ScriptDefModule* pEntityScriptDefModule);
	virtual bool writeEntityMethods(ScriptDefModule* pEntityScriptDefModule,
		ScriptDefModule* pCurrScriptDefModule);
	virtual bool writeEntityMethod(ScriptDefModule* pEntityScriptDefModule,
		ScriptDefModule* pCurrScriptDefModule, MethodDescription* pMethodDescription, const char* fillString);
	virtual bool writeEntityMethodArgs_ARRAY(FixedArrayType* pFixedArrayType, std::string& stackArgsTypeBody, const std::string& childItemName);
	virtual bool writeEntityMethodArgs_Const_Ref(DataType* pDataType, std::string& stackArgsTypeBody);

#define KBE_GDSCRIPT_PROPERTY_DECL(TYPE) virtual bool writeEntityProperty_##TYPE(ScriptDefModule*, ScriptDefModule*, PropertyDescription*);
	KBE_GDSCRIPT_PROPERTY_DECL(INT8)
	KBE_GDSCRIPT_PROPERTY_DECL(INT16)
	KBE_GDSCRIPT_PROPERTY_DECL(INT32)
	KBE_GDSCRIPT_PROPERTY_DECL(INT64)
	KBE_GDSCRIPT_PROPERTY_DECL(UINT8)
	KBE_GDSCRIPT_PROPERTY_DECL(UINT16)
	KBE_GDSCRIPT_PROPERTY_DECL(UINT32)
	KBE_GDSCRIPT_PROPERTY_DECL(UINT64)
	KBE_GDSCRIPT_PROPERTY_DECL(FLOAT)
	KBE_GDSCRIPT_PROPERTY_DECL(DOUBLE)
	KBE_GDSCRIPT_PROPERTY_DECL(STRING)
	KBE_GDSCRIPT_PROPERTY_DECL(UNICODE)
	KBE_GDSCRIPT_PROPERTY_DECL(PYTHON)
	KBE_GDSCRIPT_PROPERTY_DECL(PY_DICT)
	KBE_GDSCRIPT_PROPERTY_DECL(PY_TUPLE)
	KBE_GDSCRIPT_PROPERTY_DECL(PY_LIST)
	KBE_GDSCRIPT_PROPERTY_DECL(BLOB)
	KBE_GDSCRIPT_PROPERTY_DECL(ARRAY)
	KBE_GDSCRIPT_PROPERTY_DECL(FIXED_DICT)
	KBE_GDSCRIPT_PROPERTY_DECL(VECTOR2)
	KBE_GDSCRIPT_PROPERTY_DECL(VECTOR3)
	KBE_GDSCRIPT_PROPERTY_DECL(VECTOR4)
	KBE_GDSCRIPT_PROPERTY_DECL(ENTITYCALL)
#undef KBE_GDSCRIPT_PROPERTY_DECL

protected:
	bool writeEntityPropertyCommon(PropertyDescription* pPropertyDescription);
	bool writeEntityDefsModuleInitDefTypeRecursive(const DataType* pDataType);
	std::string initBody_;
	std::set<uint16> generatedDefTypeIDs_;
};

}
#endif
