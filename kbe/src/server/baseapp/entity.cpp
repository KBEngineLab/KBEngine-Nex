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

#include "baseapp.h"
#include "entity.h"
#include "server/cell_backup.h"
#include "server/component_routing_guard.h"
#include "server/teleport_status.h"
#include "profile.h"
#include "entity_messages_forward_handler.h"
#include "pyscript/py_gc.h"
#include "entitydef/entity_call.h"
#include "entitydef/entity_events.h"
#include "server/script_stage_timing.h"
#include "network/channel.h"	
#include "network/fixed_messages.h"
#include "client_lib/client_interface.h"
#include "common/sha1.h"

#ifndef CODE_INLINE
#include "entity.inl"
#endif

#include "../../server/baseapp/baseapp_interface.h"
#include "../../server/cellapp/cellapp_interface.h"
#include "../../server/dbmgr/dbmgr_interface.h"

namespace KBEngine{

namespace
{
bool isCurrentCellChannel(Network::Channel* channel, const EntityCall* cell)
{
	if (!cell || !channel || !channel->isInternal() || channel->isDestroyed() || channel->condemn() != 0)
		return false;
	const auto id = cell->componentID();
	auto* peer = Components::getSingleton().findComponent(CELLAPP_TYPE, id);
	return Security::isBoundBidirectionalComponentSource(id, peer, channel, channel->componentID());
}
}

ENTITY_METHOD_DECLARE_BEGIN(Baseapp, Entity)
SCRIPT_METHOD_DECLARE("createCellEntity",				createCellEntity,				METH_VARARGS,			0)
SCRIPT_METHOD_DECLARE("createCellEntityInNewSpace",		createCellEntityInNewSpace,		METH_VARARGS,			0)
SCRIPT_METHOD_DECLARE("destroyCellEntity",				pyDestroyCellEntity,			METH_VARARGS,			0)
ENTITY_METHOD_DECLARE_END()

SCRIPT_MEMBER_DECLARE_BEGIN(Entity)
SCRIPT_MEMBER_DECLARE_END()

ENTITY_GETSET_DECLARE_BEGIN(Entity)
SCRIPT_GET_DECLARE("cell",								pyGetCellEntityCall,			0,								0)	
SCRIPT_GET_DECLARE("client",							pyGetClientEntityCall,			0,								0)	
SCRIPT_GET_DECLARE("databaseID",						pyGetDBID,						0,								0)	
SCRIPT_GET_DECLARE("databaseInterfaceName",				pyGetDBInterfaceName,			0,								0)
SCRIPT_GETSET_DECLARE("shouldAutoBackup",				pyGetShouldAutoBackup,			pySetShouldAutoBackup,			0,		0)
SCRIPT_GETSET_DECLARE("shouldAutoArchive",				pyGetShouldAutoArchive,			pySetShouldAutoArchive,			0,		0)
ENTITY_GETSET_DECLARE_END()
BASE_SCRIPT_INIT(Entity, 0, 0, 0, 0, 0)

//-------------------------------------------------------------------------------------
Entity::Entity(ENTITY_ID id, const ScriptDefModule* pScriptModule,
		   PyTypeObject* pyType, bool isInitialised):
ScriptObject(pyType, isInitialised),
ENTITY_CONSTRUCTION(Entity),
clientEntityCall_(NULL),
cellEntityCall_(NULL),
cellDataDict_(NULL),
hasDB_(false),
DBID_(0),
isGetingCellData_(false),
isArchiveing_(false),
shouldAutoArchive_(1),
shouldAutoBackup_(1),
creatingCell_(false),
createdSpace_(false),
	inRestore_(false),
	pBufferedSendToClientMessages_(NULL),
	deferredMigrationSourceCellAppID_(0),
	deferredMigrationTargetCellAppID_(0),
	migrationCommitPending_(false),
	migrationCommitSourceCellAppID_(0),
	migrationCommitTargetCellAppID_(0),
	dbInterfaceIndex_(0)
{
	setDirty();

	script::PyGC::incTracing("Entity");
	ENTITY_INIT_PROPERTYS(Entity);

	// 创建并初始化cellData
	createCellData();
}

//-------------------------------------------------------------------------------------
Entity::~Entity()
{
	ENTITY_DECONSTRUCTION(Entity);
	S_RELEASE(clientEntityCall_);
	S_RELEASE(cellEntityCall_);
	S_RELEASE(cellDataDict_);
	SAFE_RELEASE(pBufferedSendToClientMessages_);

	if(Baseapp::getSingleton().pEntities())
		Baseapp::getSingleton().pEntities()->pGetbages()->erase(id());

	script::PyGC::decTracing("Entity");
}

//-------------------------------------------------------------------------------------
PyObject* Entity::__py_pyRegisterEvent(PyObject* self, PyObject* args)
{
	return EntityEvents::pyRegister(self, args);
}

//-------------------------------------------------------------------------------------
PyObject* Entity::__py_pyDeregisterEvent(PyObject* self, PyObject* args)
{
	return EntityEvents::pyDeregister(self, args);
}

//-------------------------------------------------------------------------------------
PyObject* Entity::__py_pyFireEvent(PyObject* self, PyObject* args)
{
	return EntityEvents::pyFire(self, args);
}

//-------------------------------------------------------------------------------------
void Entity::onInitializeScript()
{
	// Base 实体当前无需额外初始化；保留钩子以统一组件挂接后的生命周期扩展点。
	// Base entities currently need no extra work; keep the hook as the lifecycle extension point after component attachment.
}

//-------------------------------------------------------------------------------------
void Entity::onDefDataChanged(EntityComponent* pEntityComponent,
		const PropertyDescription* propertyDescription, PyObject* pyData)
{
	if(initing())
		return;

	if(propertyDescription->isPersistent())
		setDirty();
	
	uint32 flags = propertyDescription->getFlags();
	ENTITY_PROPERTY_UID componentPropertyUID = 0;
	int8 componentPropertyAliasID = 0;

	if(pEntityComponent)
	{
		PropertyDescription* pComponentPropertyDescription = pEntityComponent->pPropertyDescription();
		if(!pComponentPropertyDescription)
		{
			ERROR_MSG(fmt::format("{}::onDefDataChanged: EntityComponent({}) has no parent property description!\n",
				pScriptModule_->getName(), pEntityComponent->pComponentScriptDefModuleDescrs()->getName()));
			return;
		}

		// 2.8 属性更新协议用父属性标识组件，普通实体属性的父标识保持为零。
		// The 2.8 property-update protocol identifies components by their parent property; direct entity properties keep parent zero.
		componentPropertyUID = pComponentPropertyDescription->getUType();
		componentPropertyAliasID = pComponentPropertyDescription->aliasIDAsUint8();
	}

	if((flags & ED_FLAG_BASE_AND_CLIENT) <= 0 || clientEntityCall_ == NULL)
		return;

	// 创建一个需要广播的模板流
	MemoryStream* mstream = MemoryStream::createPoolObject(OBJECTPOOL_POINT);

	// 组件整体更新写给客户端时只序列化 Client 域，并在写入后恢复调用方上下文。
	// Whole-component updates serialize only the Client domain and restore the caller's context immediately afterward.
	COMPONENT_TYPE previousComponentType = EntityDef::context().currComponentType;
	EntityDef::context().currComponentType = CLIENT_TYPE;
	propertyDescription->getDataType()->addToStream(mstream, pyData);
	EntityDef::context().currComponentType = previousComponentType;

	Network::Bundle* pBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
	(*pBundle).newMessage(ClientInterface::onUpdatePropertys);
	(*pBundle) << id();

	if(pScriptModule_->usePropertyDescrAlias())
	{
		(*pBundle) << componentPropertyAliasID;
		(*pBundle) << propertyDescription->aliasIDAsUint8();
	}
	else
	{
		(*pBundle) << componentPropertyUID;
		(*pBundle) << propertyDescription->getUType();
	}

	pBundle->append(*mstream);
	
	g_privateClientEventHistoryStats.trackEvent(scriptName(), 
		propertyDescription->getName(), 
		pBundle->currMsgLength());

	// 按照当前的设计来说，有clientEntityCall_必定是proxy
	// 至于为何跑到baseEntity里来和python本身是C语言实现有关
	static_cast<Proxy*>(this)->sendToClient(ClientInterface::onUpdatePropertys, pBundle);
	MemoryStream::reclaimPoolObject(mstream);
}

//-------------------------------------------------------------------------------------
void Entity::onDestroy(bool callScript)
{
	if(callScript)
	{
		SCOPED_PROFILE(SCRIPTCALL_PROFILE);
		SCRIPT_OBJECT_CALL_ARGS0(this, const_cast<char*>("onDestroy"), false);
	}

	// 绑定方法会反向持有 Entity，必须在最终 Py_DECREF 前断开事件引用环。
	// Bound methods retain the Entity, so break the event reference cycle before the final Py_DECREF.
	EntityEvents::clear(this);

	if(this->hasDB())
	{
		onCellWriteToDBCompleted(0, -1, -1);
	}
	
	eraseEntityLog();

	// 按照当前的设计来说，有clientEntityCall_必定是proxy
	// 至于为何跑到baseEntity里来和python本身是C语言实现有关
	if(clientEntityCall_)
		static_cast<Proxy*>(this)->kick();
}

//-------------------------------------------------------------------------------------
void Entity::eraseEntityLog()
{
	// 这里没有使用hasDB()来进行判断
	// 用户可能destroy( writeToDB = False ), 这个操作会导致hasDB为false， 因此这里
	// 需要判断dbid是否大于0， 如果大于0则应该要去擦除在线等记录情况.
	if(this->dbid() > 0)
	{
		Network::Bundle* pBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
		(*pBundle).newMessage(DbmgrInterface::onEntityOffline);
		(*pBundle) << this->dbid();
		(*pBundle) << this->pScriptModule()->getUType();
		(*pBundle) << g_componentID;
		(*pBundle) << dbInterfaceIndex();

		Components::COMPONENTS& cts = Components::getSingleton().getComponents(DBMGR_TYPE);
		Components::ComponentInfos* dbmgrinfos = NULL;

		if(cts.size() > 0)
			dbmgrinfos = &(*cts.begin());

		if(dbmgrinfos == NULL || dbmgrinfos->pChannel == NULL || dbmgrinfos->cid == 0)
		{
			ERROR_MSG("Entity::onDestroy: not found dbmgr!\n");
			Network::Bundle::reclaimPoolObject(pBundle);
			return;
		}

		dbmgrinfos->pChannel->send(pBundle);
	}
}

//-------------------------------------------------------------------------------------
bool Entity::installCellDataAttr(PyObject* dictData, bool installpy)
{
	if(dictData != NULL)
	{
		if(cellDataDict_ != dictData)
		{
			if(cellDataDict_ != NULL)
				Py_DECREF(cellDataDict_);

			cellDataDict_ = dictData;
			Py_INCREF(cellDataDict_);
		}
	}

	if(installpy)
	{
		if(cellDataDict_ == NULL)
		{
			cellDataDict_ = PyDict_New();
		}

		if(PyObject_SetAttrString(this, "cellData", cellDataDict_) == -1)
		{
			ERROR_MSG("Entity::installCellDataAttr: set property cellData error!\n");
			SCRIPT_ERROR_CHECK();
			return false;
		}
	}

	return true;
}

//-------------------------------------------------------------------------------------
void Entity::createCellData(void)
{
	if(!pScriptModule_->hasCell() || !installCellDataAttr())
	{
		if(pScriptModule_->getCellPropertyDescriptions().size() > 0)
		{
			if(!pScriptModule_->hasCell())
			{
				WARNING_MSG(fmt::format("{}::createCellData: do not create cellData, cannot find the cellapp script({})!\n", 
					pScriptModule_->getName(), pScriptModule_->getName()));
			}
		}

		return;
	}

	// BaseApp 构造的是发往 Cell 的组件默认值，设置域后组件实例才能选择正确属性集合。
	// BaseApp constructs component defaults for Cell, so set the domain before selecting component properties.
	EntityDef::context().currComponentType = CELLAPP_TYPE;

	ScriptDefModule::PROPERTYDESCRIPTION_MAP& propertyDescrs = pScriptModule_->getCellPropertyDescriptions();
	ScriptDefModule::PROPERTYDESCRIPTION_MAP::const_iterator iter = propertyDescrs.begin();
	for(; iter != propertyDescrs.end(); ++iter)
	{
		PropertyDescription* propertyDescription = iter->second;
		DataType* dataType = propertyDescription->getDataType();
		
		if(dataType)
		{
			// 普通属性使用描述默认值；组件必须保留为 Cell 数据字典，BaseApp 不应实例化未加载的 Cell 脚本类。
			// Regular properties use descriptor defaults; components must remain Cell-data dictionaries because BaseApp must not instantiate unloaded Cell script classes.
			PyObject* pyObj = dataType->type() == DATA_TYPE_ENTITY_COMPONENT
				? ((EntityComponentType*)dataType)->createCellData()
				: propertyDescription->newDefaultVal();

			if(pyObj == NULL)
			{
				ERROR_MSG(fmt::format("{}::createCellData: failed to create default value for {}.\n",
					this->scriptName(), propertyDescription->getName()));
				SCRIPT_ERROR_CHECK();
				continue;
			}

			PyDict_SetItemString(cellDataDict_, propertyDescription->getName(), pyObj);
			Py_DECREF(pyObj);
		}
		else
		{
			ERROR_MSG(fmt::format("{}::createCellData: {} PropertyDescription the dataType is NULL.\n",
				this->scriptName(), propertyDescription->getName()));	
		}
		
		SCRIPT_ERROR_CHECK();
	}
	
	// 初始化cellEntity的位置和方向变量
	PyObject* position = PyTuple_New(3);
	PyTuple_SET_ITEM(position, 0, PyFloat_FromDouble(0.0));
	PyTuple_SET_ITEM(position, 1, PyFloat_FromDouble(0.0));
	PyTuple_SET_ITEM(position, 2, PyFloat_FromDouble(0.0));
	
	PyObject* direction = PyTuple_New(3);
	PyTuple_SET_ITEM(direction, 0, PyFloat_FromDouble(0.0));
	PyTuple_SET_ITEM(direction, 1, PyFloat_FromDouble(0.0));
	PyTuple_SET_ITEM(direction, 2, PyFloat_FromDouble(0.0));
	
	PyDict_SetItemString(cellDataDict_, "position", position);
	PyDict_SetItemString(cellDataDict_, "direction", direction);

	Py_DECREF(position);
	Py_DECREF(direction);

	SCRIPT_ERROR_CHECK();
}

//-------------------------------------------------------------------------------------
void Entity::addCellDataToStream(COMPONENT_TYPE sendTo, uint32 flags, MemoryStream* s, bool useAliasID)
{
	addPositionAndDirectionToStream(*s, useAliasID);

	if (!cellDataDict_)
		return;

	// 同一份 cellData 会发往 CellApp 或客户端，组件子属性过滤和别名编码取决于目标域。
	// The same cellData targets CellApp or clients; component filtering and alias encoding depend on that destination.
	EntityDef::context().currComponentType = sendTo == CLIENT_TYPE ? CLIENT_TYPE : CELLAPP_TYPE;

	ScriptDefModule::PROPERTYDESCRIPTION_MAP& propertyDescrs = pScriptModule_->getCellPropertyDescriptions();
	ScriptDefModule::PROPERTYDESCRIPTION_MAP::const_iterator iter = propertyDescrs.begin();

	for(; iter != propertyDescrs.end(); ++iter)
	{
		PropertyDescription* propertyDescription = iter->second;
		if(flags == 0 || (flags & propertyDescription->getFlags()) > 0)
		{
			PyObject* pyVal = PyDict_GetItemString(cellDataDict_, propertyDescription->getName());

			if(propertyDescription->getDataType()->type() == DATA_TYPE_ENTITY_COMPONENT)
			{
				EntityComponentType* pComponentType = static_cast<EntityComponentType*>(propertyDescription->getDataType());
				if(pComponentType->pScriptDefModule()->getCellPropertyDescriptions().empty())
					continue;

				if(useAliasID && pScriptModule_->usePropertyDescrAlias())
				{
					(*s) << static_cast<uint8>(0);
					(*s) << propertyDescription->aliasIDAsUint8();
				}
				else
				{
					(*s) << static_cast<ENTITY_PROPERTY_UID>(0);
					(*s) << propertyDescription->getUType();
				}

				pComponentType->addCellDataToStream(s, flags, pyVal, id(), propertyDescription, sendTo, true);
			}
			else
			{
				if(useAliasID && pScriptModule_->usePropertyDescrAlias())
				{
					(*s) << static_cast<uint8>(0);
					(*s) << propertyDescription->aliasIDAsUint8();
				}
				else
				{
					(*s) << static_cast<ENTITY_PROPERTY_UID>(0);
					(*s) << propertyDescription->getUType();
				}

				if(!propertyDescription->getDataType()->isSameType(pyVal))
				{
					ERROR_MSG(fmt::format("{}::addCellDataToStream: {}({}) not is ({})!\n", this->scriptName(),
						propertyDescription->getName(), (pyVal ? pyVal->ob_type->tp_name : "unknown"), propertyDescription->getDataType()->getName()));

					PyObject* pydefval = propertyDescription->parseDefaultStr("");
					propertyDescription->addToStream(s, pydefval);
					Py_DECREF(pydefval);
				}
				else
				{
					propertyDescription->addToStream(s, pyVal);
				}
			}

			if (PyErr_Occurred())
 			{	
				PyErr_PrintEx(0);
				DEBUG_MSG(fmt::format("{}::addCellDataToStream: {} error!\n", this->scriptName(),
					propertyDescription->getName()));
			}
		}
	}
}

//-------------------------------------------------------------------------------------
void Entity::addPersistentsDataToStream(uint32 flags, MemoryStream* s)
{
	std::vector<ENTITY_PROPERTY_UID> log;

	// 再将base中存储属性取出
	PyObject* pydict = PyObject_GetAttrString(this, "__dict__");

	// 先将celldata中的存储属性取出
	ScriptDefModule::PROPERTYDESCRIPTION_MAP& propertyDescrs = pScriptModule_->getPersistentPropertyDescriptions();
	ScriptDefModule::PROPERTYDESCRIPTION_MAP::const_iterator iter = propertyDescrs.begin();

	if(pScriptModule_->hasCell())
	{
		// 数据库顶层记录使用父 UID 与属性 UID 的双 UID 帧，坐标字段也必须遵循该格式。
		// Database top-level records use a parent/property UID pair, and position fields must follow the same frame.
		addPositionAndDirectionToStream(*s, false, true);
	}

	for(; iter != propertyDescrs.end(); ++iter)
	{
		PropertyDescription* propertyDescription = iter->second;
		std::vector<ENTITY_PROPERTY_UID>::const_iterator finditer = 
			std::find(log.begin(), log.end(), propertyDescription->getUType());

		if(finditer != log.end())
			continue;

		const char* attrname = propertyDescription->getName();
		if(propertyDescription->isPersistent() && (flags & propertyDescription->getFlags()) > 0)
		{
			bool isComponent = propertyDescription->getDataType()->type() == DATA_TYPE_ENTITY_COMPONENT;
			PyObject *key = PyUnicode_FromString(attrname);

			// 组件的 Base 部分是完整持久化对象的首选入口，不能被同名 Cell 字典片段提前截获。
			// The component's Base part is the preferred entry to the complete persistent object and must not be shadowed by its Cell dictionary fragment.
			if(!isComponent && cellDataDict_ != NULL && PyDict_Contains(cellDataDict_, key) > 0)
			{
				PyObject* pyVal = PyDict_GetItemString(cellDataDict_, attrname);
				if(!propertyDescription->getDataType()->isSameType(pyVal))
				{
					CRITICAL_MSG(fmt::format("{}::addPersistentsDataToStream: {} persistent({}) type(curr_py: {} != {}) error.\n",
						this->scriptName(), this->id(), attrname, (pyVal ? pyVal->ob_type->tp_name : "unknown"), propertyDescription->getDataType()->getName()));
				}
				else
				{
					// 零父 UID 保持普通属性与组件属性使用同一顶层帧格式。
					// A zero parent UID keeps normal and component properties on one top-level frame format.
					(*s) << (ENTITY_PROPERTY_UID)0 << propertyDescription->getUType();
					log.push_back(propertyDescription->getUType());
					propertyDescription->addPersistentToStream(s, pyVal);
					DEBUG_PERSISTENT_PROPERTY("addCellPersistentsDataToStream", attrname);
				}
			}
			else if(PyDict_Contains(pydict, key) > 0)
			{
				PyObject* pyVal = PyDict_GetItem(pydict, key);
				bool isSamePersistentType = isComponent ?
					static_cast<EntityComponentType*>(propertyDescription->getDataType())->isSamePersistentType(pyVal) :
					propertyDescription->getDataType()->isSameType(pyVal);

				if(!isSamePersistentType)
				{
					CRITICAL_MSG(fmt::format("{}::addPersistentsDataToStream: {} persistent({}) type(curr_py: {} != {}) error.\n",
						this->scriptName(), this->id(), attrname, (pyVal ? pyVal->ob_type->tp_name : "unknown"), propertyDescription->getDataType()->getName()));
				}
				else
				{
					(*s) << (ENTITY_PROPERTY_UID)0 << propertyDescription->getUType();
					log.push_back(propertyDescription->getUType());
					// 组件持久化需要合并 Base 对象和 Cell 字典，不能退化为面向在线组件协议的普通流编码。
					// Component persistence must merge the Base object and Cell dictionary instead of falling back to normal live-component stream encoding.
					if(isComponent)
						static_cast<EntityComponentType*>(propertyDescription->getDataType())->addPersistentToStream(s, pyVal);
					else
						propertyDescription->addPersistentToStream(s, pyVal);
					DEBUG_PERSISTENT_PROPERTY("addBasePersistentsDataToStream", attrname);
				}
			}
			else
			{
				if (!isComponent)
				{
					WARNING_MSG(fmt::format("{}::addPersistentsDataToStream: {} not found Persistent({}), use default values!\n",
						this->scriptName(), this->id(), attrname));

					(*s) << (ENTITY_PROPERTY_UID)0 << propertyDescription->getUType();
					log.push_back(propertyDescription->getUType());
					propertyDescription->addPersistentToStream(s, NULL);
				}
				else if (cellDataDict_)
				{
					// 无 Base 实例时仅持久化实际存在的 Cell 组件；没有 Cell 字段的组件无需生成空子表记录。
					// Without a Base instance, persist only an existing Cell component; a component without Cell fields needs no empty child row.
					EntityComponentType* pEntityComponentType = static_cast<EntityComponentType*>(propertyDescription->getDataType());
					if (pEntityComponentType->pScriptDefModule()->getCellPropertyDescriptions().size() == 0)
					{
						Py_DECREF(key);
						continue;
					}

					PyObject* pyVal = PyDict_GetItemString(cellDataDict_, attrname);
					if (!static_cast<EntityComponentType*>(propertyDescription->getDataType())->isSamePersistentType(pyVal))
					{
						CRITICAL_MSG(fmt::format("{}::addPersistentsDataToStream: {} persistent({}) type(curr_py: {} != {}) error.\n",
							this->scriptName(), this->id(), attrname, (pyVal ? pyVal->ob_type->tp_name : "unknown"), propertyDescription->getDataType()->getName()));
					}
					else
					{
						(*s) << (ENTITY_PROPERTY_UID)0 << propertyDescription->getUType();
						log.push_back(propertyDescription->getUType());
						pEntityComponentType->addPersistentToStream(s, pyVal);
						DEBUG_PERSISTENT_PROPERTY("addCellPersistentsDataToStream", attrname);
					}
				}
			}

			Py_DECREF(key);
		}

		SCRIPT_ERROR_CHECK();
	}

	Py_XDECREF(pydict);
	SCRIPT_ERROR_CHECK();
}

//-------------------------------------------------------------------------------------
PyObject* Entity::createCellDataDict(uint32 flags)
{
	PyObject* cellData = PyDict_New();

	ScriptDefModule::PROPERTYDESCRIPTION_MAP& propertyDescrs = pScriptModule_->getCellPropertyDescriptions();
	ScriptDefModule::PROPERTYDESCRIPTION_MAP::const_iterator iter = propertyDescrs.begin();
	for(; iter != propertyDescrs.end(); ++iter)
	{
		PropertyDescription* propertyDescription = iter->second;
		if((flags & propertyDescription->getFlags()) > 0)
		{
			PyObject* pyVal = PyDict_GetItemString(cellDataDict_, propertyDescription->getName());
			PyDict_SetItemString(cellData, propertyDescription->getName(), pyVal);
			Py_DECREF(pyVal);
			SCRIPT_ERROR_CHECK();
		}
	}

	return cellData;
}

//-------------------------------------------------------------------------------------
void Entity::sendToCellapp(Network::Bundle* pBundle)
{
	if (!cellEntityCall_)
	{
		ERROR_MSG(fmt::format("{}::sendToCellapp: no cell! entityID={}\n", this->scriptName(), id()));
		return;
	}

	sendToCellapp(cellEntityCall_->getChannel(), pBundle);
}

//-------------------------------------------------------------------------------------
void Entity::sendToCellapp(Network::Channel* pChannel, Network::Bundle* pBundle)
{
	if (!pChannel)
	{
		ERROR_MSG(fmt::format("{}::sendToCellapp: pChannel == NULL! entityID={}\n", this->scriptName(), id()));
		return;
	}

	KBE_ASSERT(pBundle != NULL);

	pChannel->send(pBundle);
}

//-------------------------------------------------------------------------------------
void Entity::destroyCellData(void)
{
	// cellDataDict_ 继续保留， 以供备份时使用， 这里仅仅让脚步层无法访问到即可
	// S_RELEASE(cellDataDict_);
	if(PyObject_DelAttrString(this, "cellData") == -1)
	{
		ERROR_MSG(fmt::format("{}::destroyCellData: delete cellData error!\n", this->scriptName()));
		SCRIPT_ERROR_CHECK();
	}
}

//-------------------------------------------------------------------------------------
bool Entity::destroyCellEntity(void)
{
	if(isDestroyed())	
	{
		return false;																					
	}

	if (cellHandoffAction_ != CellHandoff::NONE || migrationEpoch_ || partitionRestorePending_)
	{
		// 迁移结束后再销毁当前 Real，避免销毁请求发到旧路由。
		destroyCellAfterHandoff_ = true;
		return true;
	}

	if(cellEntityCall_  == NULL || cellEntityCall_->getChannel() == NULL)
	{
		isArchiveing_ = false;
		return false;
	}

	Network::Bundle* pBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
	(*pBundle).newMessage(CellappInterface::onDestroyCellEntityFromBaseapp);
	(*pBundle) << id_;
	sendToCellapp(pBundle);
	return true;
}

//-------------------------------------------------------------------------------------
PyObject* Entity::pyDestroyCellEntity()
{
	if(cellEntityCall_ == NULL) 
	{
		PyErr_Format(PyExc_AssertionError, "%s::destroyCellEntity: id:%i no cell! creatingCell=%s\n", 
			this->scriptName(), this->id(),
			creatingCell_ ? "true" : "false");
		PyErr_PrintEx(0);
		return 0;
	}
	else
		destroyCellEntity();

	S_Return;
}

//-------------------------------------------------------------------------------------
PyObject* Entity::__py_pyDestroyEntity(PyObject* self, PyObject* args, PyObject * kwargs)
{
	Entity* pobj = static_cast<Entity*>(self);

	if(pobj->initing())
	{
		PyErr_Format(PyExc_AssertionError,
			"%s::destroy(): %d initing, reject the request!\n",	
			pobj->scriptName(), pobj->id());
		PyErr_PrintEx(0);
		return NULL;
	}

	static char * keywords[] =
	{
		const_cast<char *> ("deleteFromDB"),
		const_cast<char *> ("writeToDB"),
		NULL
	};

	if(pobj->isDestroyed())	
	{
		PyErr_Format(PyExc_AssertionError, "%s::destroy: %d is destroyed!\n",
			pobj->scriptName(), pobj->id());

		PyErr_PrintEx(0);
		return NULL;
	}

	if (pobj->creatingCell())
	{
		WARNING_MSG(fmt::format("{}::destroy(): id={} creating cell! automatic 'destroy' process will begin after 'onGetCell'.\n",
			pobj->scriptName(), pobj->id()));

		pobj->addFlags(ENTITY_FLAGS_DESTROY_AFTER_GETCELL);
		S_Return;
	}

	if (pobj->cellEntityCall() != NULL)
	{
		PyErr_Format(PyExc_Exception, "%s::destroy: id:%i has cell, please destroyCellEntity() first!\n",
			pobj->scriptName(), pobj->id());

		PyErr_PrintEx(0);
		return NULL;
	}

	PyObject* pyDeleteFromDB = NULL;
	PyObject* pyWriteToDB = NULL;

	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|OO", 
		keywords, &pyDeleteFromDB, &pyWriteToDB))
	{
		PyErr_Format(PyExc_AssertionError, "%s::destroy: %d ParseTupleAndKeywords(deleteFromDB, &writeToDB) error!\n",
			pobj->scriptName(), pobj->id());

		PyErr_PrintEx(0);
		return NULL;
	}

	bool deleteFromDB = (pyDeleteFromDB != NULL) ? 
		(PyObject_IsTrue(pyDeleteFromDB) ? true : false) : false; 

	bool writeToDB = (pyWriteToDB != NULL) ? 
		(PyObject_IsTrue(pyWriteToDB) ? true : false) : pobj->hasDB();

	if(deleteFromDB || writeToDB)
	{
		// 有可能已经请求了writeToDB但还未返回写入的dbid
		// 这种情况需要返回给用户一个错误， 用户可以继续尝试这个操作
		if(pobj->hasDB() && pobj->dbid() == 0)
		{
			PyErr_Format(PyExc_AssertionError, "%s::destroy: id:%i has db, current dbid is 0. "
				"please wait for dbmgr to processing!\n", 
				pobj->scriptName(), pobj->id());

			PyErr_PrintEx(0);
			return NULL;
		}
	}

	pobj->onDestroyEntity(deleteFromDB, writeToDB);
	pobj->destroyEntity();

	S_Return;
}

