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


#include "clientapp.h"
#include "entity.h"
#include "config.h"
#include "clientobjectbase.h"
#include "moveto_point_handler.h"	
#include "entitydef/entity_call.h"
#include "entitydef/entity_component.h"
#include "network/channel.h"	
#include "network/bundle.h"	
#include "network/fixed_messages.h"
#include "pyscript/py_gc.h"

#include "../../../server/baseapp/baseapp_interface.h"
#include "../../../server/cellapp/cellapp_interface.h"

#ifndef CODE_INLINE
#include "entity.inl"
#endif

namespace KBEngine{
namespace client
{

//-------------------------------------------------------------------------------------
CLIENT_ENTITY_METHOD_DECLARE_BEGIN(ClientApp, Entity)
SCRIPT_METHOD_DECLARE("moveToPoint",				pyMoveToPoint,					METH_VARARGS,				0)
SCRIPT_METHOD_DECLARE("cancelController",			pyCancelController,				METH_VARARGS,				0)
SCRIPT_METHOD_DECLARE("isPlayer",					pyIsPlayer,						METH_VARARGS,				0)
SCRIPT_METHOD_DECLARE("addTimer",					pyAddTimer,						METH_VARARGS,				0)
SCRIPT_METHOD_DECLARE("delTimer",					pyDelTimer,						METH_VARARGS,				0)
CLIENT_ENTITY_METHOD_DECLARE_END()

SCRIPT_MEMBER_DECLARE_BEGIN(Entity)
SCRIPT_MEMBER_DECLARE_END()

CLIENT_ENTITY_GETSET_DECLARE_BEGIN(Entity)
SCRIPT_GET_DECLARE("base",							pyGetBaseEntityCall,			0,					0)
SCRIPT_GET_DECLARE("cell",							pyGetCellEntityCall,			0,					0)
SCRIPT_GET_DECLARE("clientapp",						pyGetClientApp,					0,					0)
SCRIPT_GETSET_DECLARE("position",					pyGetPosition,					pySetPosition,		0,		0)
SCRIPT_GETSET_DECLARE("direction",					pyGetDirection,					pySetDirection,		0,		0)
SCRIPT_GETSET_DECLARE("velocity",					pyGetMoveSpeed,					pySetMoveSpeed,		0,		0)
CLIENT_ENTITY_GETSET_DECLARE_END()
BASE_SCRIPT_INIT(Entity, 0, 0, 0, 0, 0)	
	
//-------------------------------------------------------------------------------------
Entity::Entity(ENTITY_ID id, const ScriptDefModule* pScriptModule, EntityCall* base, EntityCall* cell):
ScriptObject(getScriptType(), true),
ENTITY_CONSTRUCTION(Entity),
cellEntityCall_(cell),
baseEntityCall_(base),
position_(),
serverPosition_(),
direction_(),
clientPos_(FLT_MAX, FLT_MAX, FLT_MAX),
clientDir_(FLT_MAX, FLT_MAX, FLT_MAX),
pClientApp_(NULL),
aspect_(id),
velocity_(3.0f),
enterworld_(false),
isOnGround_(true),
pMoveHandlerID_(0),
inited_(false),
isControlled_(false)
{
	ENTITY_INIT_PROPERTYS(Entity);
	script::PyGC::incTracing("Entity");
}

//-------------------------------------------------------------------------------------
Entity::~Entity()
{
	stopMove();

	enterworld_ = false;
	ENTITY_DECONSTRUCTION(Entity);
	S_RELEASE(cellEntityCall_);
	S_RELEASE(baseEntityCall_);

	script::PyGC::decTracing("Entity");
	
	if (pClientApp_ != NULL)
	{
		if(pClientApp_->pEntities())
			pClientApp_->pEntities()->pGetbages()->erase(id());

		Py_DECREF(pClientApp_);
		pClientApp_ = NULL;
	}
}	

//-------------------------------------------------------------------------------------
void Entity::pClientApp(ClientObjectBase* p)
{ 
	if (p == pClientApp_)
		return;

	// Move callbacks belong to the current ClientObject timer set. Cancel them before changing
	// owners; otherwise a later callback release can re-enter an Entity with no valid owner.
	// 移动回调归当前 ClientObject 的定时器集合所有；切换 owner 前取消，避免延迟释放重入无有效 owner 的 Entity。
	if (pClientApp_ != NULL)
		stopMove();

	Py_XINCREF(p);
	ClientObjectBase* pPrevious = pClientApp_;
	pClientApp_ = p;
	Py_XDECREF(pPrevious);
}

//-------------------------------------------------------------------------------------
PyObject* Entity::pyGetBaseEntityCall()
{ 
	EntityCall* entitycall = baseEntityCall();
	if(entitycall == NULL)
		S_Return;

	Py_INCREF(entitycall);
	return entitycall; 
}

//-------------------------------------------------------------------------------------
PyObject* Entity::pyGetCellEntityCall()
{ 
	EntityCall* entitycall = cellEntityCall();
	if(entitycall == NULL)
		S_Return;

	Py_INCREF(entitycall);
	return entitycall; 
}

//-------------------------------------------------------------------------------------
PyObject* Entity::pyGetClientApp()
{ 
	ClientObjectBase* app = pClientApp();
	if(app == NULL)
		S_Return;

	Py_INCREF(app);
	return app; 
}

//-------------------------------------------------------------------------------------
PyObject* Entity::onScriptGetAttribute(PyObject* attr)
{
	DEBUG_OP_ATTRIBUTE("get", attr)
	return ScriptObject::onScriptGetAttribute(attr);
}	

//-------------------------------------------------------------------------------------
void Entity::onInitializeScript()
{
	// 客户端实体当前无需额外初始化；统一钩子确保组件脚本与 owner 均已就绪。
	// Client entities currently need no extra work; the shared hook runs only after component scripts and owners are ready.
}

//-------------------------------------------------------------------------------------
void Entity::onDefDataChanged(EntityComponent* pEntityComponent,
	const PropertyDescription* propertyDescription, PyObject* pyData)
{
	(void)pEntityComponent;
	(void)propertyDescription;
	(void)pyData;
}

//-------------------------------------------------------------------------------------
void Entity::onRemoteMethodCall(Network::Channel* pChannel, MemoryStream& s)
{
	ENTITY_METHOD_UID utype = 0;
	MethodDescription* pMethodDescription = NULL;
	ScriptDefModule* pScriptModule = pScriptModule_;
	PropertyDescription* pComponentPropertyDescription = NULL;

	// Nex 2.8 在方法标识前携带组件属性标识，0 表示调用实体本身。
	// Nex 2.8 prefixes every method identifier with a component-property identifier; zero targets the entity itself.
	if (pScriptModule->usePropertyDescrAlias())
	{
		uint8 componentPropertyAliasID = 0;
		s >> componentPropertyAliasID;

		if (componentPropertyAliasID > 0)
			pComponentPropertyDescription = pScriptModule->findAliasPropertyDescription(componentPropertyAliasID);
	}
	else
	{
		ENTITY_PROPERTY_UID componentPropertyUID = 0;
		s >> componentPropertyUID;

		if (componentPropertyUID > 0)
			pComponentPropertyDescription = pScriptModule->findClientPropertyDescription(componentPropertyUID);
	}

	PyObject* pyCallObject = this;
	if (pComponentPropertyDescription)
	{
		DataType* pDataType = pComponentPropertyDescription->getDataType();
		if (!pDataType || pDataType->type() != DATA_TYPE_ENTITY_COMPONENT)
		{
			s.done();
			ERROR_MSG(fmt::format("{}::onRemoteMethodCall: property[{}] is not an entity component. entityID={}.\n",
				this->scriptName(), pComponentPropertyDescription->getName(), id_));
			return;
		}

		pScriptModule = static_cast<EntityComponentType*>(pDataType)->pScriptDefModule();
		pyCallObject = PyObject_GetAttrString(this, pComponentPropertyDescription->getName());
		if (!pyCallObject)
		{
			s.done();
			SCRIPT_ERROR_CHECK();
			return;
		}
	}

	if(pScriptModule->useMethodDescrAlias())
	{
		ENTITY_DEF_ALIASID aliasID;
		s >> aliasID;
		pMethodDescription = pScriptModule->findAliasMethodDescription(aliasID);
		utype = aliasID;
	}
	else
	{
		s >> utype;
		pMethodDescription = pScriptModule->findClientMethodDescription(utype);
	}

	if(pMethodDescription == NULL)
	{
		s.done();
		ERROR_MSG(fmt::format("{2}::onRemoteMethodCall: can't found method. utype={0}, methodName=unknown, callerID:{1}.\n", 
			utype, id_, this->scriptName()));

		if (pyCallObject != static_cast<PyObject*>(this))
			Py_DECREF(pyCallObject);

		return;
	}

	if(g_debugEntity)
	{
		DEBUG_MSG(fmt::format("{3}::onRemoteMethodCall: {0}, {3}::{1}(utype={2}).\n", 
			id_, (pMethodDescription ? pMethodDescription->getName() : "unknown"), utype, this->scriptName()));
	}

	PyObject* pyFunc = PyObject_GetAttrString(pyCallObject, const_cast<char*>
						(pMethodDescription->getName()));

	if(pMethodDescription != NULL)
	{
		if(pMethodDescription->getArgSize() == 0)
		{
			pMethodDescription->call(pyFunc, NULL);
		}
		else
		{
			PyObject* pyargs = pMethodDescription->createFromStream(&s);
			if(pyargs)
			{
				pMethodDescription->call(pyFunc, pyargs);
				Py_DECREF(pyargs);
			}
			else
			{
				SCRIPT_ERROR_CHECK();
			}
		}
	}
	
	Py_XDECREF(pyFunc);

	if (pyCallObject != static_cast<PyObject*>(this))
		Py_DECREF(pyCallObject);

	SCRIPT_ERROR_CHECK();
}

//-------------------------------------------------------------------------------------
void Entity::onUpdatePropertys(MemoryStream& s)
{
	// 客户端属性更新可能创建组件实例，先提供 app、entity 和 Client 域上下文。
	// Client property updates may construct component instances, so provide app, entity, and Client-domain context first.
	EntityDef::context().currClientappID = pClientApp_->appID();
	EntityDef::context().currEntityID = id();
	EntityDef::context().currComponentType = CLIENT_TYPE;

	ENTITY_PROPERTY_UID posuid = ENTITY_BASE_PROPERTY_UTYPE_POSITION_XYZ;
	ENTITY_PROPERTY_UID diruid = ENTITY_BASE_PROPERTY_UTYPE_DIRECTION_ROLL_PITCH_YAW;
	ENTITY_PROPERTY_UID spaceuid = ENTITY_BASE_PROPERTY_UTYPE_SPACEID;

	if(!pScriptModule_->usePropertyDescrAlias())
	{
		Network::FixedMessages::MSGInfo* msgInfo =
					Network::FixedMessages::getSingleton().isFixed("Property::position");

		if(msgInfo != NULL)
			posuid = msgInfo->msgid;

		msgInfo = Network::FixedMessages::getSingleton().isFixed("Property::direction");
		if(msgInfo != NULL)
			diruid = msgInfo->msgid;

		msgInfo = Network::FixedMessages::getSingleton().isFixed("Property::spaceID");
		if(msgInfo != NULL)
			spaceuid = msgInfo->msgid;
	}
	else
	{
		posuid = ENTITY_BASE_PROPERTY_ALIASID_POSITION_XYZ;
		diruid = ENTITY_BASE_PROPERTY_ALIASID_DIRECTION_ROLL_PITCH_YAW;
		spaceuid = ENTITY_BASE_PROPERTY_ALIASID_SPACEID;
	}

	while(s.length() > 0)
	{
		ENTITY_PROPERTY_UID uid;
		ENTITY_PROPERTY_UID childUID;
		uint8 aliasID = 0;
		uint8 childAliasID = 0;
		PropertyDescription* pPropertyDescription = NULL;
		PyObject* setToObj = this;

		if(pScriptModule_->usePropertyDescrAlias())
		{
			s >> aliasID;
			s >> childAliasID;
			uid = aliasID;
			childUID = childAliasID;
		}
		else
		{
			s >> uid;
			s >> childUID;
		}

		// 父属性为零表示普通实体属性；非零表示组件属性，随后由子 ID 定位组件内部字段。
		// A zero parent identifies a direct entity property; a nonzero parent selects a component before its child property.
		if(uid == 0)
		{
			if(childUID == posuid)
			{
				Position3D pos;

#ifdef CLIENT_NO_FLOAT		
				int32 x, y, z;
				s >> x >> y >> z;

				pos.x = (float)x;
				pos.y = (float)y;
				pos.z = (float)z;
#else
				s >> pos.x >> pos.y >> pos.z;
#endif
				position(pos);
				clientPos(pos);
				continue;
			}
			else if(childUID == diruid)
			{
				Direction3D dir;

#ifdef CLIENT_NO_FLOAT		
				int32 x, y, z;
				s >> x >> y >> z;

				dir.roll((float)x);
				dir.pitch((float)y);
				dir.yaw((float)z);
#else
				float yaw, pitch, roll;
				s >> roll >> pitch >> yaw;
				dir.yaw(yaw);
				dir.pitch(pitch);
				dir.roll(roll);
#endif

				direction(dir);
				clientDir(dir);
				continue;
			}
			else if(childUID == spaceuid)
			{
				SPACE_ID ispaceID;
				s >> ispaceID;
				spaceID(ispaceID);
				continue;
			}

			if(pScriptModule_->usePropertyDescrAlias())
				pPropertyDescription = pScriptModule()->findAliasPropertyDescription(childAliasID);
			else
				pPropertyDescription = pScriptModule()->findClientPropertyDescription(childUID);
		}
		else
		{
			if(pScriptModule_->usePropertyDescrAlias())
				pPropertyDescription = pScriptModule()->findAliasPropertyDescription(aliasID);
			else
				pPropertyDescription = pScriptModule()->findClientPropertyDescription(uid);

			if(!pPropertyDescription || pPropertyDescription->getDataType()->type() != DATA_TYPE_ENTITY_COMPONENT)
			{
				s.done();
				ERROR_MSG(fmt::format("{}::onUpdatePropertys: component parent not found, uid={}, aliasID={}!\n",
					pScriptModule_->getName(), uid, aliasID));
				return;
			}

			setToObj = PyObject_GetAttrString(this, pPropertyDescription->getName());
			if(!setToObj || !PyObject_TypeCheck(setToObj, EntityComponent::getScriptType()))
			{
				s.done();
				Py_XDECREF(setToObj);
				SCRIPT_ERROR_CHECK();
				ERROR_MSG(fmt::format("{}::onUpdatePropertys: component {} is unavailable!\n",
					pScriptModule_->getName(), pPropertyDescription->getName()));
				return;
			}

			pPropertyDescription = static_cast<EntityComponent*>(setToObj)->getProperty(childUID);
		}

		if(pPropertyDescription == NULL)
		{
			s.done();
			ERROR_MSG(fmt::format("Entity::onUpdatePropertys: property not found, parent={}, child={}\n", uid, childUID));
			if(setToObj != static_cast<PyObject*>(this))
				Py_DECREF(setToObj);
			return;
		}

		PyObject* pyobj = pPropertyDescription->createFromStream(&s);
		if(!pyobj)
		{
			s.done();
			if(setToObj != static_cast<PyObject*>(this))
				Py_DECREF(setToObj);
			SCRIPT_ERROR_CHECK();
			return;
		}

		PyObject* pyOld = PyObject_GetAttrString(setToObj, pPropertyDescription->getName());
		if(!pyOld)
		{
			SCRIPT_ERROR_CHECK();
			pyOld = Py_None;
			Py_INCREF(pyOld);
		}

		PyObject_SetAttrString(setToObj, pPropertyDescription->getName(), pyobj);

		bool willCallScript = pPropertyDescription->hasBase() ? inited_ : enterworld_;
		if (willCallScript)
		{
			std::string setname = "set_";
			setname += pPropertyDescription->getName();

			SCRIPT_OBJECT_CALL_ARGS1(setToObj, const_cast<char*>(setname.c_str()),
				const_cast<char*>("O"), pyOld, false);
		}

		Py_DECREF(pyobj);
		Py_DECREF(pyOld);
		if(setToObj != static_cast<PyObject*>(this))
			Py_DECREF(setToObj);
		SCRIPT_ERROR_CHECK();
	}
}

//-------------------------------------------------------------------------------------
void Entity::writeToDB(void* data, void* extra1, void* extra2)
{
}

//-------------------------------------------------------------------------------------
int Entity::pySetPosition(PyObject *value)
{
	if(!script::ScriptVector3::check(value))
		return -1;

	script::ScriptVector3::convertPyObjectToVector3(position(), value);
	onPositionChanged();
	return 0;
}

//-------------------------------------------------------------------------------------
PyObject* Entity::pyGetPosition()
{
	return new script::ScriptVector3(&position(), NULL);
}

//-------------------------------------------------------------------------------------
void Entity::onPositionChanged()
{
	if(pyIsPlayer())
		return;

	EventData_PositionChanged eventdata;
	eventdata.x = position_.x;
	eventdata.y = position_.y;
	eventdata.z = position_.z;
	eventdata.speed = velocity_;
	
	eventdata.entityID = id();

	pClientApp_->fireEvent(&eventdata);
}

//-------------------------------------------------------------------------------------
int Entity::pySetDirection(PyObject *value)
{
	if(PySequence_Check(value) <= 0)
	{
		PyErr_Format(PyExc_TypeError, "args of direction is must a sequence.");
		PyErr_PrintEx(0);
		return -1;
	}

	Py_ssize_t size = PySequence_Size(value);
	if(size != 3)
	{
		PyErr_Format(PyExc_TypeError, "len(direction) != 3. can't set.");
		PyErr_PrintEx(0);
		return -1;
	}

	Direction3D& dir = direction();
	PyObject* pyItem = PySequence_GetItem(value, 0);
	dir.roll(float(PyFloat_AsDouble(pyItem)));
	Py_DECREF(pyItem);
	pyItem = PySequence_GetItem(value, 1);
	dir.pitch(float(PyFloat_AsDouble(pyItem)));
	Py_DECREF(pyItem);
	pyItem = PySequence_GetItem(value, 2);
	dir.yaw(float(PyFloat_AsDouble(pyItem)));
	Py_DECREF(pyItem);

	onDirectionChanged();
	return 0;
}

//-------------------------------------------------------------------------------------
PyObject* Entity::pyGetDirection()
{
	return new script::ScriptVector3(&direction().dir, NULL);
}

//-------------------------------------------------------------------------------------
void Entity::onDirectionChanged()
{
	if(pyIsPlayer())
		return;

	EventData_DirectionChanged eventdata;
	eventdata.yaw = direction_.yaw();
	eventdata.pitch = direction_.pitch();
	eventdata.roll = direction_.roll();
	eventdata.entityID = id();

	pClientApp_->fireEvent(&eventdata);
}

//-------------------------------------------------------------------------------------
int Entity::pySetMoveSpeed(PyObject *value)
{
	moveSpeed((float)PyFloat_AsDouble(value));
	return 0;
}

//-------------------------------------------------------------------------------------
PyObject* Entity::pyGetMoveSpeed()
{
	return PyFloat_FromDouble(velocity_);
}

//-------------------------------------------------------------------------------------
void Entity::onMoveSpeedChanged()
{
	EventData_MoveSpeedChanged eventdata;
	eventdata.speed = velocity_;
	eventdata.entityID = id();

	pClientApp_->fireEvent(&eventdata);
}

//-------------------------------------------------------------------------------------
void Entity::onEnterWorld()
{
	SCOPED_PROFILE(SCRIPTCALL_PROFILE);
	enterworld_ = true;
	SCRIPT_OBJECT_CALL_ARGS0(this, const_cast<char*>("onEnterWorld"), false);
}

//-------------------------------------------------------------------------------------
void Entity::onLeaveWorld()
{
	SCOPED_PROFILE(SCRIPTCALL_PROFILE);
	enterworld_ = false;
	spaceID(0);
	SCRIPT_OBJECT_CALL_ARGS0(this, const_cast<char*>("onLeaveWorld"), false);
}

//-------------------------------------------------------------------------------------
void Entity::onEnterSpace()
{
	this->stopMove();
	SCOPED_PROFILE(SCRIPTCALL_PROFILE);
	SCRIPT_OBJECT_CALL_ARGS0(this, const_cast<char*>("onEnterSpace"), false);
}

//-------------------------------------------------------------------------------------
void Entity::onLeaveSpace()
{
	SCOPED_PROFILE(SCRIPTCALL_PROFILE);
	spaceID(0);
	SCRIPT_OBJECT_CALL_ARGS0(this, const_cast<char*>("onLeaveSpace"), false);
	this->stopMove();
}

//-------------------------------------------------------------------------------------
PyObject* Entity::__py_pyDestroyEntity(PyObject* self, PyObject* args, PyObject * kwargs)
{
	S_Return;
}

//-------------------------------------------------------------------------------------
void Entity::addCellDataToStream(COMPONENT_TYPE sendTo, uint32 flags, MemoryStream* mstream, bool useAliasID)
{
}

//-------------------------------------------------------------------------------------
void Entity::onBecomePlayer()
{
	std::string moduleName = "Player";
	moduleName += this->pScriptModule_->getName();

	PyObject* pyModule = 
		PyImport_ImportModule(const_cast<char*>(this->pScriptModule_->getName()));

	if(pyModule == NULL)
	{
		SCRIPT_ERROR_CHECK();
	}
	else
	{
		PyObject* pyClass = 
			PyObject_GetAttrString(pyModule, const_cast<char *>(moduleName.c_str()));

		if(pyClass == NULL)
		{
			// 不在强制需要实现Player**类
			PyErr_Clear();
		}
		else
		{
			PyObject_SetAttrString(static_cast<PyObject*>(this), "__class__", pyClass);
			SCRIPT_ERROR_CHECK();
		}

		S_RELEASE(pyModule);
	}

	SCOPED_PROFILE(SCRIPTCALL_PROFILE);
	SCRIPT_OBJECT_CALL_ARGS0(this, const_cast<char*>("onBecomePlayer"), false);
}

//-------------------------------------------------------------------------------------
void Entity::onBecomeNonPlayer()
{
	if(!enterworld_)
		return;
	
	SCOPED_PROFILE(SCRIPTCALL_PROFILE);
	SCRIPT_OBJECT_CALL_ARGS0(this, const_cast<char*>("onBecomeNonPlayer"), false);

	PyObject_SetAttrString(static_cast<PyObject*>(this), "__class__", (PyObject*)this->pScriptModule_->getScriptType());
	SCRIPT_ERROR_CHECK();
}

//-------------------------------------------------------------------------------------
bool Entity::stopMove()
{
	if(pMoveHandlerID_ > 0)
	{
		// Clear first because cancel() synchronously releases its handler and may re-enter Python.
		// cancel() 会同步释放 handler 并可能重入 Python，因此先清零以保证停止操作幂等。
		ScriptID moveHandlerID = pMoveHandlerID_;
		pMoveHandlerID_ = 0;

		if (pClientApp_ != NULL)
			pClientApp_->scriptCallbacks().delCallback(moveHandlerID);

		return true;
	}

	return false;
}

//-------------------------------------------------------------------------------------
uint32 Entity::moveToPoint(const Position3D& destination, float velocity, float distance, PyObject* userData, 
						 bool faceMovement, bool moveVertically)
{
	stopMove();

	int hertz = 0;
	if(g_componentType == BOTS_TYPE)
		hertz = g_kbeSrvConfig.gameUpdateHertz();
	else
		hertz = Config::getSingleton().gameUpdateHertz();

	velocity = velocity / hertz;

	pMoveHandlerID_ = pClientApp_->scriptCallbacks().addCallback(0.0f, 0.1f, new MoveToPointHandler(pClientApp_->scriptCallbacks(), this, 0, destination, velocity, 
		distance, faceMovement, moveVertically, userData));

	return pMoveHandlerID_;
}

//-------------------------------------------------------------------------------------
PyObject* Entity::pyMoveToPoint(PyObject_ptr pyDestination, float velocity, float distance, PyObject_ptr userData,
								 int32 faceMovement, int32 moveVertically)
{
	if(this->isDestroyed())
	{
		PyErr_Format(PyExc_AssertionError, "%s::moveToPoint: %d is destroyed!\n",		
			scriptName(), id());		
		PyErr_PrintEx(0);
		return 0;
	}

	Position3D destination;

	if(!PySequence_Check(pyDestination))
	{
		PyErr_Format(PyExc_TypeError, "%s::moveToPoint: args1(position) not is PySequence!", scriptName());
		PyErr_PrintEx(0);
		return 0;
	}

	if(PySequence_Size(pyDestination) != 3)
	{
		PyErr_Format(PyExc_TypeError, "%s::moveToPoint: args1(position) invalid!", scriptName());
		PyErr_PrintEx(0);
		return 0;
	}

	// 将坐标信息提取出来
	script::ScriptVector3::convertPyObjectToVector3(destination, pyDestination);
	Py_INCREF(userData);

	return PyLong_FromLong(moveToPoint(destination, velocity, distance, userData, faceMovement > 0, moveVertically > 0));
}

//-------------------------------------------------------------------------------------
void Entity::onMove(uint32 controllerId, int layer, const Position3D& oldPos, PyObject* userarg)
{
	if(this->isDestroyed())
		return;

	AUTO_SCOPED_PROFILE("onMove");

	SCRIPT_OBJECT_CALL_ARGS2(this, const_cast<char*>("onMove"), 
		const_cast<char*>("IO"), controllerId, userarg, false);
}

//-------------------------------------------------------------------------------------
void Entity::onMoveOver(uint32 controllerId, int layer, const Position3D& oldPos, PyObject* userarg)
{
	if(this->isDestroyed())
		return;

	stopMove();

	SCOPED_PROFILE(SCRIPTCALL_PROFILE);
	SCRIPT_OBJECT_CALL_ARGS2(this, const_cast<char*>("onMoveOver"), 
		const_cast<char*>("IO"), controllerId, userarg, false);
}

//-------------------------------------------------------------------------------------
void Entity::onMoveFailure(uint32 controllerId, PyObject* userarg)
{
	if(this->isDestroyed())
		return;

	stopMove();

	SCOPED_PROFILE(SCRIPTCALL_PROFILE);
	SCRIPT_OBJECT_CALL_ARGS2(this, const_cast<char*>("onMoveFailure"), 
		const_cast<char*>("IO"), controllerId, userarg, false);
}

//-------------------------------------------------------------------------------------
void Entity::cancelController(uint32 id)
{
	if(this->isDestroyed())
	{
		return;
	}

	// 暂时只有回调, 主要是因为用在了移动中，当前可能不是非常合适
	if(id == (uint32)pMoveHandlerID_)
		this->stopMove();
}

//-------------------------------------------------------------------------------------
PyObject* Entity::__py_pyCancelController(PyObject* self, PyObject* args)
{
	uint16 currargsSize = (uint16)PyTuple_Size(args);
	Entity* pobj = static_cast<Entity*>(self);

	uint32 id = 0;
	PyObject* pyargobj = NULL;

	if(currargsSize != 1)
	{
		PyErr_Format(PyExc_AssertionError, "%s::cancel: args require 1 args(controllerID|int or \"Movement\"|str), gived %d! is script[%s].\n",								
			pobj->scriptName(), currargsSize);														
																																
		PyErr_PrintEx(0);																										
		return 0;																								
	}

	if(!PyArg_ParseTuple(args, "O", &pyargobj))
	{
		PyErr_Format(PyExc_TypeError, "%s::cancel: args(controllerID|int or \"Movement\"|str) error!", pobj->scriptName());
		PyErr_PrintEx(0);
		return 0;
	}
	
	if(pyargobj == NULL)
	{
		PyErr_Format(PyExc_TypeError, "%s::cancel: args(controllerID|int or \"Movement\"|str) error!", pobj->scriptName());
		PyErr_PrintEx(0);
		return 0;
	}

	if(PyUnicode_Check(pyargobj))
	{
		if (strcmp(PyUnicode_AsUTF8AndSize(pyargobj, NULL), "Movement") == 0)
		{
			pobj->stopMove();
		}
		else
		{
			PyErr_Format(PyExc_TypeError, "%s::cancel: args not is \"Movement\"!", pobj->scriptName());
			PyErr_PrintEx(0);
			return 0;
		}

		S_Return;
	}
	else
	{
		if(!PyLong_Check(pyargobj))
		{
			PyErr_Format(PyExc_TypeError, "%s::cancel: args(controllerID|int) error!", pobj->scriptName());
			PyErr_PrintEx(0);
			return 0;
		}

		id = PyLong_AsLong(pyargobj);
	}

	pobj->cancelController(id);
	S_Return;
}

//-------------------------------------------------------------------------------------
bool Entity::isPlayer()
{
	return id() == pClientApp_->entityID();
}

//-------------------------------------------------------------------------------------
PyObject* Entity::pyIsPlayer()
{
	if (isPlayer())
		Py_RETURN_TRUE;
	else
		Py_RETURN_FALSE;
}

//-------------------------------------------------------------------------------------
void Entity::callPropertysSetMethods()
{
	ScriptDefModule::PROPERTYDESCRIPTION_MAP &clientProperties = pScriptModule_->getClientPropertyDescriptions();
	ScriptDefModule::PROPERTYDESCRIPTION_MAP::iterator iter = clientProperties.begin();
	for (; iter != clientProperties.end(); ++iter)
	{
		bool willCallScript = false;
		PyObject* pyOld = PyObject_GetAttrString(this, iter->first.c_str());

		if (iter->second->hasBase())
		{
			if (inited_ && !enterworld_)
			{
				willCallScript = true;
			}
		}
		else
		{
			if (enterworld_)
			{
				willCallScript = true;
			}
		}

		if (willCallScript)
		{
			std::string setname = "set_";
			setname += iter->second->getName();

			SCRIPT_OBJECT_CALL_ARGS1(this, const_cast<char*>(setname.c_str()),
				const_cast<char*>("O"), pyOld, false);
		}

		Py_DECREF(pyOld);
		SCRIPT_ERROR_CHECK();
	}
}

//-------------------------------------------------------------------------------------
void Entity::onTimer(ScriptID timerID, int useraAgs)
{
	SCOPED_PROFILE(ONTIMER_PROFILE);
	
	PyObject* pyResult = PyObject_CallMethod(this, const_cast<char*>("onTimer"),
		const_cast<char*>("Ii"), timerID, useraAgs);

	if (pyResult != NULL)
		Py_DECREF(pyResult);
	else
		SCRIPT_ERROR_CHECK();
}

//-------------------------------------------------------------------------------------
void Entity::onControlled(bool p_controlled)
{
    isControlled_ = p_controlled;

    PyObject *pyval = p_controlled ? Py_True : Py_False;
    SCRIPT_OBJECT_CALL_ARGS1(this, const_cast<char*>("onControlled"), const_cast<char*>("O"), pyval, false);
}

//-------------------------------------------------------------------------------------
}
}


