/*
This source file is part of KBEngine
For the latest info, see http://www.kbengine.org/

Copyright (c) 2008-2018 KBEngine.

KBEngine is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

KBEngine is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.
 
You should have received a copy of the GNU Lesser General Public License
along with KBEngine.  If not, see <http://www.gnu.org/licenses/>.
*/


#ifndef KBE_ENTITY_MACRO_H
#define KBE_ENTITY_MACRO_H

#include "common/common.h"
#include "server/asyncio_helper.h"
#include "server/callbackmgr.h"		

namespace KBEngine{

#define ENTITY_METHOD_DECLARE_BEGIN(APP, CLASS)																\
	ENTITY_CPP_IMPL(APP, CLASS)																				\
	SCRIPT_METHOD_DECLARE_BEGIN(CLASS)																		\
	SCRIPT_METHOD_DECLARE("__reduce_ex__",	reduce_ex__,					METH_VARARGS,				0)	\
	SCRIPT_METHOD_DECLARE("__getDEP__",		pyGetDatachangeEventPtr,			METH_VARARGS,				0)	\
	SCRIPT_METHOD_DECLARE("addTimer",		pyAddTimer,						METH_VARARGS,				0)	\
	SCRIPT_METHOD_DECLARE("delTimer",		pyDelTimer,						METH_VARARGS,				0)	\
	SCRIPT_METHOD_DECLARE("writeToDB",		pyWriteToDB,					METH_VARARGS,				0)	\
	SCRIPT_METHOD_DECLARE("destroy",		pyDestroyEntity,				METH_VARARGS | METH_KEYWORDS,0)	\

	
#define ENTITY_METHOD_DECLARE_END()																			\
	SCRIPT_METHOD_DECLARE_END()																				\


#define ENTITY_GETSET_DECLARE_BEGIN(CLASS)																	\
	SCRIPT_GETSET_DECLARE_BEGIN(CLASS)																		\
	SCRIPT_GET_DECLARE("id",				pyGetID,						0,						0)		\
	SCRIPT_GET_DECLARE("isDestroyed",		pyGetIsDestroyed,				0,						0)		\
	SCRIPT_GET_DECLARE("className",			pyGetClassName,					0,						0)		\


#define ENTITY_GETSET_DECLARE_END()																			\
	SCRIPT_GETSET_DECLARE_END()																				\


#define CLIENT_ENTITY_METHOD_DECLARE_BEGIN(APP, CLASS)														\
	ENTITY_CPP_IMPL(APP, CLASS)																				\
	SCRIPT_METHOD_DECLARE_BEGIN(CLASS)																		\
	SCRIPT_METHOD_DECLARE("__reduce_ex__",	reduce_ex__,					METH_VARARGS,			0)		\
	SCRIPT_METHOD_DECLARE("__getDEP__",		pyGetDatachangeEventPtr,			METH_VARARGS,			0)		\

	
#define CLIENT_ENTITY_METHOD_DECLARE_END()																	\
	SCRIPT_METHOD_DECLARE_END()																				\


#define CLIENT_ENTITY_GETSET_DECLARE_BEGIN(CLASS)															\
	SCRIPT_GETSET_DECLARE_BEGIN(CLASS)																		\
	SCRIPT_GET_DECLARE("id",				pyGetID,						0,						0)		\
	SCRIPT_GET_DECLARE("spaceID",			pyGetSpaceID,					0,						0)		\
	SCRIPT_GET_DECLARE("isDestroyed",		pyGetIsDestroyed,				0,						0)		\
	SCRIPT_GET_DECLARE("className",			pyGetClassName,					0,						0)		\


#define CLIENT_ENTITY_GETSET_DECLARE_END()																	\
	SCRIPT_GETSET_DECLARE_END()																				\


#ifdef CLIENT_NO_FLOAT																					
	#define ADD_POS_DIR_TO_STREAM(s, pos, dir)																\
		int32 x = (int32)pos.x;																				\
		int32 y = (int32)pos.y;																				\
		int32 z = (int32)pos.z;																				\
																											\
		s << (ENTITY_PROPERTY_UID)0 << posuid << x << y << z;																			\
																											\
		x = (int32)dir.x;																					\
		y = (int32)dir.y;																					\
		z = (int32)dir.z;																					\
																											\
		s << (ENTITY_PROPERTY_UID)0 << diruid << x << y << z;																			\


	// 位置与方向统一使用父 UID + 属性 UID，持久化和 Nex 2.8 网络流共享同一帧结构。
	// Position and direction consistently use parent plus property UIDs across persistence and Nex 2.8 network streams.
	#define ADD_POS_DIR_TO_PERSISTENT_STREAM(s, pos, dir)											\
		int32 persistentX = (int32)pos.x;															\
		int32 persistentY = (int32)pos.y;															\
		int32 persistentZ = (int32)pos.z;															\
		s << (ENTITY_PROPERTY_UID)0 << posuid << persistentX << persistentY << persistentZ;		\
		persistentX = (int32)dir.x;																\
		persistentY = (int32)dir.y;																\
		persistentZ = (int32)dir.z;																\
		s << (ENTITY_PROPERTY_UID)0 << diruid << persistentX << persistentY << persistentZ;		\

	#define ADD_POS_DIR_TO_STREAM_ALIASID(s, pos, dir)														\
		int32 x = (int32)pos.x;																				\
		int32 y = (int32)pos.y;																				\
		int32 z = (int32)pos.z;																				\
																											\
		uint8 aliasID = ENTITY_BASE_PROPERTY_ALIASID_POSITION_XYZ;											\
		s << (uint8)0 << aliasID << x << y << z;																		\
																											\
		x = (int32)dir.x;																					\
		y = (int32)dir.y;																					\
		z = (int32)dir.z;																					\
																											\
		aliasID = ENTITY_BASE_PROPERTY_ALIASID_DIRECTION_ROLL_PITCH_YAW;									\
		s << (uint8)0 << aliasID << x << y << z;																		\


	#define STREAM_TO_POS_DIR(s, pos, dir)																	\
	{																										\
		int32 x = 0;																						\
		int32 y = 0;																						\
		int32 z = 0;																						\
		/* Cell 创建流采用父 UID + 属性 UID，组件解码还依赖当前实体上下文。 */							\
		/* Cell creation streams use parent UID plus property UID, and component decoding needs entity context. */		\
		EntityDef::context().currComponentType = CELLAPP_TYPE;											\
		EntityDef::context().currEntityID = id();														\
		ENTITY_PROPERTY_UID uid;																			\
																											\
		s >> uid >> uid >> x >> y >> z;																			\
																											\
		pos.x = float(x);																					\
		pos.y = float(y);																					\
		pos.z = float(z);																					\
																											\
		s >> uid >> uid >> x >> y >> z;																			\
		dir.x = float(x);																					\
		dir.y = float(y);																					\
		dir.z = float(z);																					\
	}																										\


#else																									
	#define ADD_POS_DIR_TO_STREAM(s, pos, dir)																\
		s << (ENTITY_PROPERTY_UID)0 << posuid << pos.x << pos.y << pos.z;																\
		s << (ENTITY_PROPERTY_UID)0 << diruid << dir.x << dir.y << dir.z;																\

	// 位置与方向统一使用父 UID + 属性 UID，持久化和 Nex 2.8 网络流共享同一帧结构。
	// Position and direction consistently use parent plus property UIDs across persistence and Nex 2.8 network streams.
	#define ADD_POS_DIR_TO_PERSISTENT_STREAM(s, pos, dir)											\
		s << (ENTITY_PROPERTY_UID)0 << posuid << pos.x << pos.y << pos.z;						\
		s << (ENTITY_PROPERTY_UID)0 << diruid << dir.x << dir.y << dir.z;						\