//-------------------------------------------------------------------------------------
void Entity::onDestroyEntity(bool deleteFromDB, bool writeToDB)
{
	if(deleteFromDB && hasDB())
	{
		Components::COMPONENTS& cts = Components::getSingleton().getComponents(DBMGR_TYPE);
		Components::ComponentInfos* dbmgrinfos = NULL;

		if(cts.size() > 0)
			dbmgrinfos = &(*cts.begin());

		if(dbmgrinfos == NULL || dbmgrinfos->pChannel == NULL || dbmgrinfos->cid == 0)
		{
			ERROR_MSG(fmt::format("{}::onDestroyEntity({}): writeToDB not found dbmgr!\n", this->scriptName(), this->id()));
			return;
		}

		Network::Bundle* pBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
		(*pBundle).newMessage(DbmgrInterface::removeEntity);
		
		(*pBundle) << this->dbInterfaceIndex();
		(*pBundle) << g_componentID;
		(*pBundle) << this->id();
		(*pBundle) << this->dbid();
		(*pBundle) << this->pScriptModule()->getUType();
		dbmgrinfos->pChannel->send(pBundle);

		this->hasDB(false);
		return;
	}

	if(writeToDB)
	{
		// 这个行为默认会处理
		// this->writeToDB(NULL);
	}
	else
	{
		this->hasDB(false);
	}

	shouldAutoArchive_ = 0;
	shouldAutoBackup_ = 0;
}

