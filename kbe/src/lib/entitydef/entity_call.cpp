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


#include "entity_call.h"
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

// 获得某个entity的函数地址
EntityCall::GetEntityFunc EntityCall::__getEntityFunc;
EntityCall::FindChannelFunc EntityCall::__findChannelFunc;
EntityCall::EntityCallCallHookFunc*	EntityCall::__hookCallFuncPtr = NULL;
EntityCall::ENTITYCALLS EntityCall::entityCalls;

SCRIPT_METHOD_DECLARE_BEGIN(EntityCall)
	SCRIPT_METHOD_DECLARE("getComponent", pyGetComponent, METH_VARARGS, 0)
SCRIPT_METHOD_DECLARE_END()

SCRIPT_MEMBER_DECLARE_BEGIN(EntityCall)
SCRIPT_MEMBER_DECLARE_END()

SCRIPT_GETSET_DECLARE_BEGIN(EntityCall)
SCRIPT_GETSET_DECLARE_END()
SCRIPT_INIT(EntityCall, 0, 0, 0, 0, 0)		

//-------------------------------------------------------------------------------------
EntityCall::EntityCall(ScriptDefModule* pScriptModule, 
							 const Network::Address* pAddr, 
							 COMPONENT_ID componentID, 
ENTITY_ID eid, ENTITYCALL_TYPE type):
EntityCallAbstract(getScriptType(),
					  pAddr, 
					  componentID, 
					  eid, pScriptModule->getUType(),
					  type),
					  scriptModuleName_(pScriptModule->getName()),
					  pScriptModule_(pScriptModule),
atIdx_(ENTITYCALLS::size_type(-1))
{
	atIdx_ = EntityCall::entityCalls.size();
	EntityCall::entityCalls.push_back(this);

	script::PyGC::incTracing("EntityCall");
}

//-------------------------------------------------------------------------------------
EntityCall::~EntityCall()
{
	//char s[1024];
	//c_str(s, 1024);
	//DEBUG_MSG(fmt::format("EntityCall::~EntityCall(): {}.\n", s));

	KBE_ASSERT(atIdx_ < EntityCall::entityCalls.size());
	KBE_ASSERT(EntityCall::entityCalls[ atIdx_ ] == this);

	// 如果有2个或以上的EntityCall则将最后一个EntityCall移至删除的这个EntityCall所在位置
	EntityCall* pBack = EntityCall::entityCalls.back();
	pBack->_setATIdx(atIdx_);
	EntityCall::entityCalls[atIdx_] = pBack;
	atIdx_ = ENTITYCALLS::size_type(-1);
	EntityCall::entityCalls.pop_back();

	script::PyGC::decTracing("EntityCall");
}

//-------------------------------------------------------------------------------------
RemoteEntityMethod* EntityCall::createRemoteMethod(MethodDescription* pMethodDescription)
{
	if(__hookCallFuncPtr != NULL)
	{
		return (*__hookCallFuncPtr)(pMethodDescription, this);
	}

	return new RemoteEntityMethod(pMethodDescription, this);
}