	#define ADD_POS_DIR_TO_STREAM_ALIASID(s, pos, dir)														\
		uint8 aliasID = ENTITY_BASE_PROPERTY_ALIASID_POSITION_XYZ;											\
		s << (uint8)0 << aliasID << pos.x << pos.y << pos.z;															\
		aliasID = ENTITY_BASE_PROPERTY_ALIASID_DIRECTION_ROLL_PITCH_YAW;									\
		s << (uint8)0 << aliasID << dir.x << dir.y << dir.z;															\
	

	#define STREAM_TO_POS_DIR(s, pos, dir)																	\
	{																										\
		ENTITY_PROPERTY_UID uid;																			\
		s >> uid >> uid >> pos.x >> pos.y >> pos.z;																\
		s >> uid >> uid >> dir.x >> dir.y >> dir.z;																\
	}																										\


#endif	


#define ADD_POSDIR_TO_STREAM(s, pos, dir)																	\
	{																										\
		ENTITY_PROPERTY_UID posuid = ENTITY_BASE_PROPERTY_UTYPE_POSITION_XYZ;								\
		ENTITY_PROPERTY_UID diruid = ENTITY_BASE_PROPERTY_UTYPE_DIRECTION_ROLL_PITCH_YAW;					\
																											\
		Network::FixedMessages::MSGInfo* msgInfo =															\
					Network::FixedMessages::getSingleton().isFixed("Property::position");					\
																											\
		if(msgInfo != NULL)																					\
		{																									\
			posuid = msgInfo->msgid;																		\
			msgInfo = NULL;																					\
		}																									\
																											\
		msgInfo = Network::FixedMessages::getSingleton().isFixed("Property::direction");					\
		if(msgInfo != NULL)																					\
		{																									\
			diruid = msgInfo->msgid;																		\
			msgInfo = NULL;																					\
		}																									\
																											\
		ADD_POS_DIR_TO_STREAM(s, pos, dir)																	\
																											\
	}																										\

#define ADD_POSDIR_TO_PYDICT(pydict, pos, dir)																\
	{																										\
		PyObject* pypos = PyTuple_New(3);																	\
		PyObject* pydir = PyTuple_New(3);																	\
																											\
		PyTuple_SET_ITEM(pypos, 0, PyFloat_FromDouble(pos.x));												\
		PyTuple_SET_ITEM(pypos, 1, PyFloat_FromDouble(pos.y));												\
		PyTuple_SET_ITEM(pypos, 2, PyFloat_FromDouble(pos.z));												\
																											\
		PyTuple_SET_ITEM(pydir, 0, PyFloat_FromDouble(dir.x));												\
		PyTuple_SET_ITEM(pydir, 1, PyFloat_FromDouble(dir.y));												\
		PyTuple_SET_ITEM(pydir, 2, PyFloat_FromDouble(dir.z));												\
																											\
		PyDict_SetItemString(pydict, "position", pypos);													\
		PyDict_SetItemString(pydict, "direction", pydir);													\
		Py_DECREF(pypos);																					\
		Py_DECREF(pydir);																					\
	}																										\

/*
	debug info.
*/
#define CAN_DEBUG_CREATE_ENTITY
#ifdef CAN_DEBUG_CREATE_ENTITY
#define DEBUG_CREATE_ENTITY_NAMESPACE																		\
		if(g_debugEntity)																					\
		{																									\
			const char* ccattr_DEBUG_CREATE_ENTITY_NAMESPACE = PyUnicode_AsUTF8AndSize(key, NULL);			\
			PyObject* pytsval = PyObject_Str(value);														\
			const char* cccpytsval = PyUnicode_AsUTF8AndSize(pytsval, NULL);								\
			Py_DECREF(pytsval);																				\
			DEBUG_MSG(fmt::format("{}(refc={}, id={})::debug_createNamespace:add {}({}).\n",				\
												scriptName(),												\
												static_cast<PyObject*>(this)->ob_refcnt,					\
												this->id(),													\
																ccattr_DEBUG_CREATE_ENTITY_NAMESPACE,		\
																cccpytsval));								\
		}																									\


#define DEBUG_OP_ATTRIBUTE(op, ccattr)																		\
		if(g_debugEntity)																					\
		{																									\
			const char* ccattr_DEBUG_OP_ATTRIBUTE = PyUnicode_AsUTF8AndSize(ccattr, NULL);					\
			DEBUG_MSG(fmt::format("{}(refc={}, id={})::debug_op_attr:op={}, {}.\n",							\
												scriptName(),												\
												static_cast<PyObject*>(this)->ob_refcnt, this->id(),		\
															op, ccattr_DEBUG_OP_ATTRIBUTE));				\
		}																									\


#define DEBUG_PERSISTENT_PROPERTY(op, ccattr)																\
	{																										\
		if(g_debugEntity)																					\
		{																									\
			DEBUG_MSG(fmt::format("{}(refc={}, id={})::debug_op_Persistent:op={}, {}.\n",					\
												scriptName(),												\
												static_cast<PyObject*>(this)->ob_refcnt, this->id(),		\
															op, ccattr));									\
		}																									\
	}																										\


#define DEBUG_REDUCE_EX(tentity)																			\
		if(g_debugEntity)																					\
		{																									\
			DEBUG_MSG(fmt::format("{}(refc={}, id={})::debug_reduct_ex: utype={}.\n",						\
												tentity->scriptName(),										\
												static_cast<PyObject*>(tentity)->ob_refcnt,					\
												tentity->id(),												\
												tentity->pScriptModule()->getUType()));						\
		}																									\


#else
	#define DEBUG_CREATE_ENTITY_NAMESPACE			
	#define DEBUG_OP_ATTRIBUTE(op, ccattr)
	#define DEBUG_PERSISTENT_PROPERTY(op, ccattr)
	#define DEBUG_REDUCE_EX(tentity)
#endif


#define ENTITY_DESTROYED_CHECK(RETURN, OPNAME, ENTITY)														\
{																											\
	if(ENTITY->isDestroyed())																				\
	{																										\
		PyErr_Format(PyExc_Exception, "%s::%s: %d is destroyed!\n",											\
			OPNAME, ENTITY->scriptName(), ENTITY->id());													\
		PyErr_PrintEx(0);																					\
		RETURN;																								\
	}																										\
}																											\


// 实体的标志
#define ENTITY_FLAGS_UNKNOWN						0x00000000
#define ENTITY_FLAGS_DESTROYING						0x00000001
#define ENTITY_FLAGS_INITING						0x00000002
#define ENTITY_FLAGS_TELEPORT_START					0x00000004
#define ENTITY_FLAGS_TELEPORT_STOP					0x00000008
#define ENTITY_FLAGS_DESTROY_AFTER_GETCELL			0x00000010

#define ENTITY_HEADER(CLASS)																				\
protected:																									\
	ENTITY_ID										id_;													\
	ScriptDefModule*								pScriptModule_;											\
	const ScriptDefModule::PROPERTYDESCRIPTION_MAP* pPropertyDescrs_;										\
	SPACE_ID										spaceID_;												\
	ScriptTimers									scriptTimers_;											\
	PY_CALLBACKMGR									pyCallbackMgr_;											\
	bool											isDestroyed_;											\
	uint32											flags_;													\
public:																										\
	bool initing() const{ return hasFlags(ENTITY_FLAGS_INITING); }											\
																											\
	void onInitializeScript();																				\
	void initializeScript()																					\
	{																										\
		removeFlags(ENTITY_FLAGS_INITING);																	\
		SCOPED_PROFILE(SCRIPTCALL_PROFILE);																	\
																											\
		const ScriptDefModule::COMPONENTDESCRIPTION_MAP* pComponentDescrs =									\
			&pScriptModule_->getComponentDescrs();															\
																											\
		ScriptDefModule::COMPONENTDESCRIPTION_MAP::const_iterator iter1 = pComponentDescrs->begin();		\
		for (; iter1 != pComponentDescrs->end(); ++iter1)													\
		{																									\
			PyObject* pComponentProperty = PyObject_GetAttrString(this, iter1->first.c_str());				\
			if(pComponentProperty)																			\
			{																								\
				if(PyObject_TypeCheck(pComponentProperty, EntityComponent::getScriptType()))				\
				{																							\
					EntityComponent* pEntityComponent = static_cast<EntityComponent*>(pComponentProperty);	\
					pEntityComponent->initializeScript();													\
				}																							\
				else																						\
				{																							\
					PyErr_Format(PyExc_AssertionError, "%s.%s is not property of EntityComponent!",			\
						scriptName(), iter1->first.c_str());												\
					PyErr_PrintEx(0);																		\
				}																							\
																											\
				Py_DECREF(pComponentProperty);																\
			}																								\
			else																							\
			{																								\
				PyErr_Clear();																				\
			}																								\
		}																									\
																											\
		if(PyObject_HasAttrString(this, "__init__"))														\
		{																									\
			PyObject* pyResult = PyObject_CallMethod(this, const_cast<char*>("__init__"),					\
											const_cast<char*>(""));											\
			if(pyResult != NULL){																			\
				AsyncioHelper::submitCoroutine(pyResult);															\
				Py_DECREF(pyResult);																		\
			}																								\
			else																							\
				SCRIPT_ERROR_CHECK();																		\
		}																									\
																											\
		iter1 = pComponentDescrs->begin();																	\
		for (; iter1 != pComponentDescrs->end(); ++iter1)													\
		{																									\
			PyObject* pComponentProperty = PyObject_GetAttrString(this, iter1->first.c_str());				\
			if(pComponentProperty)																			\
			{																								\
				if(PyObject_TypeCheck(pComponentProperty, EntityComponent::getScriptType()))				\
				{																							\
					EntityComponent* pEntityComponent = static_cast<EntityComponent*>(pComponentProperty);	\
					pEntityComponent->updateOwner(id(), this);												\
					pEntityComponent->onAttached();															\
				}																							\
				else																						\
				{																							\
					PyErr_Format(PyExc_AssertionError, "%s.%s is not property of EntityComponent!",			\
						scriptName(), iter1->first.c_str());												\
					PyErr_PrintEx(0);																		\
				}																							\
																											\
				Py_DECREF(pComponentProperty);																\
			}																								\
			else																							\
			{																								\
				PyErr_Clear();																				\
			}																								\
		}																									\
																											\
		onInitializeScript();																				\
	}																										\
																											\
	void initializeEntity(PyObject* dictData, bool persistentData = false)									\
	{																										\
		createNamespace(dictData, persistentData);															\
		initializeScript();																					\
	}																										\
																											\
	bool _reload(bool fullReload);																			\
	bool reload(bool fullReload)																			\
	{																										\
		if(fullReload)																						\
		{																									\
			pScriptModule_ = EntityDef::findScriptModule(scriptName());										\
			KBE_ASSERT(pScriptModule_);																		\
			pPropertyDescrs_ = &pScriptModule_->getPropertyDescrs();										\
		}																									\
																											\
		if(PyObject_SetAttrString(this, "__class__", (PyObject*)pScriptModule_->getScriptType()) == -1)		\
		{																									\
			WARNING_MSG(fmt::format("Entity::reload: "														\
				"{} {} could not change __class__ to new class!\n",											\
				pScriptModule_->getName(), id_));															\
			PyErr_Print();																					\
			return false;																					\
		}																									\
																											\
		/* 普通 reloadScript(False) 只更新脚本行为层，不能重新初始化属性，						\
		   否则在线实体数据会被默认值覆盖；只有 fullReload 才做属性差异补齐。 */					\
		/* A normal reloadScript(False) must preserve live values; only a full reload fills property differences. */\
		if(fullReload)																						\
			initProperty(true);																				\
		return _reload(fullReload);																			\
	}																										\
																											\
	void createNamespace(PyObject* dictData, bool persistentData = false)									\
	{																										\
		if(dictData == NULL)																				\
			return;																							\
																											\
		if(!PyDict_Check(dictData)){																		\
			ERROR_MSG(fmt::format(#CLASS"::createNamespace: create"#CLASS"[{}:{}] "							\
				"args is not a dict.\n",																	\
				scriptName(), id_));																		\
			return;																							\
		}																									\
																											\
		EntityDef::context().currComponentType = g_componentType;											\
		EntityDef::context().currEntityID = id();															\
																											\
		Py_ssize_t pos = 0;																					\
		PyObject *key, *value;																				\
		PyObject* cellDataDict = PyObject_GetAttrString(this, "cellData");									\
																											\
		if(cellDataDict == NULL)																			\
		{																									\
			PyErr_Clear();																					\
			EntityComponent::convertDictDataToEntityComponent(id(), this, pScriptModule_, dictData, persistentData); \
		}																									\
		else																								\
		{																									\
			PyObject* cellDataDictNew = PyDict_GetItemString(dictData, "cellData");							\
			if (cellDataDictNew)																			\
			{																								\
				if(PyDict_Check(cellDataDictNew))															\
				{																							\
					PyDict_Update(cellDataDict, cellDataDictNew);											\
				}																							\
				else																						\
				{																							\
					ERROR_MSG(fmt::format(#CLASS"::createNamespace: create"#CLASS"[{}:{}] "					\
						"cellData is not a dict.\n",														\
						scriptName(), id_));																\
				}																							\
																											\
				PyDict_DelItemString(dictData, "cellData");													\
			}																								\
			else																							\
			{																								\
				PyErr_Clear();																				\
			}																								\
		}																									\
																											\
		while(PyDict_Next(dictData, &pos, &key, &value))													\
		{																									\
			DEBUG_CREATE_ENTITY_NAMESPACE																	\
			if(PyObject_HasAttr(this, key) > 0)																\
			{																								\
				const char* ccattr = PyUnicode_AsUTF8AndSize(key, NULL);									\
																											\
				PropertyDescription* pCompPropertyDescription =												\
					pScriptModule_->findComponentPropertyDescription(ccattr);								\
																											\
				if (pCompPropertyDescription)																\
				{																							\
					/* 持久化流可能已生成组件对象，只有字典值需要按字段更新现有组件。 */\
					/* Persistent streams may already contain component objects; only dictionary values update the existing component by field. */\
					if(PyDict_Check(value))			\
					{																						\
						EntityComponent* pEntityComponent = (EntityComponent*)PyObject_GetAttr(this, key);	\
						pEntityComponent->updateFromDict(this, value);										\
						Py_DECREF(pEntityComponent);														\
					}																						\
					else																					\
					{																						\
						/* 持久化流只携带组件类型和值；父属性与所有者必须在替换默认组件前由实体上下文补齐。 */\
						/* Persistent streams carry only component type and value; the entity context must bind parent property and owner before replacing the default component. */\
						EntityComponent* pEntityComponent = static_cast<EntityComponent*>(value);		\
						pEntityComponent->pPropertyDescription(pCompPropertyDescription);				\
						pEntityComponent->updateOwner(id(), this);									\
						PyObject_SetAttr(this, key, value);													\
					}																						\
				}																							\
				else																						\
				{																							\
					PyObject_SetAttr(this, key, value);														\
				}																							\
																											\
				continue;																					\
			}																								\
																											\
			if(cellDataDict != NULL && PyDict_Contains(cellDataDict, key) > 0)								\
			{																								\
				PyObject* pyVal = PyDict_GetItem(cellDataDict, key);										\
				if (PyDict_Check(pyVal))																	\
				{																							\
					/* CellData 中的组件占位字典必须原位合并，保持后续组件转换所依赖的对象身份。 */\
					/* Component placeholder dictionaries in CellData must be merged in place to preserve identity for later conversion. */\
					if (0 != PyDict_Update(pyVal, value))					\
					{																						\
						SCRIPT_ERROR_CHECK();																\
						KBE_ASSERT(false);																	\
					}																						\
				}																							\
				else																						\
				{																							\
					PyDict_SetItem(cellDataDict, key, value);												\
				}																							\
			}																								\
			else																							\
			{																								\
				const char* ccattr = PyUnicode_AsUTF8AndSize(key, NULL);									\
																											\
				PropertyDescription* pCompPropertyDescription =												\
					pScriptModule_->findComponentPropertyDescription(ccattr);								\
																											\
																											\
				if (pCompPropertyDescription)																\
				{																							\
					/* 一般在base上可能放在cellData中是字典，而没有cell的实体需要pass这个设置 */					\
					/* Base may keep component dictionaries in CellData; components without Cell data must not overwrite entity attributes. */\
					if(PyDict_Check(value))																	\
						continue;																			\
				}																							\
																											\
				PyObject_SetAttr(this, key, value);															\
			}																								\
		}																									\
																											\
		SCRIPT_ERROR_CHECK();																				\
																											\
		Py_XDECREF(cellDataDict);																			\
	}																										\
																											\
	void addCellDataToStream(COMPONENT_TYPE sendTo, uint32 flags, MemoryStream* mstream, bool useAliasID = false);\
																											\
	PyObject* createCellDataFromStream(MemoryStream* mstream)												\
	{																										\
		/* 组件默认值解析依赖当前实体和 Cell 域，必须在读取任何属性前建立上下文。 */						\
		/* Component default parsing depends on the current entity and Cell domain, so bind the context before reading properties. */\
		EntityDef::context().currComponentType = CELLAPP_TYPE;												\
		EntityDef::context().currEntityID = id();															\
																															\
		PyObject* cellData = PyDict_New();																	\
		ENTITY_PROPERTY_UID uid;																			\
		Vector3 pos, dir;																					\
		STREAM_TO_POS_DIR(*mstream, pos, dir);																\
		ADD_POSDIR_TO_PYDICT(cellData, pos, dir);															\
																											\
		ScriptDefModule::PROPERTYDESCRIPTION_UIDMAP& propertyDescrs =										\
								pScriptModule_->getCellPropertyDescriptions_uidmap();						\
																											\
		size_t count = propertyDescrs.size();															\
																											\
		{																												\
			ScriptDefModule::PROPERTYDESCRIPTION_UIDMAP::iterator iter = propertyDescrs.begin();			\
			for(; iter != propertyDescrs.end(); ++iter)													\
			{																											\
				/* 没有 Cell 属性的组件不会写入创建流，因此不能把它计入待读取数量。 */					\
				/* Components without Cell properties are not serialized, so exclude them from the expected item count. */\
				if (iter->second->getDataType()->type() == DATA_TYPE_ENTITY_COMPONENT)						\
				{																										\
					EntityComponentType* pEntityComponentType = (EntityComponentType*)iter->second->getDataType();\
					if (pEntityComponentType->pScriptDefModule()->getCellPropertyDescriptions().size() == 0)\
					{																								\
						--count;																				\
					}																							\
				}																								\
			}																									\
		}																										\
																															\
		while(mstream->length() > 0 && count-- > 0)													\
		{																									\
			/* 创建流为每项写入父 UID 和实际属性 UID；当前层只按后者查找属性。 */					\
			/* Each creation-stream item contains a parent UID and an effective property UID; this level resolves the latter. */\
			(*mstream) >> uid >> uid;																	\
			ScriptDefModule::PROPERTYDESCRIPTION_UIDMAP::iterator iter = propertyDescrs.find(uid);			\
			if(iter == propertyDescrs.end())																\
			{																								\
				ERROR_MSG(fmt::format("{}::createCellDataFromStream: not found uid({})! entityID={}\n", scriptName(), uid, id()));	\
				break;																						\
			}																								\
																											\
			PyObject* pyobj = NULL;																			\
			if (iter->second->getDataType()->type() == DATA_TYPE_ENTITY_COMPONENT)						\
			{																											\
				pyobj = ((EntityComponentType*)iter->second->getDataType())->createCellDataFromStream(mstream);\
			}																									\
			else																								\
			{																											\
				pyobj = iter->second->createFromStream(mstream);									\
			}																									\
																											\
			if(pyobj == NULL)																				\
			{																								\
				SCRIPT_ERROR_CHECK();																		\
				pyobj = iter->second->parseDefaultStr("");													\
				PyDict_SetItemString(cellData, iter->second->getName(), pyobj);								\
				Py_DECREF(pyobj);																			\
			}																								\
			else																							\
			{																								\
				PyDict_SetItemString(cellData, iter->second->getName(), pyobj);								\
				Py_DECREF(pyobj);																			\
			}																								\
																											\
		}																									\
																											\
		return cellData;																					\
	}																										\
																											\
	void addCellDataToStreamByDetailLevel(int8 detailLevel, MemoryStream* mstream, bool useAliasID = false)	\
	{																										\
		PyObject* cellData = PyObject_GetAttrString(this, "__dict__");										\
																											\
		ScriptDefModule::PROPERTYDESCRIPTION_MAP& propertyDescrs =											\
				pScriptModule_->getCellPropertyDescriptionsByDetailLevel(detailLevel);						\
		ScriptDefModule::PROPERTYDESCRIPTION_MAP::const_iterator iter = propertyDescrs.begin();				\
		for(; iter != propertyDescrs.end(); ++iter)															\
		{																									\
			PropertyDescription* propertyDescription = iter->second;										\
			PyObject* pyVal = PyDict_GetItemString(cellData, propertyDescription->getName());				\
																											\
			if(useAliasID && pScriptModule_->usePropertyDescrAlias())										\
			{																								\
				(*mstream) << propertyDescription->aliasIDAsUint8();										\
			}																								\
			else																							\
			{																								\
				(*mstream) << propertyDescription->getUType();												\
			}																								\
																											\
			propertyDescription->getDataType()->addToStream(mstream, pyVal);								\
		}																									\
																											\
		Py_XDECREF(cellData);																				\
		SCRIPT_ERROR_CHECK();																				\
	}																										\
																											\
	void addClientDataToStream(MemoryStream* s, bool otherClient = false)									\
	{																										\
		/* 初始客户端属性流可能包含整组件，必须固定为 Client 域并在完成后恢复共享上下文，避免影响同一 Tick 的后续序列化。 */ \
		/* Initial client property streams may contain whole components, so pin them to the Client domain and restore shared context afterward. */ \
		COMPONENT_TYPE previousComponentType = EntityDef::context().currComponentType;						\
		ENTITY_ID previousEntityID = EntityDef::context().currEntityID;									\
		EntityDef::context().currComponentType = CLIENT_TYPE;											\
		EntityDef::context().currEntityID = id();													\
																											\
		PyObject* pydict = PyObject_GetAttrString(this, "__dict__");										\
																											\
		ScriptDefModule::PROPERTYDESCRIPTION_MAP& propertyDescrs =											\
				pScriptModule()->getClientPropertyDescriptions();											\
		ScriptDefModule::PROPERTYDESCRIPTION_MAP::iterator iter = propertyDescrs.begin();					\
		for(; iter != propertyDescrs.end(); ++iter)															\
		{																									\
			PropertyDescription* propertyDescription = iter->second;										\
			if(otherClient)																					\
			{																								\
				if((propertyDescription->getFlags() & ENTITY_BROADCAST_OTHER_CLIENT_FLAGS) <= 0)			\
					continue;																				\
			}																								\
																											\
			PyObject *key = PyUnicode_FromString(propertyDescription->getName());							\
																											\
			if(PyDict_Contains(pydict, key) > 0)															\
			{																								\
				if(pScriptModule()->usePropertyDescrAlias())												\
				{																							\
					(*s) << (uint8)0;																	\
	    			(*s) << propertyDescription->aliasIDAsUint8();											\
				}																							\
				else																						\
				{																							\
					(*s) << (ENTITY_PROPERTY_UID)0;														\
	    			(*s) << propertyDescription->getUType();												\
				}																							\
																											\
	    		propertyDescription->getDataType()->addToStream(s, PyDict_GetItem(pydict, key));			\
			}																								\
																											\
			Py_DECREF(key);																					\
		}																									\
																											\
		Py_XDECREF(pydict);																					\
		EntityDef::context().currComponentType = previousComponentType;								\
		EntityDef::context().currEntityID = previousEntityID;										\
	}																										\
																											\
	void addPositionAndDirectionToStream(MemoryStream& s, bool useAliasID = false, bool persistentFrame = false);\
																											\
	static PyObject* __py_reduce_ex__(PyObject* self, PyObject* protocol)									\
	{																										\
		CLASS* entity = static_cast<CLASS*>(self);															\
		DEBUG_REDUCE_EX(entity);																			\
		PyObject* args = PyTuple_New(2);																	\
		PyObject* unpickleMethod = script::Pickler::getUnpickleFunc("EntityCall");							\
		PyTuple_SET_ITEM(args, 0, unpickleMethod);															\
		PyObject* args1 = PyTuple_New(4);																	\
		PyTuple_SET_ITEM(args1, 0, PyLong_FromUnsignedLong(entity->id()));									\
		PyTuple_SET_ITEM(args1, 1, PyLong_FromUnsignedLongLong(g_componentID));								\
		PyTuple_SET_ITEM(args1, 2, PyLong_FromUnsignedLong(entity->pScriptModule()->getUType()));			\
		if(g_componentType == BASEAPP_TYPE)																	\
			PyTuple_SET_ITEM(args1, 3, PyLong_FromUnsignedLong(ENTITYCALL_TYPE_BASE));						\
		else																								\
			PyTuple_SET_ITEM(args1, 3, PyLong_FromUnsignedLong(ENTITYCALL_TYPE_CELL));						\
		PyTuple_SET_ITEM(args, 1, args1);																	\
																											\
		if(unpickleMethod == NULL){																			\
			Py_DECREF(args);																				\
			return NULL;																					\
		}																									\
		SCRIPT_ERROR_CHECK();																				\
		return args;																						\
	}																										\
																											\
	inline ScriptTimers& scriptTimers(){ return scriptTimers_; }											\
	void onTimer(ScriptID timerID, int useraAgs);															\
																											\
	PY_CALLBACKMGR& callbackMgr(){ return pyCallbackMgr_; }													\
																											\
	static PyObject* __pyget_pyGetID(CLASS *self, void *closure)											\
	{																										\
		return PyLong_FromLong(self->id());																	\
	}																										\
																											\
	INLINE ENTITY_ID id() const																				\
	{																										\
		return id_;																							\
	}																										\
																											\
	INLINE void id(ENTITY_ID v)																				\
	{																										\
		id_ = v; 																							\
	}																										\
																											\
	INLINE bool hasFlags(uint32 v) const																	\
	{																										\
		return (flags_ & v) > 0;																			\
	}																										\
																											\
	INLINE uint32 flags() const																				\
	{																										\
		return flags_;																						\
	}																										\
																											\
	INLINE void flags(uint32 v)																				\
	{																										\
		flags_ = v; 																						\
	}																										\
																											\
	INLINE uint32 addFlags(uint32 v)																		\
	{																										\
		flags_ |= v;																						\
		return flags_;																						\
	}																										\
																											\
	INLINE uint32 removeFlags(uint32 v)																		\
	{																										\
		flags_ &= ~v; 																						\
		return flags_;																						\
	}																										\
																											\
	INLINE SPACE_ID spaceID() const																			\
	{																										\
		return spaceID_;																					\
	}																										\
	INLINE void spaceID(SPACE_ID id)																		\
	{																										\
		spaceID_ = id;																						\
	}																										\
	static PyObject* __pyget_pyGetSpaceID(CLASS *self, void *closure)										\
	{																										\
		return PyLong_FromLong(self->spaceID());															\
	}																										\
																											\
	INLINE ScriptDefModule* pScriptModule(void) const														\
	{																										\
		return pScriptModule_; 																				\
	}																										\
																											\
	int onScriptDelAttribute(PyObject* attr)																\
	{																										\
		const char* ccattr = PyUnicode_AsUTF8AndSize(attr, NULL);											\
		DEBUG_OP_ATTRIBUTE("del", attr)																		\
																											\
		if(pPropertyDescrs_)																				\
		{																									\
																											\
			ScriptDefModule::PROPERTYDESCRIPTION_MAP::const_iterator iter = pPropertyDescrs_->find(ccattr);	\
			if(iter != pPropertyDescrs_->end())																\
			{																								\
				char err[255];																				\
				kbe_snprintf(err, 255, "property[%s] defined in %s.def, del failed!", ccattr, scriptName());\
				PyErr_SetString(PyExc_TypeError, err);														\
				PyErr_PrintEx(0);																			\
				return 0;																					\
			}																								\
		}																									\
																											\
		if(pScriptModule_->findMethodDescription(ccattr, g_componentType) != NULL)							\
		{																									\
			char err[255];																					\
			kbe_snprintf(err, 255, "method[%s] defined in %s.def, del failed!", ccattr, scriptName());		\
			PyErr_SetString(PyExc_TypeError, err);															\
			PyErr_PrintEx(0);																				\
			return 0;																						\
		}																									\
																											\
		return ScriptObject::onScriptDelAttribute(attr);													\
	}																										\
																											\
	int onScriptSetAttribute(PyObject* attr, PyObject* value)												\
	{																										\
		DEBUG_OP_ATTRIBUTE("set", attr)																		\
		const char* ccattr = PyUnicode_AsUTF8AndSize(attr, NULL);											\
																											\
		if(pPropertyDescrs_)																				\
		{																									\
			ScriptDefModule::PROPERTYDESCRIPTION_MAP::const_iterator iter = pPropertyDescrs_->find(ccattr);	\
			if(iter != pPropertyDescrs_->end())																\
			{																								\
				PropertyDescription* propertyDescription = iter->second;									\
				DataType* dataType = propertyDescription->getDataType();									\
																											\
				if(!hasFlags(ENTITY_FLAGS_DESTROYING) && isDestroyed_)										\
				{																							\
					PyErr_Format(PyExc_AssertionError, "can't set %s.%s to %s. entity is destroyed!",		\
													scriptName(), ccattr, value->ob_type->tp_name);			\
					return 0;																				\
				}																							\
																											\
				if(!dataType->isSameType(value))															\
				{																							\
					PyErr_Format(PyExc_ValueError, "can't set %s.%s to %s.",								\
													scriptName(), ccattr, value->ob_type->tp_name);			\
					PyErr_PrintEx(0);																		\
					return 0;																				\
				}																							\
				else																						\
				{																							\
					Py_ssize_t ob_refcnt = value->ob_refcnt;												\
					PyObject* pySetObj = propertyDescription->onSetValue(this, value);						\
																											\
					/* 如果def属性数据有改变， 那么可能需要广播 */												\
					if(pySetObj != NULL)																	\
					{																						\
						onDefDataChanged(NULL, propertyDescription, pySetObj);								\
						if(pySetObj == value && pySetObj->ob_refcnt - ob_refcnt > 1)						\
							Py_DECREF(pySetObj);															\
					}																						\
																											\
					return pySetObj == NULL ? -1 : 0;														\
				}																							\
			}																								\
		}																									\
																											\
		return ScriptObject::onScriptSetAttribute(attr, value);												\
	}																										\
																											\
	PyObject * onScriptGetAttribute(PyObject* attr);														\
																											\
	DECLARE_PY_MOTHOD_ARG3(pyAddTimer, float, float, int32);												\
																											\
	static PyObject* __py_pyWriteToDB(PyObject* self, PyObject* args)										\
	{																										\
		uint16 currargsSize = (uint16)PyTuple_Size(args);													\
		CLASS* pobj = static_cast<CLASS*>(self);															\
																											\
		if((g_componentType == CELLAPP_TYPE && currargsSize > 2) ||											\
			(g_componentType == BASEAPP_TYPE && currargsSize > 3))											\
		{																									\
			PyErr_Format(PyExc_AssertionError,																\
							"%s::writeToDB: args max require %d args, gived %d!\n",							\
				pobj->scriptName(), 1, currargsSize);														\
																											\
			PyErr_PrintEx(0);																				\
			S_Return;																						\
		}																									\
																											\
		int extra = 0;																						\
		std::string strextra;																				\
		PyObject* pycallback = NULL;																		\
																											\
		if(g_componentType == CELLAPP_TYPE)																	\
		{																									\
			PyObject* baseMB = PyObject_GetAttrString(self, "base");										\
			if(baseMB == NULL || baseMB == Py_None)															\
			{																								\
				PyErr_Clear();																				\
				PyErr_SetString(PyExc_AssertionError,														\
				"This method can only be called on a real entity that has a base entity. ");				\
				PyErr_PrintEx(0);																			\
			}																								\
		}																									\
		else if(g_componentType == BASEAPP_TYPE)															\
		{																									\
			extra = -1;	/* shouldAutoLoad -1默认不改变设置 */												\
		}																									\
																											\
		if(currargsSize == 1)																				\
		{																									\
			if(g_componentType == BASEAPP_TYPE)																\
			{																								\
				if(!PyArg_ParseTuple(args, "O", &pycallback))												\
				{																							\
					PyErr_Format(PyExc_AssertionError, "%s::writeToDB: args error!", pobj->scriptName());	\
					PyErr_PrintEx(0);																		\
					pycallback = NULL;																		\
					S_Return;																				\
				}																							\
																											\
				if(!PyCallable_Check(pycallback))															\
				{																							\
					if(pycallback != Py_None)																\
					{																						\
						PyErr_Format(PyExc_TypeError, "%s::writeToDB: args1 not is callback!", pobj->scriptName());\
						PyErr_PrintEx(0);																	\
						S_Return;																			\
					}																						\
					else																					\
					{																						\
						pycallback = NULL;																	\
					}																						\
				}																							\
			}																								\
			else																							\
			{																								\
				if(!PyArg_ParseTuple(args, "i", &extra))													\
				{																							\
					PyErr_Format(PyExc_AssertionError, "%s::writeToDB: args error!", pobj->scriptName());	\
					PyErr_PrintEx(0);																		\
					pycallback = NULL;																		\
					S_Return;																				\
				}																							\
			}																								\
		}																									\
		else if(currargsSize == 2)																			\
		{																									\
			if(g_componentType == BASEAPP_TYPE)																\
			{																								\
				if(!PyArg_ParseTuple(args, "O|i", &pycallback, &extra))										\
				{																							\
					PyErr_Format(PyExc_AssertionError, "%s::writeToDB: args error!", pobj->scriptName());	\
					PyErr_PrintEx(0);																		\
					pycallback = NULL;																		\
					S_Return;																				\
				}																							\
																											\
				if(!PyCallable_Check(pycallback))															\
				{																							\
					if(pycallback != Py_None)																\
					{																						\
						PyErr_Format(PyExc_TypeError, "%s::writeToDB: args1 not is callback!", pobj->scriptName());	\
						PyErr_PrintEx(0);																	\
						S_Return;																			\
					}																						\
					else																					\
					{																						\
						pycallback = NULL;																	\
					}																						\
				}																							\
			}																								\
			else																							\
			{																								\
				PyObject* pystr_extra = NULL;																\
				if(!PyArg_ParseTuple(args, "i|O", &extra, &pystr_extra))									\
				{																							\
					PyErr_Format(PyExc_AssertionError, "%s::writeToDB: args error!", pobj->scriptName());	\
					PyErr_PrintEx(0);																		\
					pycallback = NULL;																		\
					S_Return;																				\
				}																							\
																											\
				if(pystr_extra)																				\
				{																							\
					strextra = PyUnicode_AsUTF8AndSize(pystr_extra, NULL);									\
				}																							\
																											\
				if(!g_kbeSrvConfig.dbInterface(strextra))													\
				{																							\
					PyErr_Format(PyExc_TypeError, "%s::writeToDB: args2, "									\
													"incorrect dbInterfaceName(%s)!",						\
													pobj->scriptName(), strextra.c_str());					\
					PyErr_PrintEx(0);																		\
					S_Return;																				\
				}																							\
			}																								\
		}																									\
		else if(currargsSize == 3)																			\
		{																									\
			if(g_componentType == BASEAPP_TYPE)																\
			{																								\
				PyObject* pystr_extra = NULL;																\
				if(!PyArg_ParseTuple(args, "O|i|O", &pycallback, &extra, &pystr_extra))						\
				{																							\
					PyErr_Format(PyExc_AssertionError, "%s::writeToDB: args error!", pobj->scriptName());	\
					PyErr_PrintEx(0);																		\
					pycallback = NULL;																		\
					S_Return;																				\
				}																							\
																											\
				if(!PyCallable_Check(pycallback))															\
				{																							\
					if(pycallback != Py_None)																\
					{																						\
						PyErr_Format(PyExc_TypeError, "%s::writeToDB: args1 not is callback!", pobj->scriptName());	\
						PyErr_PrintEx(0);																	\
						S_Return;																			\
					}																						\
					else																					\
					{																						\
						pycallback = NULL;																	\
					}																						\
				}																							\
																											\
				if(pystr_extra)																				\
				{																							\
					strextra = PyUnicode_AsUTF8AndSize(pystr_extra, NULL);									\
				}																							\
																											\
				if(!g_kbeSrvConfig.dbInterface(strextra))													\
				{																							\
					PyErr_Format(PyExc_TypeError, "%s::writeToDB: args3, "									\
										"incorrect dbInterfaceName(%s)!",									\
											pobj->scriptName(), strextra.c_str());							\
					PyErr_PrintEx(0);																		\
					S_Return;																				\
				}																							\
			}																								\
			else																							\
			{																								\
				KBE_ASSERT(false);																			\
			}																								\
		}																									\
																											\
		pobj->writeToDB(pycallback, (void*)&extra, (void*)strextra.c_str());								\
		S_Return;																							\
	}																										\
																											\
	void writeToDB(void* data, void* extra1, void* extra2);													\
																											\
	void destroy(bool callScript = true)																	\
	{																										\
		if(hasFlags(ENTITY_FLAGS_DESTROYING))																\
			return;																							\
																											\
		if(!isDestroyed_)																					\
		{																									\
			isDestroyed_ = true;																			\
			addFlags(ENTITY_FLAGS_DESTROYING);																\
			/* 组件先执行 onDetached，但必须暂时保留 owner，保证实体 onDestroy 期间组件仍可访问所属实体。 */ \
			/* Components run onDetached first while retaining owner access throughout the entity onDestroy callback. */ \
			EntityComponent::onEntityDestroy(this, pScriptModule_, callScript, true); \
			onDestroy(callScript);																			\
			scriptTimers_.cancelAll();																		\
			removeFlags(ENTITY_FLAGS_DESTROYING);															\
			/* 实体回调结束后组件释放 owner 强引用，打破 Entity 与 EntityComponent 的 Python 引用环。 */ \
			/* After entity callbacks finish, components release strong owner references to break the Python reference cycle. */ \
			EntityComponent::onEntityDestroy(this, pScriptModule_, callScript, false); \
			Py_DECREF(this);																				\
		}																									\
	}																										\
	INLINE bool isDestroyed() const																			\
	{																										\
		return isDestroyed_;																				\
	}																										\
	DECLARE_PY_GET_MOTHOD(pyGetIsDestroyed);																\
																											\
	void destroyEntity();																					\
	static PyObject* __py_pyDestroyEntity(PyObject* self, PyObject* args, PyObject * kwargs);				\
																											\
	DECLARE_PY_GET_MOTHOD(pyGetClassName);																	\
																											\
	void initProperty(bool isReload = false);																\
																													\
	static PyObject* __py_pyGetDatachangeEventPtr(PyObject* self, PyObject* args)							\
	{																											\
		(void)args;																								\
		CLASS* pobj = static_cast<CLASS*>(self);															\
		/* 静态槽位为内部 ABI 提供稳定地址；组件会立即复制回调，不持有槽位或改变实体生命周期。 */ \
		/* The static slot gives the internal ABI a stable address; components copy the callback immediately without retaining it. */ \
		static EntityComponent::OnDataChangedEvent dataChangedEvent;									\
		dataChangedEvent = std::bind(&CLASS::onDefDataChanged, pobj,									\
			std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);					\
		return PyLong_FromVoidPtr(static_cast<void*>(&dataChangedEvent));								\
	}																											\


#define ENTITY_CPP_IMPL(APP, CLASS)																			\
	class EntityScriptTimerHandler : public TimerHandler													\
	{																										\
	public:																									\
		EntityScriptTimerHandler(CLASS * entity) : pEntity_( entity )										\
		{																									\
		}																									\
																											\
	private:																								\
		virtual void handleTimeout(TimerHandle handle, void * pUser)										\
		{																									\
			ScriptTimers* scriptTimers = &pEntity_->scriptTimers();											\
			int id = ScriptTimersUtil::getIDForHandle( scriptTimers, handle );								\
			pEntity_->onTimer(id, static_cast<int>(intptr(pUser)));												\
		}																									\
																											\
		virtual void onRelease( TimerHandle handle, void * /*pUser*/ )										\
		{																									\
			ScriptTimers* scriptTimers = &pEntity_->scriptTimers();											\
			scriptTimers->releaseTimer(handle);																\
			delete this;																					\
		}																									\
																											\
		CLASS* pEntity_;																					\
	};																										\
																											\
	PyObject* CLASS::pyAddTimer(float interval, float repeat, int32 userArg)								\
	{																										\
		EntityScriptTimerHandler* pHandler = new EntityScriptTimerHandler(this);							\
		ScriptTimers * pTimers = &scriptTimers_;															\
		int id = ScriptTimersUtil::addTimer(&pTimers,														\
				interval, repeat,																			\
				userArg, pHandler);																			\
																											\
		if (id == 0)																						\
		{																									\
			PyErr_SetString(PyExc_ValueError, "Unable to add timer");										\
			PyErr_PrintEx(0);																				\
			delete pHandler;																				\
																											\
			return NULL;																					\
		}																									\
																											\
		return PyLong_FromLong(id);																			\
	}																										\
																											\
	static PyObject* __py_pyDelTimer(PyObject* self, PyObject* args)										\
	{																										\
		Py_ssize_t currargsSize = PyTuple_Size(args);													\
		CLASS* pobj = static_cast<CLASS*>(self);															\
																											\
		if (currargsSize != 1)																				\
		{																									\
			PyErr_Format(PyExc_AssertionError,																\
				"%s::delTimer: args require 1 args(id|int or \"All\"|str), gived %d!\n",					\
				pobj->scriptName(), currargsSize);															\
																											\
			PyErr_PrintEx(0);																				\
			return PyLong_FromLong(-1);																		\
		}																									\
																											\
		ScriptID timerID = 0;																				\
		PyObject* pyargobj = NULL;																			\
																											\
		if (!PyArg_ParseTuple(args, "O", &pyargobj))														\
		{																									\
			PyErr_Format(PyExc_TypeError,																	\
				"%s::delTimer: args(id|int or \"All\"|str) error!",											\
				pobj->scriptName());																		\
																											\
			PyErr_PrintEx(0);																				\
			return PyLong_FromLong(-1);																		\
		}																									\
																											\
		if (pyargobj == NULL)																				\
		{																									\
			PyErr_Format(PyExc_TypeError,																	\
				"%s::delTimer: args(id|int or \"All\"|str) error!",											\
				pobj->scriptName());																		\
																											\
			PyErr_PrintEx(0);																				\
			return PyLong_FromLong(-1);																		\
		}																									\
																											\
		if (PyUnicode_Check(pyargobj))																		\
		{																									\
			if (strcmp(PyUnicode_AsUTF8AndSize(pyargobj, NULL), "All") == 0)								\
			{																								\
				pobj->scriptTimers().cancelAll();															\
			}																								\
			else																							\
			{																								\
				PyErr_Format(PyExc_TypeError,																\
					"%s::delTimer: args not is \"All\"!",													\
					pobj->scriptName());																	\
																											\
				PyErr_PrintEx(0);																			\
				return PyLong_FromLong(-1);																	\
			}																								\
																											\
			return PyLong_FromLong(0);																		\
		}																									\
		else                                                                                                \
		{																									\
			if (!PyLong_Check(pyargobj))																	\
			{																								\
				PyErr_Format(PyExc_TypeError,																\
					"%s::delTimer: args(id|int) error!",													\
					pobj->scriptName());																	\
																											\
				PyErr_PrintEx(0);																			\
				return PyLong_FromLong(-1);																	\
			}																								\
																											\
			timerID = PyLong_AsLong(pyargobj);																\
		}																									\
																											\
		if(!ScriptTimersUtil::delTimer(&pobj->scriptTimers(), timerID))										\
		{																									\
			return PyLong_FromLong(-1);																		\
		}																									\
																											\
		return PyLong_FromLong(timerID);																	\
	}																										\
																											\
	void CLASS::destroyEntity()																				\
	{																										\
		APP::getSingleton().destroyEntity(id_, true);														\
	}																										\
																											\
	PyObject* CLASS::pyGetIsDestroyed()																		\
	{																										\
		return PyBool_FromLong(isDestroyed());																\
	}																										\
																											\
	PyObject* CLASS::pyGetClassName()																		\
	{																										\
		return PyUnicode_FromString(scriptName());															\
	}																										\
																											\
	void CLASS::addPositionAndDirectionToStream(MemoryStream& s, bool useAliasID, bool persistentFrame)		\
	{																										\
		ENTITY_PROPERTY_UID posuid = ENTITY_BASE_PROPERTY_UTYPE_POSITION_XYZ;								\
		ENTITY_PROPERTY_UID diruid = ENTITY_BASE_PROPERTY_UTYPE_DIRECTION_ROLL_PITCH_YAW;					\
																											\
		Network::FixedMessages::MSGInfo* msgInfo =															\
					Network::FixedMessages::getSingleton().isFixed("Property::position");					\
																											\
		if(msgInfo != NULL)																					\
		{																									\
			posuid = msgInfo->msgid;																		\
			msgInfo = NULL;																					\
		}																									\
																											\
		msgInfo = Network::FixedMessages::getSingleton().isFixed("Property::direction");					\
		if(msgInfo != NULL)																					\
		{																									\
			diruid = msgInfo->msgid;																		\
			msgInfo = NULL;																					\
		}																									\
																											\
		PyObject* pyPos = NULL;																				\
		PyObject* pyDir = NULL;																				\
																											\
																											\
		if(g_componentType == BASEAPP_TYPE)																	\
		{																									\
			PyObject* cellDataDict = PyObject_GetAttrString(this, "cellData");								\
			if(cellDataDict == NULL)																		\
			{																								\
				PyErr_Clear();																				\
				return;																						\
			}																								\
			else																							\
			{																								\
				pyPos = PyDict_GetItemString(cellDataDict, "position");										\
				pyDir = PyDict_GetItemString(cellDataDict, "direction");									\
			}																								\
																											\
			Py_XDECREF(cellDataDict);																		\
			if(pyPos == NULL && pyDir == NULL)																\
			{																								\
				PyErr_Clear();																				\
				return;																						\
			}																								\
		}																									\
		else																								\
		{																									\
			pyPos = PyObject_GetAttrString(this, "position");												\
			pyDir = PyObject_GetAttrString(this, "direction");												\
		}																									\
																											\
																											\
		Vector3 pos, dir;																					\
		script::ScriptVector3::convertPyObjectToVector3(pos, pyPos);										\
		script::ScriptVector3::convertPyObjectToVector3(dir, pyDir);										\
																											\
		if(pScriptModule()->usePropertyDescrAlias() && useAliasID)											\
		{																									\
			ADD_POS_DIR_TO_STREAM_ALIASID(s, pos, dir)														\
		}																									\
		else if(persistentFrame)																				\
		{																									\
			ADD_POS_DIR_TO_PERSISTENT_STREAM(s, pos, dir)												\
		}																									\
		else																								\
		{																									\
			ADD_POS_DIR_TO_STREAM(s, pos, dir)																\
		}																									\
																											\
		if(g_componentType != BASEAPP_TYPE)																	\
		{																									\
			Py_XDECREF(pyPos);																				\
			Py_XDECREF(pyDir);																				\
		}																									\
																											\
	}																										\
																											\
	void CLASS::initProperty(bool isReload)																	\
	{																										\
		/* 组件默认值会根据当前运行侧选择子属性集合，同时需要实体 ID 建立所有权。 */							\
		/* Component defaults select child properties by the active runtime side and need the entity ID for ownership. */		\
		EntityDef::context().currComponentType = g_componentType;											\
		EntityDef::context().currEntityID = id();														\
																									\
		ScriptDefModule::PROPERTYDESCRIPTION_MAP* oldpropers = NULL;										\
		if(isReload)																						\
		{																									\
			ScriptDefModule* pOldScriptDefModule =															\
										EntityDef::findOldScriptModule(pScriptModule_->getName());			\
			if(!pOldScriptDefModule)																		\
			{																								\
				ERROR_MSG(fmt::format("{}::initProperty: not found old_module!\n",							\
					pScriptModule_->getName()));															\
				KBE_ASSERT(false && "Entity::initProperty: not found old_module");							\
			}																								\
																											\
			oldpropers =																					\
											&pOldScriptDefModule->getPropertyDescrs();						\
		}																									\
																											\
		ScriptDefModule::PROPERTYDESCRIPTION_MAP::const_iterator iter = pPropertyDescrs_->begin();			\
		for(; iter != pPropertyDescrs_->end(); ++iter)														\
		{																									\
			PropertyDescription* propertyDescription = iter->second;										\
			DataType* dataType = propertyDescription->getDataType();										\
																											\
			if(oldpropers)																					\
			{																								\
				ScriptDefModule::PROPERTYDESCRIPTION_MAP::iterator olditer = oldpropers->find(iter->first);	\
				if(olditer != oldpropers->end())															\
				{																							\
					if(strcmp(olditer->second->getDataType()->getName(),									\
							propertyDescription->getDataType()->getName()) == 0 &&							\
						strcmp(olditer->second->getDataType()->getName(),									\
							propertyDescription->getDataType()->getName()) == 0)							\
						continue;																			\
				}																							\
			}																								\
																											\
			if(dataType)																					\
			{																								\
				PyObject* defObj = propertyDescription->newDefaultVal();									\
				if(defObj == NULL)																	\
				{																			\
					ERROR_MSG(fmt::format(#CLASS"::initProperty: failed to create default value for {}.\n",	\
						propertyDescription->getName()));										\
					SCRIPT_ERROR_CHECK();													\
					continue;																\
				}																			\
				/* 赋值会先校验组件并可能读取 owner；后续字典合并还依赖父属性区分同类型的多个组件实例。 */				\
				/* Assignment validates the component and may read owner; later dictionary merges also need the parent property to distinguish same-type instances. */\
				if(dataType->type() == DATA_TYPE_ENTITY_COMPONENT)										\
				{																					\
					EntityComponent* pEntityComponent = (EntityComponent*)defObj;						\
					pEntityComponent->pPropertyDescription(propertyDescription);							\
					pEntityComponent->updateOwner(id(), this);										\
				}																					\
																							\
				PyObject_SetAttrString(static_cast<PyObject*>(this),										\
							propertyDescription->getName(), defObj);										\
				Py_DECREF(defObj);																			\
																											\
				/* DEBUG_MSG(fmt::format(#CLASS"::"#CLASS": added [{}] property ref={}.\n",
								propertyDescription->getName(), defObj->ob_refcnt));*/						\
			}																								\
			else																							\
			{																								\
				ERROR_MSG(fmt::format(#CLASS"::initProperty: {} dataType is NULL.\n",						\
					propertyDescription->getName()));														\
			}																								\
		}																									\
																											\
	}																										\


#define ENTITY_CONSTRUCTION(CLASS)																			\
	id_(id),																								\
	pScriptModule_(const_cast<ScriptDefModule*>(pScriptModule)),											\
	pPropertyDescrs_(&pScriptModule_->getPropertyDescrs()),													\
	spaceID_(0),																							\
	scriptTimers_(),																						\
	pyCallbackMgr_(),																						\
	isDestroyed_(false),																					\
	flags_(ENTITY_FLAGS_INITING)																			\


#define ENTITY_DECONSTRUCTION(CLASS)																		\
	DEBUG_MSG(fmt::format("{}::~{}(): {}\n", scriptName(), scriptName(), id_));								\
	pScriptModule_ = NULL;																					\
	isDestroyed_ = true;																					\
	removeFlags(ENTITY_FLAGS_INITING);																		\


#define ENTITY_INIT_PROPERTYS(CLASS)																		\



}
#endif // KBE_ENTITY_MACRO_H