//-------------------------------------------------------------------------------------
PyObject* Entity::onScriptGetAttribute(PyObject* attr)
{
	DEBUG_OP_ATTRIBUTE("get", attr)
	return ScriptObject::onScriptGetAttribute(attr);
}	

//-------------------------------------------------------------------------------------
PyObject* Entity::pyGetCellEntityCall()
{ 
	if (!hasFlags(ENTITY_FLAGS_DESTROYING) && isDestroyed())
	{
		PyErr_Format(PyExc_AssertionError, "%s: %d is destroyed!\n",		
			scriptName(), id());		

		return 0;																					
	}

	EntityCall* entitycall = cellEntityCall();
	if(entitycall == NULL)
		S_Return;

	Py_INCREF(entitycall);
	return entitycall; 
}

//-------------------------------------------------------------------------------------
PyObject* Entity::pyGetDBID()
{
	if (!hasFlags(ENTITY_FLAGS_DESTROYING) && isDestroyed())
	{
		PyErr_Format(PyExc_AssertionError, "%s: %d is destroyed!\n",		
			scriptName(), id());		

		return 0;																					
	}

	return PyLong_FromUnsignedLongLong(this->dbid()); 
}

//-------------------------------------------------------------------------------------
PyObject* Entity::pyGetDBInterfaceName()
{
	if (!hasFlags(ENTITY_FLAGS_DESTROYING) && isDestroyed())
	{
		PyErr_Format(PyExc_AssertionError, "%s: %d is destroyed!\n",
			scriptName(), id());

		return 0;
	}

	if (dbid() == 0)
		return PyUnicode_FromString("");

	return PyUnicode_FromString(g_kbeSrvConfig.dbInterfaceIndex2dbInterfaceName(dbInterfaceIndex()));
}

//-------------------------------------------------------------------------------------
PyObject* Entity::pyGetClientEntityCall()
{
	if (!hasFlags(ENTITY_FLAGS_DESTROYING) && isDestroyed())
	{
		PyErr_Format(PyExc_AssertionError, "%s: %d is destroyed!\n",		
			scriptName(), id());		

		return 0;																				
	}

	EntityCall* entitycall = clientEntityCall();
	if(entitycall == NULL)
		S_Return;

	Py_INCREF(entitycall);
	return entitycall; 
}

//-------------------------------------------------------------------------------------
int Entity::pySetShouldAutoArchive(PyObject *value)
{
	if(isDestroyed())	
	{
		PyErr_Format(PyExc_AssertionError, "%s: %d is destroyed!\n",		
			scriptName(), id());		
		PyErr_PrintEx(0);
		return 0;																				
	}

	if(!PyLong_Check(value))
	{
		PyErr_Format(PyExc_AssertionError, "%s: %d set shouldAutoArchive value is not int!\n",		
			scriptName(), id());		
		PyErr_PrintEx(0);
		return 0;	
	}

	shouldAutoArchive_ = (int8)PyLong_AsLong(value);
	return 0;
}

//-------------------------------------------------------------------------------------
PyObject* Entity::pyGetShouldAutoArchive()
{
	return PyLong_FromLong(shouldAutoArchive_);
}

//-------------------------------------------------------------------------------------
int Entity::pySetShouldAutoBackup(PyObject *value)
{
	if(isDestroyed())	
	{
		PyErr_Format(PyExc_AssertionError, "%s: %d is destroyed!\n",		
			scriptName(), id());		
		PyErr_PrintEx(0);
		return 0;																				
	}

	if(!PyLong_Check(value))
	{
		PyErr_Format(PyExc_AssertionError, "%s: %d set shouldAutoBackup value is not int!\n",		
			scriptName(), id());		
		PyErr_PrintEx(0);
		return 0;	
	}

	shouldAutoBackup_ = (int8)PyLong_AsLong(value);
	return 0;
}

//-------------------------------------------------------------------------------------
PyObject* Entity::pyGetShouldAutoBackup()
{
	return PyLong_FromLong(shouldAutoBackup_);
}

//-------------------------------------------------------------------------------------
void Entity::onCreateCellFailure(void)
{
	SCOPED_PROFILE(SCRIPTCALL_PROFILE);

	creatingCell_ = false;
	isGetingCellData_ = false;

	SCRIPT_OBJECT_CALL_ARGS0(this, const_cast<char*>("onCreateCellFailure"), false);
}

