/*
实体组件运行时负责承载组件脚本对象、属性编解码和生命周期关联。
The entity-component runtime owns component script objects, property serialization, and lifecycle binding.
*/


#include "entity_call.h"
#include "property.h"
#include "entity_component_call.h"
#include "scriptdef_module.h"
#include "helper/debug_helper.h"
#include "network/channel.h"
#include "pyscript/pickler.h"
#include "pyscript/py_gc.h"
#include "entitydef/method.h"
#include "remote_entity_method.h"
#include "entitydef/entitydef.h"

namespace KBEngine
{
SCRIPT_METHOD_DECLARE_BEGIN(EntityComponentCall)
SCRIPT_METHOD_DECLARE_END()

SCRIPT_MEMBER_DECLARE_BEGIN(EntityComponentCall)
SCRIPT_MEMBER_DECLARE_END()

SCRIPT_GETSET_DECLARE_BEGIN(EntityComponentCall)
SCRIPT_GETSET_DECLARE_END()
SCRIPT_INIT(EntityComponentCall, 0, 0, 0, 0, 0)

//-------------------------------------------------------------------------------------
EntityComponentCall::EntityComponentCall(EntityCall* pEntityCall, PropertyDescription* pComponentPropertyDescription):
	EntityCallAbstract(getScriptType(),
		&pEntityCall->addr(),
		pEntityCall->componentID(),
		pEntityCall->id(), pEntityCall->utype(),
		pEntityCall->type()),
		pEntityCall_(pEntityCall),
		pComponentPropertyDescription_(pComponentPropertyDescription)
{
	Py_INCREF(pEntityCall_);
	script::PyGC::incTracing("EntityComponentCall");
}

//-------------------------------------------------------------------------------------
EntityComponentCall::~EntityComponentCall()
{
	//char s[1024];
	//c_str(s, 1024);
	//DEBUG_MSG(fmt::format("EntityComponentCall::~EntityComponentCall(): {}.\n", s));

	script::PyGC::decTracing("EntityComponentCall");
	Py_DECREF(pEntityCall_);
}

//-------------------------------------------------------------------------------------
RemoteEntityMethod* EntityComponentCall::createRemoteMethod(MethodDescription* pMethodDescription)
{
	// 1.x 抽象调用层未提供 2.x 组件钩子，因此直接创建远程方法包装器。
	// The 1.x abstract call layer does not expose the 2.x component hook, so create the remote-method wrapper directly.
	return new RemoteEntityMethod(pMethodDescription, this);
}

//-------------------------------------------------------------------------------------
Network::Channel* EntityComponentCall::getChannel(void)
{
	// 父调用对象集中管理本地直达与跨 Base/Cell 转发通道，组件代理只增加协议寻址信息。
	// The parent call centralizes direct and Base/Cell-forwarded channels; the component proxy only adds protocol addressing data.
	return pEntityCall_->getChannel();
}

//-------------------------------------------------------------------------------------
PyObject* EntityComponentCall::onScriptGetAttribute(PyObject* attr)
{
	const char* ccattr = PyUnicode_AsUTF8AndSize(attr, NULL);

	MethodDescription* pMethodDescription = NULL;
	ScriptDefModule* pScriptDefModule = pComponentScriptDefModule();

	switch(type_)
	{
	case ENTITYCALL_TYPE_CELL:
		pMethodDescription = pScriptDefModule->findCellMethodDescription(ccattr);
		break;
	case ENTITYCALL_TYPE_BASE:
		pMethodDescription = pScriptDefModule->findBaseMethodDescription(ccattr);
		break;
	case ENTITYCALL_TYPE_CLIENT:
		pMethodDescription = pScriptDefModule->findClientMethodDescription(ccattr);
		break;
	case ENTITYCALL_TYPE_CELL_VIA_BASE:
		pMethodDescription = pScriptDefModule->findCellMethodDescription(ccattr);
		break;
	case ENTITYCALL_TYPE_BASE_VIA_CELL:
		pMethodDescription = pScriptDefModule->findBaseMethodDescription(ccattr);
		break;
	case ENTITYCALL_TYPE_CLIENT_VIA_CELL:
		pMethodDescription = pScriptDefModule->findClientMethodDescription(ccattr);
		break;
	case ENTITYCALL_TYPE_CLIENT_VIA_BASE:
		pMethodDescription = pScriptDefModule->findClientMethodDescription(ccattr);
		break;
	default:
		break;
	};

	if(pMethodDescription != NULL)
	{
		if(g_componentType == CLIENT_TYPE || g_componentType == BOTS_TYPE)
		{
			if(!pMethodDescription->isExposed())
				return ScriptObject::onScriptGetAttribute(attr);
		}

		return createRemoteMethod(pMethodDescription);
	}

	return ScriptObject::onScriptGetAttribute(attr);
}

//-------------------------------------------------------------------------------------
PyObject* EntityComponentCall::__unpickle__(PyObject* self, PyObject* args)
{
	ERROR_MSG("EntityComponentCall::__unpickle__: pickle is not supported!\n");
	S_Return;
}

//-------------------------------------------------------------------------------------
PyObject* EntityComponentCall::tp_repr()
{
	char s[1024];
	c_str(s, 1024);
	return PyUnicode_FromString(s);
}

//-------------------------------------------------------------------------------------
void EntityComponentCall::c_str(char* s, size_t size)
{
	const char * entitycallName =
		(type_ == ENTITYCALL_TYPE_CELL)					? "Cell" :
		(type_ == ENTITYCALL_TYPE_BASE)					? "Base" :
		(type_ == ENTITYCALL_TYPE_CLIENT)				? "Client" :
		(type_ == ENTITYCALL_TYPE_BASE_VIA_CELL)		? "BaseViaCell" :
		(type_ == ENTITYCALL_TYPE_CLIENT_VIA_CELL)		? "ClientViaCell" :
		(type_ == ENTITYCALL_TYPE_CELL_VIA_BASE)		? "CellViaBase" :
		(type_ == ENTITYCALL_TYPE_CLIENT_VIA_BASE)		? "ClientViaBase" : "???";

	Network::Channel* pChannel = getChannel();

	kbe_snprintf(s, size, "%s id:%d, utype:%u, component=%s[%" PRIu64 "], addr: %s.",
		entitycallName, id_,  utype_,
		COMPONENT_NAME_EX(ENTITYCALL_COMPONENT_TYPE_MAPPING[type_]),
		componentID_, (pChannel && pChannel->pEndPoint()) ? pChannel->addr().c_str() : "None");
}

//-------------------------------------------------------------------------------------
PyObject* EntityComponentCall::tp_str()
{
	return tp_repr();
}

//-------------------------------------------------------------------------------------
void EntityComponentCall::onInstallScript(PyObject* mod)
{
}

//-------------------------------------------------------------------------------------
ScriptDefModule* EntityComponentCall::pComponentScriptDefModule()
{
	EntityComponentType* pEntityComponentType = static_cast<EntityComponentType*>(pComponentPropertyDescription_->getDataType());
	return pEntityComponentType->pScriptDefModule();
}

//-------------------------------------------------------------------------------------
void EntityComponentCall::newCall(Network::Bundle& bundle)
{
	// 先构造不含父 UID 的寻址头，再追加唯一的组件属性 ID，避免转发路径重复写入。
	// Build the address header without a parent UID, then append the sole component property ID to prevent duplicates during forwarding.
	newCall_(bundle);

	ScriptDefModule* pScriptDefModule = pComponentScriptDefModule();
	if (isClient() && pScriptDefModule->usePropertyDescrAlias())
		bundle << pComponentPropertyDescription_->aliasIDAsUint8();
	else
		bundle << pComponentPropertyDescription_->getUType();
}

//-------------------------------------------------------------------------------------
std::vector<EntityComponentCall*> EntityComponentCall::getComponents(const std::string& name, EntityCall* pEntityCall, ScriptDefModule* pEntityScriptDescrs)
{
	std::vector<EntityComponentCall*> founds;

	ScriptDefModule::COMPONENTDESCRIPTION_MAP& componentDescrs = pEntityScriptDescrs->getComponentDescrs();

	ScriptDefModule::COMPONENTDESCRIPTION_MAP::iterator comps_iter = componentDescrs.begin();
	for (; comps_iter != componentDescrs.end(); ++comps_iter)
	{
		if (name != comps_iter->second->getName())
			continue;

		if (pEntityCall->isBase())
		{
			if (!comps_iter->second->hasBase())
				continue;
		}
		else if (pEntityCall->isCell())
		{
			if (!comps_iter->second->hasCell())
				continue;
		}
		else
		{
			if (!comps_iter->second->hasClient())
				continue;
		}

		PyObject* pyObj = PyObject_GetAttrString(pEntityCall, comps_iter->first.c_str());
		if (pyObj)
		{
			founds.push_back((EntityComponentCall*)pyObj);
		}
		else
		{
			SCRIPT_ERROR_CHECK();
		}
	}

	return founds;
}

//-------------------------------------------------------------------------------------
}