//-------------------------------------------------------------------------------------
PyObject* EntityCall::onScriptGetAttribute(PyObject* attr)
{
	const char* ccattr = PyUnicode_AsUTF8AndSize(attr, NULL);

	MethodDescription* pMethodDescription = NULL;

	switch(type_)
	{
	case ENTITYCALL_TYPE_CELL:
		pMethodDescription = pScriptModule_->findCellMethodDescription(ccattr);
		break;
	case ENTITYCALL_TYPE_BASE:
		pMethodDescription = pScriptModule_->findBaseMethodDescription(ccattr);
		break;
	case ENTITYCALL_TYPE_CLIENT:
		pMethodDescription = pScriptModule_->findClientMethodDescription(ccattr);
		break;
	case ENTITYCALL_TYPE_CELL_VIA_BASE:
		pMethodDescription = pScriptModule_->findCellMethodDescription(ccattr);
		break;
	case ENTITYCALL_TYPE_BASE_VIA_CELL:
		pMethodDescription = pScriptModule_->findBaseMethodDescription(ccattr);
		break;
	case ENTITYCALL_TYPE_CLIENT_VIA_CELL:
		pMethodDescription = pScriptModule_->findClientMethodDescription(ccattr);
		break;
	case ENTITYCALL_TYPE_CLIENT_VIA_BASE:
		pMethodDescription = pScriptModule_->findClientMethodDescription(ccattr);
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
	else
	{
		// 组件属性必须先转换为携带父属性 UID 的调用代理，后续方法编码才能与普通实体 RPC 明确区分。
		// Component properties must become call proxies carrying the parent-property UID so later method encoding remains distinct from regular entity RPCs.
		PropertyDescription* pComponentPropertyDescription =
			pScriptModule_->findComponentPropertyDescription(ccattr);

		if(pComponentPropertyDescription != NULL)
			return new EntityComponentCall(this, pComponentPropertyDescription);
	}

	// 首先要求名称不能为自己  比如：自身是一个cell， 不能使用cell.cell
	if(strcmp(ccattr, ENTITYCALL_TYPE_TO_NAME_TABLE[type_]) != 0)
	{
		int8 mbtype = -1;

		if(strcmp(ccattr, "cell") == 0)
		{
			if(type_ == ENTITYCALL_TYPE_BASE_VIA_CELL)
				mbtype = ENTITYCALL_TYPE_CELL;
			else
				mbtype = ENTITYCALL_TYPE_CELL_VIA_BASE;
		}
		else if(strcmp(ccattr, "base") == 0)
		{
			if(type_ == ENTITYCALL_TYPE_CELL_VIA_BASE)
				mbtype = ENTITYCALL_TYPE_BASE;
			else
				mbtype = ENTITYCALL_TYPE_BASE_VIA_CELL;
		}
		else if(strcmp(ccattr, "client") == 0)
		{
			if(type_ == ENTITYCALL_TYPE_BASE)
				mbtype = ENTITYCALL_TYPE_CLIENT_VIA_BASE;
			else if(type_ == ENTITYCALL_TYPE_CELL)
				mbtype = ENTITYCALL_TYPE_CLIENT_VIA_CELL;
		}
		
		if(mbtype != -1)
		{
			if(g_componentType != CLIENT_TYPE && g_componentType != BOTS_TYPE)
			{
				return new EntityCall(pScriptModule_, &addr_, componentID_, 
					id_, (ENTITYCALL_TYPE)mbtype);
			}
			else
			{
				Py_INCREF(this);
				return this;
			}
		}
	}
	
	return ScriptObject::onScriptGetAttribute(attr);
}

//-------------------------------------------------------------------------------------
PyObject* EntityCall::tp_repr()
{
	char s[1024];
	c_str(s, 1024);
	return PyUnicode_FromString(s);
}

//-------------------------------------------------------------------------------------
void EntityCall::c_str(char* s, size_t size)
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
PyObject* EntityCall::tp_str()
{
	return tp_repr();
}

//-------------------------------------------------------------------------------------
PyObject* EntityCall::tryGetEntity(COMPONENT_ID componentID, ENTITY_ID entityID)
{
	return __getEntityFunc(componentID, entityID);
}

//-------------------------------------------------------------------------------------
PyObject* EntityCall::__unpickle__(PyObject* self, PyObject* args)
{
	ENTITY_ID eid = 0;
	COMPONENT_ID componentID = 0;
	ENTITY_SCRIPT_UID utype = 0;
	int16 type = 0;

	Py_ssize_t size = PyTuple_Size(args);
	if(size != 4)
	{
		ERROR_MSG("EntityCall::__unpickle__: args error! size != 4.\n");
		S_Return;
	}

	if(!PyArg_ParseTuple(args, "iKHh", &eid, &componentID, &utype, &type))
	{
		ERROR_MSG("EntityCall::__unpickle__: args error!\n");
		S_Return;
	}

	ScriptDefModule* sm = EntityDef::findScriptModule(utype);
	if(sm == NULL)
	{
		ERROR_MSG(fmt::format("EntityCall::__unpickle__: not found utype {}!\n", utype));
		S_Return;
	}

	// COMPONENT_TYPE componentType = ENTITYCALL_COMPONENT_TYPE_MAPPING[(ENTITYCALL_TYPE)type];
	
	PyObject* entity = tryGetEntity(componentID, eid);
	if(entity != NULL)
	{
		Py_INCREF(entity);
		return entity;
	}

	return new EntityCall(sm, NULL, componentID, eid, (ENTITYCALL_TYPE)type);
}

//-------------------------------------------------------------------------------------
void EntityCall::onInstallScript(PyObject* mod)
{
	static PyMethodDef __unpickle__Method = 
	{"EntityCall", (PyCFunction)&EntityCall::__unpickle__, METH_VARARGS, 0};

	PyObject* pyFunc = PyCFunction_New(&__unpickle__Method, NULL);
	script::Pickler::registerUnpickleFunc(pyFunc, "EntityCall");

	Py_DECREF(pyFunc);
}

//-------------------------------------------------------------------------------------
Network::Channel* EntityCall::getChannel(void)
{
	if(__findChannelFunc == NULL)
		return NULL;

	return __findChannelFunc(*this);
}

//-------------------------------------------------------------------------------------
void EntityCall::newCall(Network::Bundle& bundle)
{
	newCall_(bundle);

	// 客户端别名模式必须与生成 SDK 的一字节父 ID 对齐，其他链路保留完整 UID。
	// Client alias mode must match the generated SDK's one-byte parent ID; other paths retain the full UID.
	if (isClient() && pScriptModule_->usePropertyDescrAlias())
		bundle << static_cast<uint8>(0);
	else
		bundle << static_cast<ENTITY_PROPERTY_UID>(0);
}

//-------------------------------------------------------------------------------------
PyObject* EntityCall::pyGetComponent(const std::string& componentName, bool all)
{
	std::vector<EntityComponentCall*> founds =
		EntityComponentCall::getComponents(componentName, this, pScriptModule_);

	if(!all)
	{
		if(founds.empty())
			Py_RETURN_NONE;

		// 属性查询返回新引用；单结果模式必须释放未返回项，避免重复组件类型在每次查询时泄漏代理对象。
		// Attribute lookup returns new references; single-result mode must release unreturned entries to avoid leaking proxies on repeated component types.
		for(size_t i = 1; i < founds.size(); ++i)
			Py_DECREF(founds[i]);

		return founds[0];
	}

	PyObject* pyComponents = PyTuple_New(static_cast<Py_ssize_t>(founds.size()));
	if(pyComponents == NULL)
	{
		for(size_t i = 0; i < founds.size(); ++i)
			Py_DECREF(founds[i]);

		return NULL;
	}

	for(Py_ssize_t i = 0; i < static_cast<Py_ssize_t>(founds.size()); ++i)
	{
		// PyTuple_SET_ITEM 接管新引用，成功路径不得再次递减引用计数。
		// PyTuple_SET_ITEM steals each new reference, so the success path must not decrement it again.
		PyTuple_SET_ITEM(pyComponents, i, founds[static_cast<size_t>(i)]);
	}

	return pyComponents;
}

//-------------------------------------------------------------------------------------
PyObject* EntityCall::__py_pyGetComponent(PyObject* self, PyObject* args)
{
	const Py_ssize_t argsSize = PyTuple_Size(args);
	if(argsSize < 1 || argsSize > 2)
	{
		PyErr_SetString(PyExc_TypeError, "EntityCall.getComponent() requires one or two arguments");
		return NULL;
	}

	const char* componentName = NULL;
	PyObject* pyAll = Py_False;
	if(!PyArg_ParseTuple(args, "s|O:getComponent", &componentName, &pyAll))
		return NULL;

	// 第二参数遵循 Python 真值语义，同时让无效对象通过标准异常返回给脚本而不是吞掉错误。
	// The second argument follows Python truth semantics and propagates standard errors instead of swallowing invalid objects.
	const int all = PyObject_IsTrue(pyAll);
	if(all < 0)
		return NULL;

	return static_cast<EntityCall*>(self)->pyGetComponent(componentName, all != 0);
}

//-------------------------------------------------------------------------------------
void EntityCall::reload()
{
	pScriptModule_ = EntityDef::findScriptModule(scriptModuleName_.c_str());
}

//-------------------------------------------------------------------------------------
}