//-------------------------------------------------------------------------------------
void Entity::onRemoteMethodCall(Network::Channel* pChannel, MemoryStream& s)
{
	SCOPED_PROFILE(SCRIPTCALL_PROFILE);
	ScriptStageMetrics& stageMetrics = scriptStageMetrics();
	const bool sampleStages = stageMetrics.beginRpcCall();
	const uint64 lookupStart = sampleStages ? timestamp() : 0;

	if(isDestroyed())																				
	{																										
		ERROR_MSG(fmt::format("{}::onRemoteMethodCall: {} is destroyed!\n",											
			scriptName(), id()));

		s.done();
		return;																							
	}

	// Nex 2.8 客户端 RPC 帧先携带父组件属性 UID；普通实体方法固定为 0。
	// Nex 2.8 client RPC frames lead with the parent component-property UID; regular entity methods use zero.
	ENTITY_PROPERTY_UID componentPropertyUID = 0;
	s >> componentPropertyUID;

	ENTITY_METHOD_UID utype = 0;
	s >> utype;

	ScriptDefModule* pCallScriptModule = pScriptModule_;
	PyObject* pyCallObject = this;
	PropertyDescription* pComponentPropertyDescription = NULL;

	if (componentPropertyUID > 0)
	{
		pComponentPropertyDescription = pScriptModule_->findBasePropertyDescription(componentPropertyUID);
		if (pComponentPropertyDescription == NULL ||
			pComponentPropertyDescription->getDataType()->type() != DATA_TYPE_ENTITY_COMPONENT)
		{
			ERROR_MSG(fmt::format("{}::onRemoteMethodCall: can't find EntityComponent({}), methodUID={}, callerID={}.\n",
				this->scriptName(), componentPropertyUID, utype, id_));
			s.done();
			return;
		}

		pCallScriptModule = static_cast<EntityComponentType*>(
			pComponentPropertyDescription->getDataType())->pScriptDefModule();
		pyCallObject = PyObject_GetAttrString(this, pComponentPropertyDescription->getName());
		if (pyCallObject == NULL)
		{
			SCRIPT_ERROR_CHECK();
			s.done();
			return;
		}
	}

	MethodDescription* pMethodDescription = pCallScriptModule->findBaseMethodDescription(utype);
	if(pMethodDescription == NULL)
	{
		ERROR_MSG(fmt::format("{}::onRemoteMethodCall: can't find {}method, methodUID={}, callerID={}.\n",
			this->scriptName(), pComponentPropertyDescription ? "component " : "", utype, id_));

		if (pyCallObject != static_cast<PyObject*>(this))
			Py_DECREF(pyCallObject);

		s.done();
		return;
	}

	// 如果是外部通道调用则判断来源性
	if (pChannel->isExternal())
	{
		ENTITY_ID srcEntityID = pChannel->proxyID();
		if (srcEntityID <= 0 || srcEntityID != this->id())
		{
			WARNING_MSG(fmt::format("{}::onRemoteMethodCall({}): srcEntityID={} != thisEntityID={}.\n",
				this->scriptName(), pMethodDescription->getName(), srcEntityID, this->id()));

			if (pyCallObject != static_cast<PyObject*>(this))
				Py_DECREF(pyCallObject);

			s.done();
			return;
		}

		if(!pMethodDescription->isExposed())
		{
			ERROR_MSG(fmt::format("{}::onRemoteMethodCall: {} is not exposed, srcEntityID={}.\n",
				this->scriptName(), pMethodDescription->getName(), srcEntityID));

			if (pyCallObject != static_cast<PyObject*>(this))
				Py_DECREF(pyCallObject);

			s.done();
			return;
		}
	}

	if(g_debugEntity)
	{
		DEBUG_MSG(fmt::format("{}::onRemoteMethodCall: entityID={}, {}{}{}(utype={}).\n",
			this->scriptName(), id_, pComponentPropertyDescription ? pCallScriptModule->getName() : "",
			pComponentPropertyDescription ? "::" : "", pMethodDescription->getName(), utype));
	}

	const char* handlerName = pMethodDescription->getName();
	stageMetrics.record(SCRIPT_STAGE_RPC_LOOKUP,
		sampleStages ? scriptStageDurationNanos(lookupStart) : 0,
		sampleStages, handlerName);

	pMethodDescription->currCallerID(this->id());
	// RPC 参数可能包含组件值，反序列化前绑定当前实体以保证组件 owner 正确。
	// RPC arguments may contain component values; bind the current entity before deserialization to preserve ownership.
	EntityDef::context().currEntityID = this->id();
	const uint64 pythonLookupStart = sampleStages ? timestamp() : 0;
	PyObject* pyFunc = PyObject_GetAttrString(pyCallObject, const_cast<char*>
						(pMethodDescription->getName()));
	stageMetrics.record(SCRIPT_STAGE_PYTHON_LOOKUP,
		sampleStages ? scriptStageDurationNanos(pythonLookupStart) : 0,
		sampleStages, handlerName);
	uint64 cleanupStart = 0;

	if(pMethodDescription != NULL)
	{
		if(pMethodDescription->getArgSize() == 0)
		{
			stageMetrics.record(SCRIPT_STAGE_ARGUMENT_DECODE, 0, sampleStages, handlerName);
			const uint64 pythonCallStart = sampleStages ? timestamp() : 0;
			pMethodDescription->call(pyFunc, NULL);
			stageMetrics.record(SCRIPT_STAGE_PYTHON_CALL,
				sampleStages ? scriptStageDurationNanos(pythonCallStart) : 0,
				sampleStages, handlerName);
			cleanupStart = sampleStages ? timestamp() : 0;
		}
		else
		{
			const uint64 decodeStart = sampleStages ? timestamp() : 0;
			PyObject* pyargs = pMethodDescription->createFromStream(&s);
			stageMetrics.record(SCRIPT_STAGE_ARGUMENT_DECODE,
				sampleStages ? scriptStageDurationNanos(decodeStart) : 0,
				sampleStages, handlerName);
			if(pyargs)
			{
				const uint64 pythonCallStart = sampleStages ? timestamp() : 0;
				pMethodDescription->call(pyFunc, pyargs);
				stageMetrics.record(SCRIPT_STAGE_PYTHON_CALL,
					sampleStages ? scriptStageDurationNanos(pythonCallStart) : 0,
					sampleStages, handlerName);
				cleanupStart = sampleStages ? timestamp() : 0;
				Py_XDECREF(pyargs);
			}
			else
			{
				SCRIPT_ERROR_CHECK();
			}
		}
	}
	
	if (sampleStages && cleanupStart == 0)
		cleanupStart = timestamp();
	Py_XDECREF(pyFunc);

	if (pyCallObject != static_cast<PyObject*>(this))
		Py_DECREF(pyCallObject);
	stageMetrics.record(SCRIPT_STAGE_CLEANUP,
		sampleStages ? scriptStageDurationNanos(cleanupStart) : 0,
		sampleStages, handlerName);
}

//-------------------------------------------------------------------------------------
void Entity::onGetCell(Network::Channel* pChannel, COMPONENT_ID componentID)
{
	if(pChannel->isExternal())
		return;
	
	SCOPED_PROFILE(SCRIPTCALL_PROFILE);

	creatingCell_ = false;
	lostPartitionReal_.reset();
	// 在线 Proxy 可能已在客户端进场回调里建立 Cell 引用。
	if (!cellEntityCall_ || !cellBackupGeneration_)
		resetRecoveryCellBackup();

	// 删除cellData属性
	destroyCellData();
	
	// 回调给脚本，获得了cell
	if(cellEntityCall_ == NULL)
		cellEntityCall_ = new EntityCall(pScriptModule_, NULL, componentID, id_, ENTITYCALL_TYPE_CELL);

	if(!inRestore_)
		SCRIPT_OBJECT_CALL_ARGS0(this, const_cast<char*>("onGetCell"), false);

	if (!isDestroyed() && hasFlags(ENTITY_FLAGS_DESTROY_AFTER_GETCELL))
	{
		WARNING_MSG(fmt::format("{}::onGetCell(): Automatically destroy cell! id={}.\n",
			this->scriptName(), this->id()));

		destroyCellEntity();
	}
}

//-------------------------------------------------------------------------------------
void Entity::onClientDeath()
{
	SCOPED_PROFILE(SCRIPTCALL_PROFILE);

	CALL_COMPONENTS_AND_ENTITY_METHOD(this,
		SCRIPT_OBJECT_CALL_ARGS0(pyTempObj, const_cast<char*>("onClientDeath"), GETERR));
}

//-------------------------------------------------------------------------------------
void Entity::onLoseCell(Network::Channel* pChannel, MemoryStream& s)
{
	if (pChannel->isExternal() ||
		((cellHandoffAction_ != CellHandoff::NONE || migrationEpoch_) && cellEntityCall() &&
			pChannel->componentID() != cellEntityCall()->componentID()))
		return;
	loseCell();
}

void Entity::loseCell()
{
	SCOPED_PROFILE(SCRIPTCALL_PROFILE);
	// onLoseCell 可以在脚本中销毁自身；外层引用覆盖回调后的标志检查和自动销毁分支。
	Py_INCREF(this);

	S_RELEASE(cellEntityCall_);
	cancelPartitionRestore();
	lostPartitionReal_.reset();
	resetRecoveryCellBackup();

	cellHandoffAction_ = CellHandoff::NONE;
	cellHandoffRoute_ = {};
	cellHandoffWarningAt_ = 0;
	cellHandoffSourceExited_ = false;
	cellHandoffTargetLoss_.reset();
	destroyCellAfterHandoff_ = false;
	resetTeleport();
	completedMigrationEpoch_ = 0;
	migrationWitnessPending_ = false;
	isArchiveing_ = false;
	isGetingCellData_ = false;
	createdSpace_ = false;
	
	CALL_COMPONENTS_AND_ENTITY_METHOD(this,
		SCRIPT_OBJECT_CALL_ARGS0(pyTempObj, const_cast<char*>("onLoseCell"), GETERR));

	if (!isDestroyed() && hasFlags(ENTITY_FLAGS_DESTROY_AFTER_GETCELL))
	{
		WARNING_MSG(fmt::format("{}::onLoseCell(): Automatically destroy! id={}.\n",
			this->scriptName(), this->id()));

		destroy();
	}

	Py_DECREF(this);
}

//-------------------------------------------------------------------------------------
void Entity::onRestore()
{
	if(!inRestore_)
		return;

	SCOPED_PROFILE(SCRIPTCALL_PROFILE);

	SCRIPT_OBJECT_CALL_ARGS0(this, const_cast<char*>("onRestore"), false);

	inRestore_ = false;
	isArchiveing_ = false;
	removeFlags(ENTITY_FLAGS_INITING);
}

//-------------------------------------------------------------------------------------
void Entity::reqBackupCellData()
{
	if (ServerConfig::getSingleton().getCellAppMgr().cellappmgr_space_partitioning_enabled)
	{
		requestRecoveryCellBackup();
		return;
	}

	if(isGetingCellData_)
		return;

	EntityCall* mb = this->cellEntityCall();
	if(mb == NULL)
		return;

	Network::Bundle* pBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
	(*pBundle).newMessage(CellappInterface::reqBackupEntityCellData);
	(*pBundle) << this->id();
	sendToCellapp(pBundle);

	isGetingCellData_ = true;
}

void Entity::resetRecoveryCellBackup()
{
	++cellBackupGeneration_;
	recoveryCellBackup_.reset();
}

void Entity::requestRecoveryCellBackup()
{
	if (!spaceID() || !cellBackupGeneration_ || !cellEntityCall() ||
		hasPendingCellTransfer() || lostPartitionReal_)
		return;

	auto* channel = Components::getSingleton().findComponentChannel(CELLAPP_TYPE, cellEntityCall()->componentID());
	if (!channel || channel->isDestroyed() || channel->condemn() != 0)
		return;
	const auto now = timestamp();
	const auto cell = cellEntityCall()->componentID();
	const auto session = channel->sessionEpoch();
	if (!recoveryCellBackup_ || !recoveryCellBackup_->owns(cell, session, spaceID(), cellBackupGeneration_))
	{
		recoveryCellBackup_ = std::make_unique<CellRecovery::CellBackup>();
		recoveryCellBackup_->cell = cell;
		recoveryCellBackup_->session = session;
		recoveryCellBackup_->space = spaceID();
		recoveryCellBackup_->generation = cellBackupGeneration_;
	}
	auto& backup = *recoveryCellBackup_;
	if (backup.request && now < backup.retryAt)
		return;

	// 请求编号不随 Entity 重建而重用；一次迟到的回包不能结束下一次等待。
	static uint64 nextRequest = 0;
	backup.request = ++nextRequest;
	backup.retryAt = now + 5 * stampsPerSecond();
	auto* bundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
	bundle->newMessage(CellappInterface::reqRecoveryCellBackup);
	(*bundle) << id();
	backup.stamp().write(*bundle);
	(*bundle) << static_cast<uint8>(!backup.data.empty());
	if (!backup.data.empty())
		CellRecovery::writeBackupDigest(*bundle, backup.digest);
	channel->send(bundle);
}

void Entity::onRecoveryCellBackup(Network::Channel& channel, MemoryStream& stream)
{
	BoundedStreamReader reader(stream);
	CellRecovery::BackupStamp stamp;
	uint8 full = 0;
	const auto matches = [&]()
	{
		const auto* cell = cellEntityCall();
		return !isDestroyed() && cell && !lostPartitionReal_ && !hasPendingCellTransfer() &&
			spaceID() == stamp.space && cellBackupGeneration_ == stamp.generation &&
			&channel == Components::getSingleton().findComponentChannel(CELLAPP_TYPE, cell->componentID()) &&
			recoveryCellBackup_ &&
			recoveryCellBackup_->matches(cell->componentID(), channel.sessionEpoch(), stamp);
	};
	if (!stamp.read(reader) || !reader.read(full) || full > 1 || !matches())
		return;
	if (!full)
	{
		if (reader.empty() && !recoveryCellBackup_->data.empty())
			recoveryCellBackup_->request = 0;
		return;
	}

	CellRecovery::BackupDigest digest;
	if (!CellRecovery::readBackupDigest(reader, digest) || reader.empty())
		return;
	stream.read_skip(stream.length() - reader.remaining());
	const auto* data = stream.data() + stream.rpos();
	const auto length = stream.length();
	std::unique_ptr<PyObject, decltype(&Py_DecRef)> decoded(nullptr, Py_DecRef);
	try
	{
		decoded.reset(createCellDataFromStream(&stream, true));
	}
	catch (const MemoryStreamException&)
	{
		// 截断的属性流不能覆盖上一次备份。
	}
	if (!decoded)
	{
		SCRIPT_ERROR_CHECK();
		WARNING_MSG(fmt::format("{}::onRecoveryCellBackup: incomplete Cell data, entity={}.\n", scriptName(), id()));
		return;
	}
	if (!matches())
		return;

	// 恢复副本不依赖脚本可修改的 cellData，也不随普通存档改写。
	auto& backup = *recoveryCellBackup_;
	backup.data.assign(data, data + length);
	backup.digest = digest;
	backup.request = 0;
	installCellDataAttr(decoded.get());
	setDirty();
}

//-------------------------------------------------------------------------------------
void Entity::onBackupCellData(Network::Channel* pChannel, MemoryStream& s)
{
	if(pChannel->isExternal())
		return;
	
	isGetingCellData_ = false;

	bool isDirty = false;
	s >> isDirty;
	
	if(isDirty)
	{		
		PyObject* cellData = createCellDataFromStream(&s);
		installCellDataAttr(cellData);
		Py_DECREF(cellData);
		setDirty();
	}
}

//-------------------------------------------------------------------------------------
void Entity::writeBackupData(MemoryStream* s)
{
	onBackup();
}

//-------------------------------------------------------------------------------------
void Entity::onBackup()
{
	reqBackupCellData();
}

//-------------------------------------------------------------------------------------
void Entity::writeToDB(void* data, void* extra1, void* extra2)
{
	PyObject* pyCallback = NULL;
	int8 shouldAutoLoad = dbid() <= 0 ? 0 : -1;

	// data 是有可能会NULL的， 比如定时存档是不需要回调函数的
	if(data != NULL)
		pyCallback = static_cast<PyObject*>(data);

	if(extra1 != NULL && (*static_cast<int*>(extra1)) != -1)
		shouldAutoLoad = (*static_cast<int*>(extra1)) > 0 ? 1 : 0;

	if (extra2)
	{
		if (strlen(static_cast<char*>(extra2)) > 0)
		{
			DBInterfaceInfo* pDBInterfaceInfo = g_kbeSrvConfig.dbInterface(static_cast<char*>(extra2));
			if (pDBInterfaceInfo->isPure)
			{
				ERROR_MSG(fmt::format("Entity::writeToDB: dbInterface({}) is a pure database does not support Entity! "
					"kbengine[_defs].xml->dbmgr->databaseInterfaces->*->pure\n",
					static_cast<char*>(extra2)));

				return;
			}

			int dbInterfaceIndex = pDBInterfaceInfo->index;
			if (dbInterfaceIndex >= 0)
			{
				dbInterfaceIndex_ = dbInterfaceIndex;
			}
			else
			{
				ERROR_MSG(fmt::format("Entity::writeToDB: not found dbInterface({})!\n",
					static_cast<char*>(extra2)));

				return;
			}
		}
	}

	if(isArchiveing_)
	{
		// __py_pyWriteToDB没有增加引用
		//if(pyCallback != NULL)
		//	Py_DECREF(pyCallback);

		WARNING_MSG(fmt::format("{}::writeToDB(): is archiveing! entityid={}, dbid={}.\n", 
			this->scriptName(), this->id(), this->dbid()));

		return;
	}

	isArchiveing_ = true;

	if(isDestroyed())
	{	
		// __py_pyWriteToDB没有增加引用
		//if(pyCallback != NULL)
		//	Py_DECREF(pyCallback);

		ERROR_MSG(fmt::format("{}::writeToDB(): is destroyed! entityid={}, dbid={}.\n", 
			this->scriptName(), this->id(), this->dbid()));

		return;
	}

	CALLBACK_ID callbackID = 0;
	if(pyCallback != NULL)
	{
		callbackID = callbackMgr().save(pyCallback);
	}

	// creatingCell_ 此时可能正在创建cell
	// 不过我们在此假设在cell未创建完成的时候base这个接口被调用
	// 写入数据库的是该entity的初始值， 并不影响
	if(this->cellEntityCall() == NULL) 
	{
		onCellWriteToDBCompleted(callbackID, shouldAutoLoad, -1);
	}
	else
	{
		Network::Bundle* pBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
		(*pBundle).newMessage(CellappInterface::reqWriteToDBFromBaseapp);
		(*pBundle) << this->id();
		(*pBundle) << callbackID;
		(*pBundle) << shouldAutoLoad;
		sendToCellapp(pBundle);
	}
}

//-------------------------------------------------------------------------------------
void Entity::onWriteToDBCallback(ENTITY_ID eid, 
								DBID entityDBID, 
								uint16 dbInterfaceIndex,
								CALLBACK_ID callbackID, 
								int8 shouldAutoLoad,
								bool success)
{
	isArchiveing_ = false;

	PyObjectPtr pyCallback;

	if(callbackID > 0)
		pyCallback = callbackMgr().take(callbackID);

	if(dbid() <= 0)
	{
		dbid(dbInterfaceIndex, entityDBID);
	}
	
	if (dbid() <= 0)
	{
		KBE_ASSERT(!success);
		hasDB(false);
	}

	if(callbackID > 0)
	{
		PyObject* pyargs = PyTuple_New(2);

		Py_INCREF(this);
		PyTuple_SET_ITEM(pyargs, 0, PyBool_FromLong((long)success));
		PyTuple_SET_ITEM(pyargs, 1, this);
		
		if(pyCallback != NULL)
		{
			PyObject* pyRet = PyObject_CallObject(pyCallback.get(), pyargs);
			if(pyRet == NULL)
			{
				SCRIPT_ERROR_CHECK();
			}
			else
			{
				Py_DECREF(pyRet);
			}
		}
		else
		{
			ERROR_MSG(fmt::format("{}::onWriteToDBCallback: not found callback:{}.\n",
				this->scriptName(), callbackID));
		}

		Py_DECREF(pyargs);
	}
}

//-------------------------------------------------------------------------------------
void Entity::onCellWriteToDBCompleted(CALLBACK_ID callbackID, int8 shouldAutoLoad, int dbInterfaceIndex)
{
	SCOPED_PROFILE(SCRIPTCALL_PROFILE);
	
	// onPreArchive 必须在归档继续前同步完成，因此不能经过支持 awaitable 的通用回调宏。
	// onPreArchive must finish before archiving continues, so it must bypass the generic callback macro that accepts awaitables.
	if (PyObject_HasAttrString(this, "onPreArchive"))
	{
		PyObject* pyResult = PyObject_CallMethod(this, const_cast<char*>("onPreArchive"), const_cast<char*>(""));
		if (pyResult != NULL)
			Py_DECREF(pyResult);
		else
			SCRIPT_ERROR_CHECK();
	}

	if (dbInterfaceIndex >= 0)
		dbInterfaceIndex_ = dbInterfaceIndex;

	hasDB(true);
	
	onWriteToDB();
	
	// 如果在数据库中已经存在该entity则允许应用层多次调用写库进行数据及时覆盖需求
	if(this->DBID_ > 0)
		isArchiveing_ = false;
	
	Components::COMPONENTS& cts = Components::getSingleton().getComponents(DBMGR_TYPE);
	Components::ComponentInfos* dbmgrinfos = NULL;

	if(cts.size() > 0)
		dbmgrinfos = &(*cts.begin());

	if(dbmgrinfos == NULL || dbmgrinfos->pChannel == NULL || dbmgrinfos->cid == 0)
	{
		ERROR_MSG(fmt::format("{}::onCellWriteToDBCompleted({}): not found dbmgr!\n", 
			this->scriptName(), this->id()));
		return;
	}
	
	MemoryStream* s = MemoryStream::createPoolObject(OBJECTPOOL_POINT);

	try
	{
		addPersistentsDataToStream(ED_FLAG_ALL, s);
	}
	catch (MemoryStreamWriteOverflow & err)
	{
		ERROR_MSG(fmt::format("{}::onCellWriteToDBCompleted({}): {}\n",
			this->scriptName(), this->id(), err.what()));

		MemoryStream::reclaimPoolObject(s);
		return;
	}

	if (s->length() == 0)
	{
		MemoryStream::reclaimPoolObject(s);
		return;
	}

	KBE_SHA1 sha;
	uint32 digest[5];

	sha.Input(s->data(), s->length());
	sha.Result(digest);

	// 检查数据是否有变化，有变化则将数据备份并且记录数据hash，没变化什么也不做
	if (memcmp((void*)&persistentDigest_[0], (void*)&digest[0], sizeof(persistentDigest_)) == 0)
	{
		MemoryStream::reclaimPoolObject(s);
		return;
	}
	else
	{
		setDirty((uint32*)&digest[0]);
	}

	Network::Bundle* pBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
	(*pBundle).newMessage(DbmgrInterface::writeEntity);

	(*pBundle) << g_componentID;
	(*pBundle) << this->id();
	(*pBundle) << this->dbid();
	(*pBundle) << this->dbInterfaceIndex();
	(*pBundle) << this->pScriptModule()->getUType();
	(*pBundle) << callbackID;
	(*pBundle) << shouldAutoLoad;

	// 记录登录地址
	if(this->dbid() == 0)
	{
		uint32 ip = 0;
		uint16 port = 0;
		
		if(this->clientEntityCall())
		{
			ip = this->clientEntityCall()->addr().ip;
			port = this->clientEntityCall()->addr().port;
		}

		(*pBundle) << ip;
		(*pBundle) << port;
	}

	(*pBundle).append(*s);

	dbmgrinfos->pChannel->send(pBundle);
	MemoryStream::reclaimPoolObject(s);
}

//-------------------------------------------------------------------------------------
void Entity::onWriteToDB()
{
	SCOPED_PROFILE(SCRIPTCALL_PROFILE);

	PyObject* cd = cellDataDict_;
	if (!cd)
		cd = Py_None;

	SCRIPT_OBJECT_CALL_ARGS1(this, const_cast<char*>("onWriteToDB"), 
		const_cast<char*>("O"), cd, false);
}

//-------------------------------------------------------------------------------------
void Entity::onCellAppDeath()
{
	isArchiveing_ = false;
	isGetingCellData_ = false;
	resetRecoveryCellBackup();
	resetTeleport();
	completedMigrationEpoch_ = 0;
	migrationWitnessPending_ = false;
	destroyCellAfterHandoff_ = false;
}

//-------------------------------------------------------------------------------------
PyObject* Entity::createCellEntity(PyObject* pyobj)
{
	if(isDestroyed())
	{
		PyErr_Format(PyExc_AssertionError, "%s::createCellEntity: %d is destroyed!\n",
			scriptName(), id());
		PyErr_PrintEx(0);
		return 0;
	}

	if(Baseapp::getSingleton().findEntity(id()) == NULL)
	{
		PyErr_Format(PyExc_AssertionError, "%s::createCellEntity: %d not found!\n", 
			scriptName(), id());

		PyErr_PrintEx(0);
		return 0;
	}

	if(creatingCell_ || this->cellEntityCall())
	{
		PyErr_Format(PyExc_AssertionError, "%s::createCellEntity: %d has a cell!\n", 
			scriptName(), id());

		PyErr_PrintEx(0);
		return 0;
	}

	if(!PyObject_TypeCheck(pyobj, EntityCall::getScriptType()))
	{
		PyErr_Format(PyExc_TypeError, "create %s arg1 is not cellEntityCall!", 
			this->scriptName());

		PyErr_PrintEx(0);
		return 0;
	}
	
	EntityCallAbstract* cellEntityCall = static_cast<EntityCallAbstract*>(pyobj);
	if(cellEntityCall->type() != ENTITYCALL_TYPE_CELL &&
		cellEntityCall->type() != ENTITYCALL_TYPE_CELL_VIA_BASE)
	{
		PyErr_Format(PyExc_TypeError, "create %s arg1 is not a cell EntityCall!",
			this->scriptName());

		PyErr_PrintEx(0);
		return 0;
	}
	
	creatingCell_ = true;
	Baseapp::getSingleton().createCellEntity(cellEntityCall, this);
	S_Return;
}

//-------------------------------------------------------------------------------------
void Entity::restoreCell(EntityCallAbstract* cellEntityCall)
{
	if(creatingCell_ || inRestore_) return;

	creatingCell_ = true;
	inRestore_ = true;

	if(createdSpace_)
	{
		Baseapp::getSingleton().restoreSpaceInCell(this);
	}
	else
	{
		Baseapp::getSingleton().createCellEntity(cellEntityCall, this);
	}
}

//-------------------------------------------------------------------------------------
PyObject* Entity::createCellEntityInNewSpace(PyObject* args)
{
	if(isDestroyed())
	{
		PyErr_Format(PyExc_AssertionError, "%s::createCellEntityInNewSpace: %d is destroyed!\n",
			scriptName(), id());
		PyErr_PrintEx(0);
		return 0;
	}	

	if(createdSpace_ || this->cellEntityCall() != NULL)
	{
		PyErr_Format(PyExc_AssertionError, "%s::createCellEntityInNewSpace: %d in space!\n", 
			scriptName(), id());

		PyErr_PrintEx(0);
		return 0;
	}

	createdSpace_ = true;
	Baseapp::getSingleton().createCellEntityInNewSpace(this, args);
	S_Return;
}

//-------------------------------------------------------------------------------------
void Entity::forwardEntityMessageToCellappFromClient(Network::Channel* pChannel, MemoryStream& s)
{
	if(pChannel->proxyID() != this->id())
	{
		WARNING_MSG(fmt::format("{2}::forwardEntityMessageToCellappFromClient: not srcEntity({0}/{1}).\n",
			pChannel->proxyID(), this->id(), this->scriptName()));

		return;
	}

	EntityCall* mb = this->cellEntityCall();
	if(mb == NULL)
		return;

	// 将这个消息再打包转寄给cellapp， cellapp会对这个包中的每个消息进行判断
	// 检查是否是entity消息， 否则不合法.
	Network::Bundle* pBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
	(*pBundle).newMessage(CellappInterface::forwardEntityMessageToCellappFromClient);
	(*pBundle) << this->id();
	(*pBundle).append(s);
	sendToCellapp(pBundle);
}

//-------------------------------------------------------------------------------------
void Entity::onTeleportCB(Network::Channel* pChannel, SPACE_ID spaceID, bool fromCellTeleport)
{
	if(pChannel->isExternal())
		return;
	if (ServerConfig::getSingleton().getCellAppMgr().cellappmgr_space_partitioning_enabled)
	{
		if (!isCurrentCellChannel(pChannel, cellEntityCall()))
			return;
		// 换图通知可能先于提交确认；故障发生时 Space 必须仍和 Base 的旧路由对应。
		if (fromCellTeleport && migrationEpoch_)
			return;
	}
	
	if(spaceID > 0)
	{
		if (spaceID != this->spaceID())
			resetRecoveryCellBackup();
		if(!fromCellTeleport)
			onTeleportSuccess(spaceID);
		else
			this->spaceID(spaceID);
	}
	else
	{
		onTeleportFailure();
	}
}

//-------------------------------------------------------------------------------------
void Entity::onTeleportFailure()
{
	SCOPED_PROFILE(SCRIPTCALL_PROFILE);

	SCRIPT_OBJECT_CALL_ARGS0(this, const_cast<char*>("onTeleportFailure"), false);
}

//-------------------------------------------------------------------------------------
void Entity::onTeleportSuccess(SPACE_ID spaceID)
{
	SCOPED_PROFILE(SCRIPTCALL_PROFILE);

	this->spaceID(spaceID);
	SCRIPT_OBJECT_CALL_ARGS0(this, const_cast<char*>("onTeleportSuccess"), false);
}

namespace
{
void sendCellHandoff(ENTITY_ID id, const CellHandoff::Route& route,
	CellHandoff::Action action, COMPONENT_ID receiver, uint32 pid)
{
	auto* peer = Components::getSingleton().findComponent(CELLAPP_TYPE, receiver);
	if (!peer || (pid && peer->pid && peer->pid != pid) || !peer->pChannel ||
		peer->pChannel->isDestroyed() || peer->pChannel->condemn() != 0)
		return;

	auto* bundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
	bundle->newMessage(CellappInterface::onCellHandoff);
	(*bundle) << id << static_cast<uint8>(action);
	route.write(*bundle);
	peer->pChannel->send(bundle);
}
}

//-------------------------------------------------------------------------------------
bool Entity::retryCellHandoff()
{
	using namespace CellHandoff;
	const auto& route = cellHandoffRoute_;
	if (cellHandoffWarningAt_ && timestamp() >= cellHandoffWarningAt_)
	{
		cellHandoffWarningAt_ = 0;
		if (cellHandoffAction_ == IDENTIFY_TARGET)
			cellHandoffAction_ = RESTORE_SOURCE;
		else
			WARNING_MSG(fmt::format("Baseapp::cellHandoff: still pending, entity={}, epoch={}, source={}, target={}, action={}.\n",
				id(), route.epoch, route.source, route.target, static_cast<int>(cellHandoffAction_)));
	}

	switch (cellHandoffAction_)
	{
	case IDENTIFY_TARGET:
		if (!cellHandoffSourceExited_)
			sendCellHandoff(id(), route, IDENTIFY_TARGET, route.target, 0);
		return true;
	case CHECK_TARGET:
		if (!cellHandoffSourceExited_)
			sendCellHandoff(id(), route, DISPATCH, route.source, route.sourcePid);
		sendCellHandoff(id(), route, CHECK_TARGET, route.target, route.targetPid);
		return true;
	case RESTORE_SOURCE:
	case RELEASE_SOURCE:
		if (!cellHandoffSourceExited_)
			sendCellHandoff(id(), route, cellHandoffAction_, route.source, route.sourcePid);
		return true;
	case FINISH_TARGET:
		sendCellHandoff(id(), route, FINISH_TARGET, route.target, route.targetPid);
		return true;
	default:
		return retryTeleport();
	}
}

void Entity::onPartitionCellExit(const CellRecovery::LostReal& loss)
{
	using namespace CellHandoff;
	const auto& route = cellHandoffRoute_;
	const auto cid = loss.cellapp;
	if (isDestroyed() || !cellEntityCall() || !hasCellHandoffWith(cid) || spaceID() != loss.space ||
		route.space != loss.space || route.layoutVersion != loss.layoutVersion)
		return;

	const auto current = cellEntityCall()->componentID();
	if (cid == route.source)
	{
		if (cellHandoffSourceExited_)
			return;
		cellHandoffSourceExited_ = true;
		// 目标已确认并接到路由时，不再等已退出的源回包。未切换时仍要等目标确认。
		if (current == route.target && cellHandoffAction_ == RELEASE_SOURCE)
			cellHandoffAction_ = FINISH_TARGET;
	}
	else if (current == route.source &&
		(cellHandoffAction_ == IDENTIFY_TARGET || cellHandoffAction_ == CHECK_TARGET ||
			cellHandoffAction_ == RESTORE_SOURCE))
	{
		// 目标进程已退出且从未接到路由，源可以从本轮快照回滚。
		cellHandoffAction_ = RESTORE_SOURCE;
	}
	else if (current == route.target && cellHandoffAction_ == RELEASE_SOURCE)
	{
		// 保留退出确认，继续清理源 Ghost。路由已经切换，源快照不能再恢复为 Real。
		if (!cellHandoffTargetLoss_)
			cellHandoffTargetLoss_ = std::make_unique<CellRecovery::LostReal>(loss);
	}
	else if (current == route.target && cellHandoffAction_ == FINISH_TARGET)
	{
		// 源已清理，由 Base 按当前路由处理丢失的 Real。
		cellHandoffAction_ = NONE;
		cellHandoffWarningAt_ = 0;
	}
	else
		return;

	INFO_MSG(fmt::format("Baseapp::cellRecovery: reconciling handoff, entity={}, space={}, version={}, "
		"epoch={}, exited={}, action={}.\n", id(), loss.space, loss.layoutVersion, route.epoch, cid,
		static_cast<uint8>(cellHandoffAction_)));
	if (cellHandoffAction_ == FINISH_TARGET && cellHandoffTargetLoss_)
		finishCellHandoffSourceRelease();
	else
		retryCellHandoff();
}

void Entity::finishCellHandoffSourceRelease()
{
	if (auto loss = std::move(cellHandoffTargetLoss_))
	{
		cellHandoffAction_ = CellHandoff::NONE;
		cellHandoffWarningAt_ = 0;
		Baseapp::getSingleton().recoverPartitionReal(*this, *loss);
		return;
	}
	cellHandoffAction_ = CellHandoff::FINISH_TARGET;
	cellHandoffWarningAt_ = timestamp() + 5 * stampsPerSecond();
	retryCellHandoff();
}

bool Entity::markPartitionRealLost(const CellRecovery::LostReal& loss)
{
	if (isDestroyed() || !cellEntityCall() || cellEntityCall()->componentID() != loss.cellapp ||
		spaceID() != loss.space || hasPendingCellTransfer() || lostPartitionReal_)
		return false;

	// 此处只记录归属。没有核实备份前，不能清除引用，也不能创建替代实体。
	lostPartitionReal_ = std::make_unique<CellRecovery::LostReal>(loss);
	return true;
}

const std::vector<uint8>* Entity::partitionRecoveryCellData(const CellRecovery::LostReal& expected) const
{
	const auto* loss = lostPartitionReal_.get();
	const auto* backup = recoveryCellBackup_.get();
	const auto* cell = cellEntityCall();
	// 备份回包能容纳的数据略多于恢复请求。超限时按无可用备份处理，避免留下旧路由。
	if (isDestroyed() || !cell || cell->componentID() != expected.cellapp || spaceID() != expected.space ||
		hasPendingCellTransfer() || !loss || !(*loss == expected) || !backup || backup->data.empty() ||
		backup->data.size() > CellRecovery::MAX_STAGED_BACKUP_BYTES ||
		!backup->owns(loss->cellapp, loss->cellSession, loss->space, cellBackupGeneration_))
		return nullptr;
	return &backup->data;
}

bool Entity::routeRecoveredCell(const CellRecovery::LostReal& expected, COMPONENT_ID target)
{
	if (!partitionRecoveryCellData(expected))
		return false;
	cellEntityCall()->componentID(target);
	lostPartitionReal_.reset();
	resetRecoveryCellBackup();
	isGetingCellData_ = false;
	isArchiveing_ = false;
	inRestore_ = true;
	partitionRestorePending_ = true;
	return true;
}

void Entity::finishPartitionRestore()
{
	if (!partitionRestorePending_)
		return;
	partitionRestorePending_ = false;
	onRestore();
	if (isDestroyed() || !cellEntityCall())
		return;
	if (migrationWitnessPending_)
	{
		migrationWitnessPending_ = false;
		if (clientEntityCall())
			static_cast<Proxy*>(this)->onGetWitness();
	}
	if (destroyCellAfterHandoff_)
	{
		destroyCellAfterHandoff_ = false;
		destroyCellEntity();
	}
}

void Entity::cancelPartitionRestore()
{
	if (partitionRestorePending_)
	{
		partitionRestorePending_ = false;
		inRestore_ = false;
		migrationWitnessPending_ = false;
	}
}

namespace
{
void sendTeleportCompletion(ENTITY_ID id, COMPONENT_ID source, COMPONENT_ID target,
	COMPONENT_ID receiver, uint64 epoch, bool abort)
{
	auto* channel = Components::getSingleton().findComponentChannel(CELLAPP_TYPE, receiver);
	if (!channel || channel->isDestroyed() || channel->condemn() != 0)
		return;
	auto* bundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
	bundle->newMessage(CellappInterface::reqTeleportToCellAppOver);
	(*bundle) << id << source << target << epoch << static_cast<uint8>(abort);
	channel->send(bundle);
}
}

bool Entity::retryTeleport()
{
	if (!migrationEpoch_ || !cellEntityCall() || cellEntityCall()->componentID() != migrationAttemptSource_)
		return false;
	if (!migrationRollbackPending_ && migrationTargetProcess_ &&
		Baseapp::getSingleton().hasConfirmedCellProcessExit(migrationAttemptTarget_, migrationTargetProcess_))
	{
		// Base 仍指向源 Cell。目标的本轮进程已退出，可以用保存的快照恢复源端。
		migrationRollbackPending_ = true;
		migrationWitnessPending_ = true;
		INFO_MSG(fmt::format("{}::retryTeleport: rolling back after target exit, entity={}, source={}, target={}, epoch={}.\n",
			scriptName(), id(), migrationAttemptSource_, migrationAttemptTarget_, migrationEpoch_));
	}
	const uint64 now = timestamp();
	if (!migrationWarningAt_)
		migrationWarningAt_ = now + 5 * stampsPerSecond();
	else if (now >= migrationWarningAt_)
	{
		WARNING_MSG(fmt::format("{}::retryTeleport: waiting for CellApp, entity={}, source={}, target={}, epoch={}, rollback={}.\n",
			scriptName(), id(), migrationAttemptSource_, migrationAttemptTarget_, migrationEpoch_, migrationRollbackPending_));
		migrationWarningAt_ = now + 30 * stampsPerSecond();
	}
	if (migrationRollbackPending_)
		sendTeleportCompletion(id(), migrationAttemptSource_, migrationAttemptTarget_,
			migrationAttemptSource_, migrationEpoch_, true);
	else if (migrationCommitPending_)
	{
		auto* channel = Components::getSingleton().findComponentChannel(CELLAPP_TYPE, migrationCommitTargetCellAppID_);
		if (channel && !channel->isDestroyed() && channel->condemn() == 0)
		{
			auto* bundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
			bundle->newMessage(CellappInterface::reqTeleportToCellAppPrepare);
			(*bundle) << id() << migrationCommitSourceCellAppID_ << migrationCommitTargetCellAppID_ << migrationEpoch_;
			channel->send(bundle);
		}
	}
	return true;
}

//-------------------------------------------------------------------------------------
void Entity::onCellHandoff(Network::Channel* channel, MemoryStream& stream)
{
	using namespace CellHandoff;
	BoundedStreamReader reader(stream);
	Route route;
	uint8 action = NONE;
	const bool parsed = reader.read(action) && route.read(reader) && reader.empty();
	stream.done();
	auto& components = Components::getSingleton();
	if (!parsed || !channel || !cellEntityCall() || isDestroyed() ||
		!components.isExpectedComponentChannel(CELLAPP_TYPE, channel))
		return;
	const auto* sender = components.findComponent(CELLAPP_TYPE, channel->componentID());
	if (!sender)
		return;
	const bool fromSource = sender->cid == route.source && (!sender->pid || sender->pid == route.sourcePid);
	const bool fromTarget = sender->cid == route.target && route.targetPid &&
		(!sender->pid || sender->pid == route.targetPid);
	if (action == START)
	{
		if (!fromSource || route.targetPid || cellHandoffRoute_.sameAttempt(route))
			return;
		if (cellEntityCall()->componentID() != route.source || spaceID() != route.space ||
			route.epoch <= cellHandoffRoute_.epoch ||
			cellHandoffAction_ != NONE || migrationEpoch_ || pBufferedSendToClientMessages_ ||
			hasFlags(ENTITY_FLAGS_TELEPORT_START) || hasFlags(ENTITY_FLAGS_TELEPORT_STOP))
		{
			// 未批准本轮交接，源端尚未向目标发送快照。
			sendCellHandoff(id(), route, RESTORE_SOURCE, route.source, route.sourcePid);
			return;
		}
		cellHandoffRoute_ = route;
		cellHandoffAction_ = IDENTIFY_TARGET;
		cellHandoffSourceExited_ = false;
		cellHandoffWarningAt_ = timestamp() + 5 * stampsPerSecond();
		Baseapp::getSingleton().trackCellHandoff(id());
		retryCellHandoff();
		return;
	}
	if (cellHandoffAction_ == IDENTIFY_TARGET && !cellHandoffSourceExited_ && action == TARGET_IDENTIFIED && fromTarget &&
		cellHandoffRoute_.sameAttempt(route) && cellEntityCall()->componentID() == route.source)
	{
		cellHandoffRoute_.targetPid = route.targetPid;
		cellHandoffAction_ = CHECK_TARGET;
		cellHandoffWarningAt_ = timestamp() + 5 * stampsPerSecond();
		retryCellHandoff();
		return;
	}
	if (!(route == cellHandoffRoute_))
		return;
	const auto current = cellEntityCall()->componentID();
	if (cellHandoffAction_ == CHECK_TARGET && current == route.source)
	{
		if (action == TARGET_INSTALLED && fromTarget)
		{
			sendCellHandoff(id(), route, CHECK_TARGET, route.target, route.targetPid);
			return;
		}
		if (action == TARGET_READY && fromTarget)
		{
			// 切换路由后只重发释放和完成通知，不再恢复源实体。
			cellEntityCall()->componentID(route.target);
			resetRecoveryCellBackup();
			cellHandoffAction_ = cellHandoffSourceExited_ ? FINISH_TARGET : RELEASE_SOURCE;
		}
		else if ((action == TARGET_REJECTED && fromTarget) || (action == SOURCE_UNSENT && fromSource))
			cellHandoffAction_ = RESTORE_SOURCE;
		else
			return;
	}
	else if (cellHandoffAction_ == RELEASE_SOURCE && current == route.target &&
		action == SOURCE_RELEASED && fromSource)
	{
		finishCellHandoffSourceRelease();
		return;
	}
	else if ((cellHandoffAction_ == FINISH_TARGET && current == route.target &&
		action == TARGET_FINISHED && fromTarget) ||
		(cellHandoffAction_ == RESTORE_SOURCE && current == route.source &&
			action == SOURCE_RESTORED && fromSource))
	{
		if (cellHandoffAction_ == RESTORE_SOURCE && recoveryCellBackup_)
			recoveryCellBackup_->request = 0;
		cellHandoffAction_ = NONE;
	}
	else
		return;

	cellHandoffWarningAt_ = cellHandoffAction_ == NONE ? 0 : timestamp() + 5 * stampsPerSecond();
	if (cellHandoffAction_ == NONE && destroyCellAfterHandoff_)
	{
		destroyCellAfterHandoff_ = false;
		destroyCellEntity();
	}
	retryCellHandoff();
}

void Entity::onMigrationCellappReserve(Network::Channel* channel, MemoryStream& stream)
{
	BoundedStreamReader reader(stream);
	TeleportStatus::Dispatch route;
	const bool parsed = route.read(reader) && reader.empty();
	stream.done();
	if (!parsed || !channel || channel->isExternal() || channel->isDestroyed() || channel->condemn() != 0 ||
		!ServerConfig::getSingleton().getCellAppMgr().cellappmgr_space_partitioning_enabled ||
		!Components::getSingleton().isExpectedComponentChannel(CELLAPP_TYPE, channel))
		return;
	const auto source = channel->componentID();
	if (!source || source == route.target)
		return;

	const bool sameSource = !isDestroyed() && cellEntityCall() &&
		cellEntityCall()->componentID() == source && spaceID() == route.origin;
	const bool duplicate = migrationEpoch_ == route.epoch &&
		migrationAttemptSource_ == source && migrationAttemptTarget_ == route.target &&
		migrationTargetProcess_ == route.targetProcess;
	const bool available = !migrationEpoch_ && cellHandoffAction_ == CellHandoff::NONE &&
		!pBufferedSendToClientMessages_ && !hasFlags(ENTITY_FLAGS_TELEPORT_START) &&
		!hasFlags(ENTITY_FLAGS_TELEPORT_STOP) && route.epoch > completedMigrationEpoch_ &&
		route.epoch > cellHandoffRoute_.epoch &&
		!Baseapp::getSingleton().hasConfirmedCellProcessExit(route.target, route.targetProcess);
	const bool accepted = sameSource && (available || duplicate);
	if (accepted && !duplicate)
	{
		// 先记录本轮迁出。源 Cell 收到回包后才会向目标发送快照。
		migrationEpoch_ = route.epoch;
		migrationAttemptSource_ = source;
		migrationAttemptTarget_ = route.target;
		migrationTargetProcess_ = route.targetProcess;
		addFlags(ENTITY_FLAGS_TELEPORT_START);
		Baseapp::getSingleton().trackCellHandoff(id());
	}
	auto* reply = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
	reply->newMessage(CellappInterface::onTeleportDispatchPermission);
	(*reply) << id();
	route.write(*reply);
	(*reply) << static_cast<uint8>(accepted);
	channel->send(reply);
}

//-------------------------------------------------------------------------------------
void Entity::onMigrationCellappStart(Network::Channel* pChannel, COMPONENT_ID sourceCellAppID,
	COMPONENT_ID targetCellAppID, uint64 epoch, uint8 refreshWitness)
{
	if (!pChannel || pChannel->componentID() != sourceCellAppID ||
		!Components::getSingleton().isExpectedComponentChannel(CELLAPP_TYPE, pChannel) ||
		!cellEntityCall() || !epoch || !sourceCellAppID || !targetCellAppID ||
		sourceCellAppID == targetCellAppID || refreshWitness > 1)
		return;
	if (epoch <= completedMigrationEpoch_)
	{
		// 源端未收到释放通知会重报 Start；源端仍按本轮编号和 Ghost 状态校验。
		sendTeleportCompletion(id(), sourceCellAppID, targetCellAppID, sourceCellAppID, epoch, false);
		if (refreshWitness && epoch == completedMigrationEpoch_)
		{
			migrationWitnessPending_ = true;
			refreshTeleportWitness();
		}
		return;
	}
	if (!migrationEpoch_ && ServerConfig::getSingleton().getCellAppMgr().cellappmgr_space_partitioning_enabled)
		return;
	if (cellHandoffAction_ != CellHandoff::NONE || cellEntityCall()->componentID() != sourceCellAppID ||
		(migrationEpoch_ && (epoch != migrationEpoch_ || migrationAttemptSource_ != sourceCellAppID ||
			migrationAttemptTarget_ != targetCellAppID)))
		return;

	migrationWitnessPending_ |= refreshWitness != 0;
	if (migrationRollbackPending_ || migrationCommitPending_ || hasFlags(ENTITY_FLAGS_TELEPORT_START))
		return;
	DEBUG_MSG(fmt::format("{}::onMigrationCellappStart: {}, sourceCellAppID={}, targetCellappID={}\n",
		scriptName(), id(), sourceCellAppID, targetCellAppID));
	migrationEpoch_ = epoch;
	migrationAttemptSource_ = sourceCellAppID;
	migrationAttemptTarget_ = targetCellAppID;
	Baseapp::getSingleton().trackCellHandoff(id());

	if (hasFlags(ENTITY_FLAGS_TELEPORT_STOP))
	{
		// End and Start travel over different CellApp channels and may be reordered. Only
		// the Start matching the saved End route may consume STOP; a stale Start must not
		// release another migration's buffered client messages. / End 与 Start 经过不同
		// CellApp Channel，可能乱序；只有匹配已保存 End 路由的 Start 才能消费 STOP。
		if (deferredMigrationSourceCellAppID_ != sourceCellAppID ||
			deferredMigrationTargetCellAppID_ != targetCellAppID)
		{
			WARNING_MSG(fmt::format("{}::onMigrationCellappStart: ignored stale route while waiting "
				"for deferred Start, entity={}, deferredSource={}, deferredTarget={}, source={}, target={}.\n",
				scriptName(), id(), deferredMigrationSourceCellAppID_,
				deferredMigrationTargetCellAppID_, sourceCellAppID, targetCellAppID));
			return;
		}

		const COMPONENT_ID deferredSourceCellAppID = deferredMigrationSourceCellAppID_;
		const COMPONENT_ID deferredTargetCellAppID = deferredMigrationTargetCellAppID_;
		removeFlags(ENTITY_FLAGS_TELEPORT_STOP);
		deferredMigrationSourceCellAppID_ = 0;
		deferredMigrationTargetCellAppID_ = 0;

		if (pBufferedSendToClientMessages_)
		{
			pBufferedSendToClientMessages_->startForward();
			return;
		}

		// The forwarding task can deliberately self-complete after its bounded wait. Its
		// callback has already started (or completed) the saved End route, so a later Start
		// is a recoverable ordering event, never a process-fatal invariant. If commit could
		// not start earlier (for example Target discovery was transiently unavailable), retry
		// the same saved route now. / 转发任务会在有界等待后主动完成；其回调已按保存的
		// End 路由开始收口。迟到 Start 属于可恢复乱序；若此前提交未启动，则重试同一路由。
		const bool sameCommitPending = migrationCommitPending_ &&
			migrationCommitSourceCellAppID_ == deferredSourceCellAppID &&
			migrationCommitTargetCellAppID_ == deferredTargetCellAppID;
		EntityCall* pCellEntityCall = cellEntityCall();
		const bool routeAlreadyCommitted = pCellEntityCall != NULL &&
			pCellEntityCall->componentID() == deferredTargetCellAppID;

		if (sameCommitPending || routeAlreadyCommitted)
		{
			WARNING_MSG(fmt::format("{}::onMigrationCellappStart: consumed delayed Start after "
				"forwarding completion, entity={}, sourceCellAppID={}, targetCellAppID={}, committed={}.\n",
				scriptName(), id(), deferredSourceCellAppID, deferredTargetCellAppID,
				routeAlreadyCommitted));
			return;
		}

		WARNING_MSG(fmt::format("{}::onMigrationCellappStart: retrying deferred migration completion "
			"after forwarding task release, entity={}, sourceCellAppID={}, targetCellAppID={}.\n",
			scriptName(), id(), deferredSourceCellAppID, deferredTargetCellAppID));
		onMigrationCellappOver(deferredSourceCellAppID, deferredTargetCellAppID);
		return;
	}

	addFlags(ENTITY_FLAGS_TELEPORT_START);
}

//-------------------------------------------------------------------------------------
void Entity::onMigrationCellappEnd(Network::Channel* pChannel, COMPONENT_ID sourceCellAppID,
	COMPONENT_ID targetCellAppID, uint64 epoch, SPACE_ID spaceID)
{
	if (!pChannel || pChannel->componentID() != targetCellAppID ||
		!Components::getSingleton().isExpectedComponentChannel(CELLAPP_TYPE, pChannel) ||
		cellHandoffAction_ != CellHandoff::NONE || !cellEntityCall() || !epoch || !spaceID)
		return;
	if (epoch <= completedMigrationEpoch_)
	{
		if (epoch == completedMigrationEpoch_ && cellEntityCall()->componentID() == targetCellAppID && !migrationEpoch_)
		{
			sendTeleportCompletion(id(), sourceCellAppID, targetCellAppID, targetCellAppID, epoch, false);
			if (sourceCellAppID != targetCellAppID)
				sendTeleportCompletion(id(), sourceCellAppID, targetCellAppID, sourceCellAppID, epoch, false);
		}
		return;
	}
	
	DEBUG_MSG(fmt::format("{}::onMigrationCellappEnd: {}, sourceCellAppID={}, targetCellappID={}\n",
		scriptName(), id(), sourceCellAppID, targetCellAppID));

	if (sourceCellAppID == 0 || targetCellAppID == 0)
	{
		WARNING_MSG(fmt::format("{}::onMigrationCellappEnd: rejected invalid migration route, "
			"entity={}, sourceCellAppID={}, targetCellAppID={}.\n",
			scriptName(), id(), sourceCellAppID, targetCellAppID));
		return;
	}
	if (cellEntityCall()->componentID() != sourceCellAppID ||
		(migrationEpoch_ && (migrationEpoch_ != epoch || migrationAttemptSource_ != sourceCellAppID)))
		return;
	if (sourceCellAppID == targetCellAppID)
	{
		// 源端已恢复，不再等待目标校验或重放目标端的客户端消息。
		// 目标如果已进入 Space，客户端可能已经换场景，回滚后补发源端视野。
		migrationWitnessPending_ |= migrationTargetSession_ != 0;
		migrationEpoch_ = epoch;
		migrationSpace_ = spaceID;
		commitMigrationCellapp(sourceCellAppID, targetCellAppID);
		return;
	}
	if (!migrationEpoch_ && ServerConfig::getSingleton().getCellAppMgr().cellappmgr_space_partitioning_enabled)
		return;
	if (migrationRollbackPending_ || (migrationEpoch_ && migrationAttemptTarget_ != targetCellAppID))
		return;
	migrationEpoch_ = epoch;
	migrationAttemptSource_ = sourceCellAppID;
	migrationAttemptTarget_ = targetCellAppID;
	migrationSpace_ = spaceID;
	if (!migrationTargetSession_)
		migrationTargetSession_ = pChannel->sessionEpoch();
	Baseapp::getSingleton().trackCellHandoff(id());

	if (migrationCommitPending_ &&
		migrationCommitSourceCellAppID_ == sourceCellAppID &&
		migrationCommitTargetCellAppID_ == targetCellAppID)
	{
		// Target 已经进入提交确认阶段；重复 End 不能重新创建转发缓冲或改变迁移状态。
		// Target is already in commit validation; a duplicate End must not recreate forwarding state.
		return;
	}

	if (hasFlags(ENTITY_FLAGS_TELEPORT_STOP) && pBufferedSendToClientMessages_ == NULL)
	{
		if (deferredMigrationSourceCellAppID_ != sourceCellAppID ||
			deferredMigrationTargetCellAppID_ != targetCellAppID)
		{
			WARNING_MSG(fmt::format("{}::onMigrationCellappEnd: ignored stale End while waiting "
				"for deferred Start, entity={}, deferredSource={}, deferredTarget={}, source={}, target={}.\n",
				scriptName(), id(), deferredMigrationSourceCellAppID_,
				deferredMigrationTargetCellAppID_, sourceCellAppID, targetCellAppID));
			return;
		}

		// The prior forwarding task has already drained and attempted this End route. A
		// duplicate End may retry commit discovery, but must not allocate another long-lived
		// task while the matching Start is still in flight. / 前一转发任务已经排空并尝试
		// 该 End 路由；重复 End 可以重试提交发现，但不能在等待 Start 时再创建长生命周期任务。
		onMigrationCellappOver(deferredMigrationSourceCellAppID_,
			deferredMigrationTargetCellAppID_);
		return;
	}

	if (pBufferedSendToClientMessages_)
		return;
	
	// 某些极端情况下可能onMigrationCellappStart会慢于onMigrationCellappEnd触发，此时必须设置标记
	// 等待onMigrationCellappEnd触发后做清理
	if (!hasFlags(ENTITY_FLAGS_TELEPORT_START))
	{
		addFlags(ENTITY_FLAGS_TELEPORT_STOP);
		deferredMigrationSourceCellAppID_ = sourceCellAppID;
		deferredMigrationTargetCellAppID_ = targetCellAppID;

		if (pBufferedSendToClientMessages_ == NULL)
		{
			pBufferedSendToClientMessages_ = new BaseMessagesForwardClientHandler(
				this, sourceCellAppID, targetCellAppID);
		}

		pBufferedSendToClientMessages_->stopForward();
	}
	else
	{
		removeFlags(ENTITY_FLAGS_TELEPORT_START);
		onMigrationCellappOver(sourceCellAppID, targetCellAppID);
	}
}

//-------------------------------------------------------------------------------------
void Entity::onMigrationCellappOver(COMPONENT_ID sourceCellAppID, COMPONENT_ID targetCellAppID)
{
	if (sourceCellAppID == 0 || targetCellAppID == 0)
	{
		ERROR_MSG(fmt::format("{}::onMigrationCellappOver: invalid migration route, "
			"entity={}, sourceCellAppID={}, targetCellAppID={}.\n",
			scriptName(), id(), sourceCellAppID, targetCellAppID));
		return;
	}

	// 目标创建失败时 Source 已经由 CellApp 恢复为 real，直接收口单端回滚。
	// A failed Target creation has already restored Source as real, so finish the one-sided rollback directly.
	if (sourceCellAppID == targetCellAppID)
	{
		commitMigrationCellapp(sourceCellAppID, targetCellAppID);
		return;
	}

	if (migrationCommitPending_)
	{
		if (migrationCommitSourceCellAppID_ == sourceCellAppID &&
			migrationCommitTargetCellAppID_ == targetCellAppID)
		{
			return;
		}

		ERROR_MSG(fmt::format("{}::onMigrationCellappOver: another commit validation is pending, "
			"entity={}, pendingSource={}, pendingTarget={}, source={}, target={}.\n",
			scriptName(), id(), migrationCommitSourceCellAppID_, migrationCommitTargetCellAppID_,
			sourceCellAppID, targetCellAppID));
		return;
	}

	// End 只表示 Target 曾完成初始化。真正提交前必须沿 BaseApp->Target 独立往返再次
	// 验证实体和 Space 仍存活；在成功响应前不切路由，也绝不释放 Source。
	// End only proves that Target finished initialization at an earlier instant. Before commit,
	// an independent BaseApp-to-Target round trip revalidates Entity and Space lifetime; routing
	// remains on Source and Source is never released before the successful response.
	migrationCommitPending_ = true;
	migrationCommitSourceCellAppID_ = sourceCellAppID;
	migrationCommitTargetCellAppID_ = targetCellAppID;

	retryTeleport();
}

//-------------------------------------------------------------------------------------
void Entity::onMigrationCellappPrepared(Network::Channel* pChannel,
	COMPONENT_ID sourceCellAppID, COMPONENT_ID targetCellAppID, uint64 epoch, uint8 status)
{
	if (pChannel == NULL || pChannel->isExternal() ||
		!Components::getSingleton().isExpectedComponentChannel(CELLAPP_TYPE, pChannel) ||
		pChannel->componentID() != targetCellAppID)
	{
		WARNING_MSG(fmt::format("{}::onMigrationCellappPrepared: rejected invalid Target source, "
			"entity={}, sourceCellAppID={}, targetCellAppID={}.\n",
			scriptName(), id(), sourceCellAppID, targetCellAppID));
		return;
	}

	if (!epoch || epoch != migrationEpoch_ || migrationAttemptSource_ != sourceCellAppID ||
		migrationAttemptTarget_ != targetCellAppID || !cellEntityCall() ||
		cellEntityCall()->componentID() != sourceCellAppID || migrationRollbackPending_)
	{
		return;
	}

	if (migrationTargetProcess_ &&
		Baseapp::getSingleton().hasConfirmedCellProcessExit(targetCellAppID, migrationTargetProcess_))
	{
		retryTeleport();
		return;
	}

	if (status == TeleportStatus::NOT_INSTALLED ||
		(status == TeleportStatus::REJECTED && migrationCommitPending_ &&
			pChannel->sessionEpoch() == migrationTargetSession_))
	{
		// 是否恢复由 Base 决定；切到目标后，即使原目标已不存在也不能回滚。
		migrationRollbackPending_ = true;
		retryTeleport();
		return;
	}
	if (status == TeleportStatus::READY && migrationCommitPending_)
		commitMigrationCellapp(sourceCellAppID, targetCellAppID);
}

//-------------------------------------------------------------------------------------
void Entity::commitMigrationCellapp(COMPONENT_ID sourceCellAppID, COMPONENT_ID targetCellAppID)
{
	const uint64 epoch = migrationEpoch_;
	completedMigrationEpoch_ = epoch;
	if (sourceCellAppID != targetCellAppID || spaceID() != migrationSpace_)
		resetRecoveryCellBackup();
	else if (recoveryCellBackup_)
		recoveryCellBackup_->request = 0;
	spaceID(migrationSpace_);
	resetTeleport();
	// BaseApp 是客户端到 Cell RPC 的唯一权威路由点。Target 已完成提交时存活确认后，
	// 必须先切换路由，再确认两端完成，保证 Source Ghost 覆盖旧 Channel 的尾部 RPC。
	// BaseApp is the authoritative client-to-Cell routing point. After Target's commit-time
	// liveness validation, switch routing before confirming either peer so Source Ghost covers
	// the tail of RPCs already queued on the old Channel.
	if(this->cellEntityCall())
		this->cellEntityCall()->componentID(targetCellAppID);

	// A failed migration reports source==target after restoring the Source real Entity.
	// Confirm it once; sending the same completion twice races lifecycle callbacks and
	// can duplicate script-visible Cell-loss handling. / 迁移失败恢复 Source real Entity
	// 后会报告 source==target；此时只确认一次，避免重复完成与生命周期回调竞态。
	sendTeleportCompletion(id(), sourceCellAppID, targetCellAppID, targetCellAppID, epoch, false);
	if (sourceCellAppID != targetCellAppID)
		sendTeleportCompletion(id(), sourceCellAppID, targetCellAppID, sourceCellAppID, epoch, false);
	if (destroyCellAfterHandoff_)
	{
		destroyCellAfterHandoff_ = false;
		migrationWitnessPending_ = false;
		destroyCellEntity();
	}
	else
		refreshTeleportWitness();
}

void Entity::resetTeleport()
{
	migrationEpoch_ = migrationTargetSession_ = migrationWarningAt_ = 0;
	migrationTargetProcess_ = 0;
	migrationAttemptSource_ = migrationAttemptTarget_ = 0;
	migrationCommitPending_ = migrationRollbackPending_ = false;
	migrationSpace_ = 0;
	migrationCommitSourceCellAppID_ = migrationCommitTargetCellAppID_ = 0;
	deferredMigrationSourceCellAppID_ = deferredMigrationTargetCellAppID_ = 0;
	removeFlags(ENTITY_FLAGS_TELEPORT_START | ENTITY_FLAGS_TELEPORT_STOP);
	SAFE_RELEASE(pBufferedSendToClientMessages_);
}

void Entity::refreshTeleportWitness()
{
	if (!migrationWitnessPending_ || migrationEpoch_ || cellHandoffAction_ != CellHandoff::NONE)
		return;
	migrationWitnessPending_ = false;
	auto* client = clientEntityCall();
	auto* channel = client ? client->getChannel() : nullptr;
	if (channel && !channel->isDestroyed() && channel->condemn() == 0 && channel->proxyID() == id())
		static_cast<Proxy*>(this)->onGetWitness();
}

//-------------------------------------------------------------------------------------
void Entity::onBufferedForwardToCellappMessagesOver()
{
}

//-------------------------------------------------------------------------------------
void Entity::onBufferedForwardToClientMessagesOver()
{
	const auto source = pBufferedSendToClientMessages_->sourceCellappID();
	const auto target = pBufferedSendToClientMessages_->cellappID();
	SAFE_RELEASE(pBufferedSendToClientMessages_);
	onMigrationCellappOver(source, target);
}

//-------------------------------------------------------------------------------------
void Entity::onGetDBID(Network::Channel* pChannel, DBID dbid)
{
	if(pChannel->isExternal())
		return;
}

//-------------------------------------------------------------------------------------
void Entity::onTimer(ScriptID timerID, int useraAgs)
{
	SCOPED_PROFILE(ONTIMER_PROFILE);

	CALL_ENTITY_AND_COMPONENTS_METHOD(this, SCRIPT_OBJECT_CALL_ARGS2(pyTempObj,
		const_cast<char*>("onTimer"), const_cast<char*>("Ii"), timerID, useraAgs, GETERR));
}

//-------------------------------------------------------------------------------------
bool Entity::_reload(bool fullReload)
{
	return true;
}

//-------------------------------------------------------------------------------------
}
