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


#include "cellapp.h"
#include "space.h"
#include "space_entity.h"
#include "profile.h"
#include "witness.h"
#include "coordinate_node.h"
#include "view_trigger.h"
#include "watch_obj_pools.h"
#include "cellapp_interface.h"
#include "entity_remotemethod.h"
#include "initprogress_handler.h"
#include "forward_message_over_handler.h"
#include "network/tcp_packet.h"
#include "network/udp_packet.h"
#include "network/network_stats.h"
#include "server/client_request_guard.h"
#include "server/bounded_stream_reader.h"
#include "server/component_routing_guard.h"
#include "server/components.h"
#include "server/telnet_server.h"
#include "server/py_file_descriptor.h"
#include "dbmgr/dbmgr_interface.h"
#include "navigation/navigation.h"
#include "client_lib/client_interface.h"
#include "common/sha1.h"

#include "../../server/baseappmgr/baseappmgr_interface.h"
#include "../../server/cellappmgr/cellappmgr_interface.h"
#include "../../server/baseapp/baseapp_interface.h"
#include "../../server/cellapp/cellapp_interface.h"
#include "../../server/dbmgr/dbmgr_interface.h"
#include "../../server/loginapp/loginapp_interface.h"

namespace KBEngine{
	
ServerConfig g_serverConfig;
KBE_SINGLETON_INIT(Cellapp);

Navigation g_navigation;

namespace
{
const size_t ENTITY_TYPE_MAX_LENGTH = 128;

bool validateSpaceCreationForward(const MemoryStream& stream)
{
	BoundedStreamReader reader(stream);
	ENTITY_ID entityID = 0;
	SPACE_ID spaceID = 0;
	COMPONENT_ID componentID = 0;
	return reader.skipString(ENTITY_TYPE_MAX_LENGTH, false) &&
		reader.read(entityID) && entityID > 0 &&
		reader.read(spaceID) && spaceID > 0 &&
		reader.read(componentID) && componentID > 0 &&
		reader.skip(sizeof(uint8));
}

bool validateEntityCreationForward(const MemoryStream& stream)
{
	BoundedStreamReader reader(stream);
	ENTITY_ID createToEntityID = 0;
	ENTITY_ID entityID = 0;
	COMPONENT_ID componentID = 0;
	return reader.read(createToEntityID) && createToEntityID > 0 &&
		reader.skipString(ENTITY_TYPE_MAX_LENGTH, false) &&
		reader.read(entityID) && entityID > 0 &&
		reader.read(componentID) && componentID > 0 &&
		reader.skip(sizeof(uint8) * 2);
}

Components::ComponentInfos* findBoundCellappSource(Network::Channel* pChannel)
{
	if (pChannel == NULL || pChannel->isDestroyed() || pChannel->condemn() != 0)
		return NULL;

	Components& components = Components::getSingleton();
	Components::ComponentInfos* infos = components.findComponent(pChannel);
	if (infos == NULL && pChannel->componentID() != 0)
		infos = components.findComponent(CELLAPP_TYPE, pChannel->componentID());

	if (infos == NULL || infos->componentType != CELLAPP_TYPE || infos->cid == 0 ||
		!Security::isBoundBidirectionalComponentSource(infos->cid, infos, pChannel,
			pChannel->componentID()))
	{
		return NULL;
	}

	return infos;
}

bool isCurrentGhostPeer(Entity* entity, COMPONENT_ID sourceCellID)
{
	if (entity == NULL || sourceCellID == 0)
		return false;

	const COMPONENT_ID peerCellID = entity->isReal() ? entity->ghostCell() : entity->realCell();
	return peerCellID != 0 && peerCellID == sourceCellID;
}

Components::ComponentInfos* validateBaseappEntityCreationSource(
	Network::Channel* pChannel, COMPONENT_ID componentID,
	const std::string& entityType, bool hasClient, bool allowCellappmgrRelay,
	const char* operation)
{
	Components& components = Components::getSingleton();
	Components::ComponentInfos* sourceInfos = componentID == 0 ? NULL :
		components.findComponent(BASEAPP_TYPE, componentID);
	Components::ComponentInfos* channelInfos = components.findComponent(pChannel);
	if (channelInfos == NULL && pChannel != NULL && pChannel->componentID() != 0)
		channelInfos = components.findComponent(pChannel->componentID());
	ScriptDefModule* scriptModule = EntityDef::findScriptModule(entityType.c_str());
	const bool validScriptModule = scriptModule != NULL && scriptModule->hasCell() &&
		(!hasClient || scriptModule->hasClient());
	const bool validDirectSource = sourceInfos != NULL &&
		Security::isBoundBidirectionalComponentSource(componentID, sourceInfos,
			pChannel, pChannel != NULL ? pChannel->componentID() : 0);
	const bool validRelay = allowCellappmgrRelay && channelInfos != NULL &&
		channelInfos->componentType == CELLAPPMGR_TYPE &&
		Security::isBoundBidirectionalComponentSource(channelInfos->cid, channelInfos,
			pChannel, pChannel != NULL ? pChannel->componentID() : 0);
	const bool validChannel = pChannel != NULL && !pChannel->isExternal() &&
		!pChannel->isDestroyed() && pChannel->condemn() == 0 &&
		(validDirectSource || validRelay);
	if (!validChannel || !validScriptModule)
	{
		WARNING_MSG(fmt::format("{}: rejected componentID={}, entityType={}, hasClient={}, addr={}.\n",
			operation, componentID, entityType, hasClient,
			pChannel != NULL ? pChannel->c_str() : "none"));
		return NULL;
	}

	return sourceInfos;
}

bool bindClientStateForCreatedEntity(Entity* entity, const char* operation)
{
	if (entity == NULL || entity->baseEntityCall() == NULL || entity->hasWitness())
	{
		ERROR_MSG(fmt::format("{}: rejected invalid client binding state, entityID={}.\n",
			operation, entity != NULL ? entity->id() : 0));
		return false;
	}

	PyObject* clientEntityCall = PyObject_GetAttrString(entity->baseEntityCall(), "client");
	if (clientEntityCall == NULL || clientEntityCall == Py_None ||
		!PyObject_TypeCheck(clientEntityCall, EntityCall::getScriptType()))
	{
		ERROR_MSG(fmt::format("{}: rejected missing client EntityCall, entityID={}.\n",
			operation, entity->id()));
		Py_XDECREF(clientEntityCall);
		SCRIPT_ERROR_CHECK();
		return false;
	}

	// PyObject_GetAttrString returns a new reference; the Entity owns it after binding.
	// PyObject_GetAttrString 返回新引用，绑定后由 Entity 持有该引用。
	entity->clientEntityCall(static_cast<EntityCall*>(clientEntityCall));
	entity->setWitness(Witness::createPoolObject(OBJECTPOOL_POINT));
	return true;
}
}

//-------------------------------------------------------------------------------------
Cellapp::Cellapp(Network::EventDispatcher& dispatcher,
			 Network::NetworkInterface& ninterface, 
			 COMPONENT_TYPE componentType,
			 COMPONENT_ID componentID):
	EntityApp<Entity>(dispatcher, ninterface, componentType, componentID),
	pCellAppData_(NULL),
	forward_messagebuffer_(ninterface),
	cells_(),
	pTelnetServer_(NULL),
	pWitnessedTimeoutHandler_(NULL),
	pGhostManager_(NULL),
	flags_(APP_FLAGS_NONE),
	spaceViewers_(),
	pInitProgressHandler_(NULL),
	staleWitnessVolatileControlDrops_(0)
{
	KBEngine::Network::MessageHandlers::pMainMessageHandlers = &CellappInterface::messageHandlers;

	// hook entitycall
	static EntityCall::EntityCallCallHookFunc entityCallCallHookFunc = std::bind(&Cellapp::createEntityCallCallEntityRemoteMethod, this,
		std::placeholders::_1, std::placeholders::_2);

	EntityCall::setEntityCallCallHookFunc(&entityCallCallHookFunc);
}

//-------------------------------------------------------------------------------------
Cellapp::~Cellapp()
{
	pInitProgressHandler_ = NULL;
	EntityCall::resetCallHooks();
}

//-------------------------------------------------------------------------------------	
ShutdownHandler::CAN_SHUTDOWN_STATE Cellapp::canShutdown()
{
	Entities<Entity>::ENTITYS_MAP& entities =  this->pEntities()->getEntities();
	Entities<Entity>::ENTITYS_MAP::iterator iter = entities.begin();
	for(; iter != entities.end(); ++iter)
	{
		//Entity* pEntity = static_cast<Entity*>(iter->second.get());
		//if(pEntity->baseEntityCall() != NULL && 
		//		pEntity->pScriptModule()->isPersistent())
		{
			INFO_MSG(fmt::format("Cellapp::canShutdown(): Wait for the entity's into the database! The remaining {}.\n",
				entities.size()));

			lastShutdownFailReason_ = "destroyHasBaseEntitys";
			return ShutdownHandler::CAN_SHUTDOWN_STATE_FALSE;
		}
	}

	return ShutdownHandler::CAN_SHUTDOWN_STATE_TRUE;
}

//-------------------------------------------------------------------------------------	
void Cellapp::onShutdown(bool first)
{
	EntityApp<Entity>::onShutdown(first);

	uint32 count = g_serverConfig.getCellApp().perSecsDestroyEntitySize;
	Entities<Entity>::ENTITYS_MAP& entities =  this->pEntities()->getEntities();

	while(count > 0 && entities.size() > 0)
	{
		std::vector<ENTITY_ID> vecs;
		
		Entities<Entity>::ENTITYS_MAP::iterator iter = entities.begin();
		for(; iter != entities.end(); ++iter)
		{
			//Entity* pEntity = static_cast<Entity*>(iter->second.get());
			//if(pEntity->baseEntityCall() != NULL && 
			//	pEntity->pScriptModule()->isPersistent())
			{
				vecs.push_back(static_cast<Entity*>(iter->second.get())->id());

				if(--count == 0)
					break;
			}
		}

		std::vector<ENTITY_ID>::iterator iter1 = vecs.begin();
		for(; iter1 != vecs.end(); ++iter1)
		{
			Entity* e = this->findEntity((*iter1));
			if(!e)
				continue;
			
			this->destroyEntity((*iter1), true);
		}
	}

	// 如果count等于perSecsDestroyEntitySize说明上面已经没有可处理的东西了
	// 剩下的应该都是space，可以开始销毁了
	if(count == g_serverConfig.getCellApp().perSecsDestroyEntitySize)
		Spaces::finalise();
}

//-------------------------------------------------------------------------------------		
bool Cellapp::initializeWatcher()
{
	ProfileVal::setWarningPeriod(stampsPerSecond() / g_kbeSrvConfig.gameUpdateHertz());
	// Publish zero-sample latency windows before the first script, timer, or client update.
	// 在首次脚本、Timer 或客户端更新前发布零样本延迟窗口，避免空载场景缺少指标目录。
	SCRIPTCALL_PROFILE.initializeWatcher();
	ONTIMER_PROFILE.initializeWatcher();
	CLIENT_UPDATE_PROFILE.initializeWatcher();

	WATCH_OBJECT("load", this, &Cellapp::_getLoad);
	WATCH_OBJECT("spaceSize", this, &Cellapp::spaceSize);
	WATCH_OBJECT("stats/runningTime", &runningTime);
	WATCH_OBJECT("witness/active", &Witness::activeCount);
	WATCH_OBJECT("witness/dirtyQueued", &Witness::dirtyQueuedCount);
	WATCH_OBJECT("witness/fullScans", &Witness::fullScanCount);
	WATCH_OBJECT("witness/fullScanEntities", &Witness::fullScanEntityCount);
	WATCH_OBJECT("witness/dirtyProcessed", &Witness::dirtyProcessedCount);
	WATCH_OBJECT("witness/maxQueueDepth", &Witness::maxQueueDepth);
	WATCH_OBJECT("witness/viewEntities", &Witness::viewEntityCount);
	WATCH_OBJECT("witness/maxViewEntities", &Witness::maxViewEntityCount);
	WATCH_OBJECT("witness/dirtyEnqueued", &Witness::dirtyEnqueuedCount);
	WATCH_OBJECT("witness/dirtyRequeues", &Witness::dirtyRequeueCount);
	WATCH_OBJECT("witness/staleDiscards", &Witness::staleDiscardCount);
	WATCH_OBJECT("witness/stateSkips", &Witness::stateSkipCount);
	WATCH_OBJECT("witness/volatileBytesSent", &Witness::volatileBytesSentCount);
	WATCH_OBJECT("witness/volatileBudgetDeferred", &Witness::volatileBudgetDeferredCount);
	WATCH_OBJECT("witness/volatileBudgetExhaustions", &Witness::volatileBudgetExhaustionCount);
	WATCH_OBJECT("witness/sendBytes", &Witness::sendBytesCount);
	WATCH_OBJECT("witness/sendBudgetExhaustions", &Witness::sendBudgetExhaustionCount);
	WATCH_OBJECT("witness/structuralProcessed", &Witness::structuralProcessedCount);
	WATCH_OBJECT("witness/queues/structuralQueued", &Witness::structuralQueuedCount);
	WATCH_OBJECT("witness/queues/volatileQueued", &Witness::volatileQueuedCount);
	WATCH_OBJECT("witness/queues/structuralEnqueued", &Witness::structuralEnqueuedCount);
	WATCH_OBJECT("witness/queues/volatileEnqueued", &Witness::volatileEnqueuedCount);
	WATCH_OBJECT("witness/queues/deduplicated", &Witness::queueDeduplicatedCount);
	WATCH_OBJECT("witness/queues/producerCoalesced", &Witness::producerCoalescedCount);
	WATCH_OBJECT("witness/queues/structuralPromotions", &Witness::structuralPromotionCount);
	WATCH_OBJECT("witness/queues/promotedVolatileSkips", &Witness::promotedVolatileSkipCount);
	WATCH_OBJECT("witness/queues/cancelledPendingLeaves", &Witness::cancelledPendingLeaveCount);
	WATCH_OBJECT("witness/scheduler/updateLimit", &Witness::globalUpdateLimit);
	WATCH_OBJECT("witness/scheduler/admitted", &Witness::globalAdmittedCount);
	WATCH_OBJECT("witness/scheduler/deferred", &Witness::globalDeferredCount);
	WATCH_OBJECT("witness/messages/enterCount", &Witness::enterUpdateCount);
	WATCH_OBJECT("witness/messages/enterBytes", &Witness::enterBytesCount);
	WATCH_OBJECT("witness/messages/leaveCount", &Witness::leaveUpdateCount);
	WATCH_OBJECT("witness/messages/leaveBytes", &Witness::leaveBytesCount);
	WATCH_OBJECT("witness/messages/volatileCount", &Witness::volatileUpdateCount);
	WATCH_OBJECT("witness/messages/volatileBytes", &Witness::volatileUpdateBytesCount);
	WATCH_OBJECT("witness/backpressure/activeSuppressed", &Witness::activeSuppressedCount);
	WATCH_OBJECT("witness/backpressure/suppressionTransitions", &Witness::suppressionTransitionCount);
	WATCH_OBJECT("witness/backpressure/resumeTransitions", &Witness::resumeTransitionCount);
	WATCH_OBJECT("witness/backpressure/staleControlDrops", this, &Cellapp::staleWitnessVolatileControlDrops);
	WATCH_OBJECT("witness/backpressure/suppressedUpdateSkips", &Witness::suppressedUpdateSkipCount);
	WATCH_OBJECT("witness/backpressure/structuralWhileSuppressed", &Witness::structuralWhileSuppressedCount);
	WATCH_OBJECT("witness/bundlesSent", &Witness::bundlesSentCount);
	WATCH_OBJECT("witness/maxBundleBytes", &Witness::maxBundleBytes);
	return EntityApp<Entity>::initializeWatcher() && WatchObjectPool::initWatchPools();
}

//-------------------------------------------------------------------------------------
bool Cellapp::installPyModules()
{
	Entity::installScript(getScript().getModule());
	SpaceEntity::installScript(getScript().getModule(), "Space");
	EntityComponent::installScript(getScript().getModule());
	GlobalDataClient::installScript(getScript().getModule());

	registerScript(Entity::getScriptType());
	registerScript(SpaceEntity::getScriptType());
	// 组件默认值会在 Cell 实体属性初始化期间直接构造，必须先注册组件基类并纳入类型校验。
	// Component defaults are constructed during cell-entity property initialization, so the component base must be registered and included in type validation first.
	registerScript(EntityComponent::getScriptType());
	
	// 将app标记注册到脚本
	std::map<uint32, std::string> flagsmaps = createAppFlagsMaps();
	std::map<uint32, std::string>::iterator fiter = flagsmaps.begin();
	for (; fiter != flagsmaps.end(); ++fiter)
	{
		if (PyModule_AddIntConstant(getScript().getModule(), fiter->second.c_str(), fiter->first))
		{
			ERROR_MSG(fmt::format("Cellapp::onInstallPyModules: Unable to set KBEngine.{}.\n", fiter->second));
		}
	}

	// 注册创建entity的方法到py
	APPEND_SCRIPT_MODULE_METHOD(getScript().getModule(),		time,							__py_gametime,											METH_VARARGS,			0);
	APPEND_SCRIPT_MODULE_METHOD(getScript().getModule(),		createEntity,					__py_createEntity,										METH_VARARGS,			0);
	APPEND_SCRIPT_MODULE_METHOD(getScript().getModule(), 		executeRawDatabaseCommand,		__py_executeRawDatabaseCommand,							METH_VARARGS,			0);
	APPEND_SCRIPT_MODULE_METHOD(getScript().getModule(),		reloadScript,					__py_reloadScript,										METH_VARARGS,			0);
	APPEND_SCRIPT_MODULE_METHOD(getScript().getModule(),		registerReadFileDescriptor,		PyFileDescriptor::__py_registerReadFileDescriptor,		METH_VARARGS,			0);
	APPEND_SCRIPT_MODULE_METHOD(getScript().getModule(),		registerWriteFileDescriptor,	PyFileDescriptor::__py_registerWriteFileDescriptor,		METH_VARARGS,			0);
	APPEND_SCRIPT_MODULE_METHOD(getScript().getModule(),		deregisterReadFileDescriptor,	PyFileDescriptor::__py_deregisterReadFileDescriptor,	METH_VARARGS,			0);
	APPEND_SCRIPT_MODULE_METHOD(getScript().getModule(),		deregisterWriteFileDescriptor,	PyFileDescriptor::__py_deregisterWriteFileDescriptor,	METH_VARARGS,			0);
	// CellApp 同时导出完成式接口，使服务器脚本模块与 Nex 2.8 的公共 API 保持一致。
	// CellApp exports completion APIs as well, keeping the server script module aligned with the Nex 2.8 public API.
	APPEND_SCRIPT_MODULE_METHOD(getScript().getModule(),		registerReadDataFileDescriptor,	PyFileDescriptor::__py_registerReadDataFileDescriptor,	METH_VARARGS,			0);
	APPEND_SCRIPT_MODULE_METHOD(getScript().getModule(),		deregisterReadDataFileDescriptor,	PyFileDescriptor::__py_deregisterReadDataFileDescriptor,	METH_VARARGS,			0);
	APPEND_SCRIPT_MODULE_METHOD(getScript().getModule(),		registerAcceptFileDescriptor,	PyFileDescriptor::__py_registerAcceptFileDescriptor,	METH_VARARGS,			0);
	APPEND_SCRIPT_MODULE_METHOD(getScript().getModule(),		deregisterAcceptFileDescriptor,	PyFileDescriptor::__py_deregisterAcceptFileDescriptor,	METH_VARARGS,			0);
	APPEND_SCRIPT_MODULE_METHOD(getScript().getModule(),		writeFileDescriptor,			PyFileDescriptor::__py_writeFileDescriptor,			METH_VARARGS,			0);
	APPEND_SCRIPT_MODULE_METHOD(getScript().getModule(),		addSpaceGeometryMapping,		Space::__py_AddSpaceGeometryMapping,					METH_VARARGS,			0);
	APPEND_SCRIPT_MODULE_METHOD(getScript().getModule(),		getSpaceGeometryMapping,		Space::__py_GetSpaceGeometryMapping,					METH_VARARGS,			0);
	APPEND_SCRIPT_MODULE_METHOD(getScript().getModule(),		setSpaceData,					Space::__py_SetSpaceData,								METH_VARARGS,			0);
	APPEND_SCRIPT_MODULE_METHOD(getScript().getModule(),		getSpaceData,					Space::__py_GetSpaceData,								METH_VARARGS,			0);
	APPEND_SCRIPT_MODULE_METHOD(getScript().getModule(),		delSpaceData,					Space::__py_DelSpaceData,								METH_VARARGS,			0);
	APPEND_SCRIPT_MODULE_METHOD(getScript().getModule(),		spaces,						Spaces::__py_Spaces,									METH_VARARGS,			0);
	APPEND_SCRIPT_MODULE_METHOD(getScript().getModule(),		entitiesForSpace,				Spaces::__py_EntitiesForSpace,							METH_VARARGS,			0);
	APPEND_SCRIPT_MODULE_METHOD(getScript().getModule(),		isShuttingDown,					__py_isShuttingDown,									METH_VARARGS,			0);
	APPEND_SCRIPT_MODULE_METHOD(getScript().getModule(),		address,						__py_address,											METH_VARARGS,			0);
	APPEND_SCRIPT_MODULE_METHOD(getScript().getModule(),		raycast,						__py_raycast,											METH_VARARGS,			0);
	APPEND_SCRIPT_MODULE_METHOD(getScript().getModule(), 		setAppFlags,					__py_setFlags,											METH_VARARGS,			0);
	APPEND_SCRIPT_MODULE_METHOD(getScript().getModule(), 		getAppFlags,					__py_getFlags,											METH_VARARGS,			0);
	
	return EntityApp<Entity>::installPyModules();
}

//-------------------------------------------------------------------------------------
void Cellapp::onInstallPyModules()
{
	// 添加globalData, cellAppData支持
	pCellAppData_ = new GlobalDataClient(DBMGR_TYPE, GlobalDataServer::CELLAPP_DATA);
	registerPyObjectToScript("cellAppData", pCellAppData_);
}

//-------------------------------------------------------------------------------------
bool Cellapp::uninstallPyModules()
{
	if(g_kbeSrvConfig.getCellApp().profiles.open_pyprofile)
	{
		script::PyProfile::stop("kbengine");

		char buf[MAX_BUF];
		kbe_snprintf(buf, MAX_BUF, "cellapp%u.pyprofile", startGroupOrder_);
		script::PyProfile::dump("kbengine", buf);
		script::PyProfile::remove("kbengine");
	}

	unregisterPyObjectToScript("cellAppData");
	S_RELEASE(pCellAppData_); 

	Entity::uninstallScript();
	SpaceEntity::uninstallScript();
	EntityComponent::uninstallScript();
	GlobalDataClient::uninstallScript();
	return EntityApp<Entity>::uninstallPyModules();
}

// 空间脚本必须构造专用实体子类，才能让 Python 的继承关系和 C++ 对象类型保持一致。
// Space scripts must construct the dedicated entity subtype so Python inheritance and C++ object layout stay aligned.
Entity* Cellapp::onCreateEntity(PyObject* pyEntity, ScriptDefModule* sm, ENTITY_ID eid)
{
	if (PyType_IsSubtype(sm->getScriptType(), SpaceEntity::getScriptType()))
		return new(pyEntity) SpaceEntity(eid, sm);

	return EntityApp<Entity>::onCreateEntity(pyEntity, sm, eid);
}

//-------------------------------------------------------------------------------------
PyObject* Cellapp::__py_gametime(PyObject* self, PyObject* args)
{
	return PyLong_FromUnsignedLong(Cellapp::getSingleton().time());
}

//-------------------------------------------------------------------------------------
bool Cellapp::run()
{
	return EntityApp<Entity>::run();
}

//-------------------------------------------------------------------------------------
void Cellapp::handleTimeout(TimerHandle handle, void * arg)
{
	switch (reinterpret_cast<uintptr>(arg))
	{
		case TIMEOUT_LOADING_TICK:
			break;
		default:
			break;
	}

	EntityApp<Entity>::handleTimeout(handle, arg);
}

//-------------------------------------------------------------------------------------
void Cellapp::handleGameTick()
{
	AUTO_SCOPED_PROFILE_LATENCY("gameTick");

	// 一定要在最前面
	updateLoad();

	EntityApp<Entity>::handleGameTick();

	Witness::beginUpdateTick();
	updatables_.update();
	Spaces::update();
}

//-------------------------------------------------------------------------------------
bool Cellapp::initializeBegin()
{
	return true;
}

//-------------------------------------------------------------------------------------
bool Cellapp::initializeEnd()
{
	// 如果需要pyprofile则在此处安装
	// 结束时卸载并输出结果
	if(g_kbeSrvConfig.getCellApp().profiles.open_pyprofile)
	{
		script::PyProfile::start("kbengine");
	}

	pWitnessedTimeoutHandler_ = new WitnessedTimeoutHandler();

	// 是否管理Y轴
	CoordinateSystem::hasY = g_kbeSrvConfig.getCellApp().coordinateSystem_hasY;

	dispatcher_.clearSpareTime();

	pGhostManager_ = new GhostManager();

	pTelnetServer_ = new TelnetServer(&this->dispatcher(), &this->networkInterface());
	pTelnetServer_->pScript(&this->getScript());

	bool ret = pTelnetServer_->start(g_kbeSrvConfig.getCellApp().telnet_passwd, 
		g_kbeSrvConfig.getCellApp().telnet_deflayer, 
		g_kbeSrvConfig.getCellApp().telnet_port);

	Components::getSingleton().extraData4(pTelnetServer_->port());
	return ret;
}

//-------------------------------------------------------------------------------------
void Cellapp::finalise()
{
	spaceViewers_.finalise();

	SAFE_RELEASE(pGhostManager_);
	SAFE_RELEASE(pWitnessedTimeoutHandler_);

	if(pTelnetServer_)
	{
		pTelnetServer_->stop();
		SAFE_RELEASE(pTelnetServer_);
	}

	Spaces::finalise();
	Navigation::getSingleton().finalise();
	forward_messagebuffer_.clear();
	updatables_.clear();

	destroyObjPool();
	EntityApp<Entity>::finalise();
}

//-------------------------------------------------------------------------------------
void Cellapp::destroyObjPool()
{
	EntityRef::destroyObjPool();
	Witness::destroyObjPool();
}

//-------------------------------------------------------------------------------------
void Cellapp::onGetEntityAppFromDbmgr(Network::Channel* pChannel, int32 uid, std::string& username,
						COMPONENT_TYPE componentType, COMPONENT_ID componentID, COMPONENT_ORDER globalorderID, COMPONENT_ORDER grouporderID,
						uint32 intaddr, uint16 intport, uint32 extaddr, uint16 extport, std::string& extaddrEx)
{
	if (!Components::getSingleton().isExpectedComponentChannel(DBMGR_TYPE, pChannel))
	{
		WARNING_MSG(fmt::format("Cellapp::onGetEntityAppFromDbmgr: rejected non-DBMgr source, addr={}.\n",
			pChannel != NULL ? pChannel->c_str() : "none"));
		return;
	}

	if ((componentType != BASEAPP_TYPE && componentType != CELLAPP_TYPE) || componentID == 0)
	{
		WARNING_MSG(fmt::format("Cellapp::onGetEntityAppFromDbmgr: rejected componentType={}, componentID={}, addr={}.\n",
			componentType, componentID, pChannel->c_str()));
		return;
	}

	Components::ComponentInfos* cinfos = Components::getSingleton().findComponent((
		KBEngine::COMPONENT_TYPE)componentType, uid, componentID);

	if(cinfos)
	{
		if(cinfos->pIntAddr->ip != intaddr || cinfos->pIntAddr->port != intport)
		{
			ERROR_MSG(fmt::format("Cellapp::onGetEntityAppFromDbmgr: Illegal app(uid:{0}, username:{1}, componentType:{2}, "
					"componentID:{3}, globalorderID={9}, grouporderID={10}, intaddr:{4}, intport:{5}, extaddr:{6}, extport:{7},  from {8})\n",
					uid, 
					username,
					COMPONENT_NAME_EX((COMPONENT_TYPE)componentType), 
					componentID,
					inet_ntoa((struct in_addr&)intaddr),
					ntohs(intport),
					(extaddr != 0 ? inet_ntoa((struct in_addr&)extaddr) : "nonsupport"),
					ntohs(extport),
					pChannel->c_str(),
					((int32)globalorderID), 
					((int32)grouporderID)));

			Network::Bundle* pBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
			(*pBundle).newMessage(DbmgrInterface::reqKillServer);
			(*pBundle) << g_componentID << g_componentType << KBEngine::getUsername() << KBEngine::getUserUID() << "Duplicate app-id.";
			pChannel->send(pBundle);
		}
	}

	EntityApp<Entity>::onRegisterNewApp(pChannel, uid, username, componentType, componentID, globalorderID, grouporderID,
									intaddr, intport, extaddr, extport, extaddrEx);

	KBEngine::COMPONENT_TYPE tcomponentType = (KBEngine::COMPONENT_TYPE)componentType;

	cinfos = Components::getSingleton().findComponent(tcomponentType, uid, componentID);
	
	if (cinfos == NULL)
	{
		ERROR_MSG(fmt::format("Cellapp::onGetEntityAppFromDbmgr: Illegal app(uid:{0}, username:{1}, componentType:{2}, "
				"componentID:{3}, globalorderID={9}, grouporderID={10}, intaddr:{4}, intport:{5}, extaddr:{6}, extport:{7},  from {8})\n",
				uid, 
				username,
				COMPONENT_NAME_EX((COMPONENT_TYPE)componentType), 
				componentID,
				inet_ntoa((struct in_addr&)intaddr),
				ntohs(intport),
				(extaddr != 0 ? inet_ntoa((struct in_addr&)extaddr) : "nonsupport"),
				ntohs(extport),
				pChannel->c_str(),
				((int32)globalorderID), 
				((int32)grouporderID)));

		return;
	}
	
	cinfos->pChannel = NULL;

	int ret = Components::getSingleton().connectComponent(tcomponentType, uid, componentID);

	if (ret == -1)
	{
		if (!pInitProgressHandler_)
			pInitProgressHandler_ = new InitProgressHandler(this->networkInterface());

		pInitProgressHandler_->updateInfos(componentID_, startGlobalOrder_, startGroupOrder_);

		InitProgressHandler::PendingConnectEntityApp appInfos;
		appInfos.componentID = componentID;
		appInfos.componentType = tcomponentType;
		appInfos.uid = uid;
		appInfos.count = 0;
		pInitProgressHandler_->addPendingConnectEntityApps(appInfos);

		ERROR_MSG(fmt::format("Cellapp::onGetEntityAppFromDbmgr: Add to the pending list and try connecting later! uid:{}, componentType:{}, componentID:{}\n",
			uid,
			COMPONENT_NAME_EX((COMPONENT_TYPE)tcomponentType),
			componentID));

		return;
	}

	Network::Bundle* pBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);

	switch(tcomponentType)
	{
	case BASEAPP_TYPE:
		(*pBundle).newMessage(BaseappInterface::onRegisterNewApp);
		BaseappInterface::onRegisterNewAppArgs11::staticAddToBundle((*pBundle), getUserUID(), getUsername(), 
			CELLAPP_TYPE, componentID_, startGlobalOrder_, startGroupOrder_,
			this->networkInterface().intaddr().ip, this->networkInterface().intaddr().port, 
			this->networkInterface().extaddr().ip, this->networkInterface().extaddr().port, g_kbeSrvConfig.getConfig().externalAddress);
		break;
	case CELLAPP_TYPE:
		(*pBundle).newMessage(CellappInterface::onRegisterNewApp);
		CellappInterface::onRegisterNewAppArgs11::staticAddToBundle((*pBundle), getUserUID(), getUsername(), 
			CELLAPP_TYPE, componentID_, startGlobalOrder_, startGroupOrder_,
			this->networkInterface().intaddr().ip, this->networkInterface().intaddr().port, 
			this->networkInterface().extaddr().ip, this->networkInterface().extaddr().port, g_kbeSrvConfig.getConfig().externalAddress);
		break;
	default:
		Network::Bundle::reclaimPoolObject(pBundle);
		return;
	};
	
	cinfos->pChannel->send(pBundle);
}

//-------------------------------------------------------------------------------------
void Cellapp::onUpdateLoad()
{
	Network::Channel* pChannel = Components::getSingleton().getCellappmgrChannel();
	if(pChannel != NULL)
	{
		Network::Bundle* pBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
		(*pBundle).newMessage(CellappmgrInterface::updateCellapp);
		CellappmgrInterface::updateCellappArgs4::staticAddToBundle((*pBundle), 
			componentID_, (ENTITY_ID)pEntities_->getEntities().size(), getLoad(), flags_);

		pChannel->send(pBundle);
	}
}

//-------------------------------------------------------------------------------------
PyObject* Cellapp::__py_createEntity(PyObject* self, PyObject* args)
{
	PyObject* params = NULL;
	char* entityType = NULL;
	SPACE_ID spaceID;
	PyObject* position, *direction;
	
	if(!PyArg_ParseTuple(args, "s|I|O|O|O", &entityType, &spaceID, &position, &direction, &params))
	{
		PyErr_Format(PyExc_TypeError, 
			"KBEngine::createEntity: args error! args[scriptName, spaceID, position, direction, states].");
		PyErr_PrintEx(0);
		return 0;
	}
	
	if(entityType == NULL || strlen(entityType) == 0)
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::createEntity: entityType is NULL.");
		PyErr_PrintEx(0);
		return 0;
	}

	Space* space = Spaces::findSpace(spaceID);
	if(space == NULL || !space->isGood())
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::createEntity: spaceID %ld not found.", spaceID);
		PyErr_PrintEx(0);
		return 0;
	}
	
	if(Cellapp::getSingleton().isShuttingdown())
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::createEntity: shutting down! entityType=%s", entityType);
		PyErr_PrintEx(0);
		return 0;
	}
	
	// 创建entity
	Entity* pEntity = Cellapp::getSingleton().createEntity(entityType, params, false, 0);

	if(pEntity != NULL)
	{
		Py_INCREF(pEntity);
		pEntity->spaceID(space->id());
		pEntity->createNamespace(params);
		pEntity->pySetPosition(position);
		pEntity->pySetDirection(direction);	
		pEntity->initializeScript();

		// 添加到space
		space->addEntityAndEnterWorld(pEntity);

		// 有可能在addEntityAndEnterWorld中被销毁了
		// 这里需要让实体返回给脚本，只不过实体为isDestroyed = true状态
		//if(pEntity->isDestroyed())
		//{
		//	Py_DECREF(pEntity);
		//	return NULL;
		//}
	}

	//Py_XDECREF(params);
	return pEntity;
}

//-------------------------------------------------------------------------------------
PyObject* Cellapp::__py_executeRawDatabaseCommand(PyObject* self, PyObject* args)
{
	int argCount = (int)PyTuple_Size(args);
	PyObject* pycallback = NULL;
	PyObject* pyDBInterfaceName = NULL;
	int ret = 0;
	ENTITY_ID eid = -1;

	char* data = NULL;
	Py_ssize_t size;
	
	if (argCount == 4)
		ret = PyArg_ParseTuple(args, "s#|O|i|O", &data, &size, &pycallback, &eid, &pyDBInterfaceName);
	else if (argCount == 3)
		ret = PyArg_ParseTuple(args, "s#|O|i", &data, &size, &pycallback, &eid);
	else if(argCount == 2)
		ret = PyArg_ParseTuple(args, "s#|O", &data, &size, &pycallback);
	else if(argCount == 1)
		ret = PyArg_ParseTuple(args, "s#", &data, &size);

	if(!ret)
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::executeRawDatabaseCommand: args error!");
		PyErr_PrintEx(0);
		S_Return;
	}
	
	std::string dbInterfaceName = "default";
	if (pyDBInterfaceName)
	{
		dbInterfaceName = PyUnicode_AsUTF8AndSize(pyDBInterfaceName, NULL);
		
		if (!g_kbeSrvConfig.dbInterface(dbInterfaceName))
		{
			PyErr_Format(PyExc_TypeError, "KBEngine::executeRawDatabaseCommand: args4, incorrect dbInterfaceName(%s)!", 
				dbInterfaceName.c_str());
			
			PyErr_PrintEx(0);
			S_Return;
		}
	}

	Cellapp::getSingleton().executeRawDatabaseCommand(data, (uint32)size, pycallback, eid, dbInterfaceName);
	S_Return;
}

//-------------------------------------------------------------------------------------
void Cellapp::executeRawDatabaseCommand(const char* datas, uint32 size, PyObject* pycallback, ENTITY_ID eid, const std::string& dbInterfaceName)
{
	if(datas == NULL)
	{
		ERROR_MSG("KBEngine::executeRawDatabaseCommand: execute error!\n");
		return;
	}

	Components::COMPONENTS& cts = Components::getSingleton().getComponents(DBMGR_TYPE);
	Components::ComponentInfos* dbmgrinfos = NULL;

	if(cts.size() > 0)
		dbmgrinfos = &(*cts.begin());

	if(dbmgrinfos == NULL || dbmgrinfos->pChannel == NULL || dbmgrinfos->cid == 0)
	{
		ERROR_MSG("KBEngine::executeRawDatabaseCommand: not found dbmgr!\n");
		return;
	}

	int dbInterfaceIndex = g_kbeSrvConfig.dbInterfaceName2dbInterfaceIndex(dbInterfaceName);
	if (dbInterfaceIndex < 0)
	{
		ERROR_MSG(fmt::format("KBEngine::executeRawDatabaseCommand: not found dbInterface({})!\n",
			dbInterfaceName));

		return;
	}

	//INFO_MSG(fmt::format("KBEngine::executeRawDatabaseCommand{}:{}.\n", (eid > 0 ? fmt::format("(entityID={})", eid) : ""), datas));

	Network::Bundle* pBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
	(*pBundle).newMessage(DbmgrInterface::executeRawDatabaseCommand);
	(*pBundle) << eid;
	(*pBundle) << (uint16)dbInterfaceIndex;
	(*pBundle) << componentID_ << componentType_;

	CALLBACK_ID callbackID = 0;

	if(pycallback && PyCallable_Check(pycallback))
		callbackID = callbackMgr().save(pycallback);

	(*pBundle) << callbackID;
	(*pBundle) << size;
	(*pBundle).append(datas, size);
	dbmgrinfos->pChannel->send(pBundle);
}

//-------------------------------------------------------------------------------------
void Cellapp::onExecuteRawDatabaseCommandCB(Network::Channel* pChannel, KBEngine::MemoryStream& s)
{
	if (!Components::getSingleton().isExpectedComponentChannel(DBMGR_TYPE, pChannel))
	{
		WARNING_MSG(fmt::format("Cellapp::onExecuteRawDatabaseCommandCB: rejected non-DBMgr source, addr={}.\n",
			pChannel->c_str()));
		s.done();
		return;
	}

	std::string err;
	CALLBACK_ID callbackID = 0;
	uint32 nrows = 0;
	uint32 nfields = 0;
	uint64 affectedRows = 0;
	uint64 lastInsertID = 0;

	PyObject* pResultSet = NULL;
	PyObject* pAffectedRows = NULL;
	PyObject* pLastInsertID = NULL;
	PyObject* pErrorMsg = NULL;

	s >> callbackID;
	s >> err;

	if(err.size() <= 0)
	{
		s >> nfields;

		pErrorMsg = Py_None;
		Py_INCREF(pErrorMsg);

		if(nfields > 0)
		{
			pAffectedRows = Py_None;
			Py_INCREF(pAffectedRows);

			pLastInsertID = Py_None;
			Py_INCREF(pLastInsertID);

			s >> nrows;

			pResultSet = PyList_New(nrows);
			for (uint32 i = 0; i < nrows; ++i)
			{
				PyObject* pRow = PyList_New(nfields);
				for(uint32 j = 0; j < nfields; ++j)
				{
					std::string cell;
					s.readBlob(cell);

					PyObject* pCell = NULL;
						
					if(cell == "KBE_QUERY_DB_NULL")
					{
						Py_INCREF(Py_None);
						pCell = Py_None;
					}
					else
					{
						pCell = PyBytes_FromStringAndSize(cell.data(), cell.length());
					}

					PyList_SET_ITEM(pRow, j, pCell);
				}

				PyList_SET_ITEM(pResultSet, i, pRow);
			}
		}
		else
		{
			pResultSet = Py_None;
			Py_INCREF(pResultSet);

			pErrorMsg = Py_None;
			Py_INCREF(pErrorMsg);

			s >> affectedRows;

			pAffectedRows = PyLong_FromUnsignedLongLong(affectedRows);

			s >> lastInsertID;
			pLastInsertID = PyLong_FromUnsignedLongLong(lastInsertID);
		}
	}
	else
	{
			pResultSet = Py_None;
			Py_INCREF(pResultSet);

			pErrorMsg = PyUnicode_FromString(err.c_str());

			pAffectedRows = Py_None;
			Py_INCREF(pAffectedRows);

			pLastInsertID = Py_None;
			Py_INCREF(pLastInsertID);
	}

	s.done();

	//DEBUG_MSG(fmt::format("Cellapp::onExecuteRawDatabaseCommandCB: nrows={}, nfields={}, err={}.\n", 
	//	nrows, nfields, err.c_str()));

	if(callbackID > 0)
	{
		SCOPED_PROFILE(SCRIPTCALL_PROFILE);

		PyObjectPtr pyfunc = pyCallbackMgr_.take(callbackID);
		if(pyfunc != NULL)
		{
			PyObject* pyResult = PyObject_CallFunction(pyfunc.get(), 
												const_cast<char*>("OOOO"), 
												pResultSet, pAffectedRows, pLastInsertID, pErrorMsg);

			if(pyResult != NULL)
				Py_DECREF(pyResult);
			else
				SCRIPT_ERROR_CHECK();
		}
		else
		{
			ERROR_MSG(fmt::format("Cellapp::onExecuteRawDatabaseCommandCB: not found callback:{}.\n",
				callbackID));
		}
	}

	Py_XDECREF(pResultSet);
	Py_XDECREF(pAffectedRows);
	Py_XDECREF(pLastInsertID);
	Py_XDECREF(pErrorMsg);
}

//-------------------------------------------------------------------------------------
void Cellapp::reqBackupEntityCellData(Network::Channel* pChannel, KBEngine::MemoryStream& s)
{
	ENTITY_ID entityID = 0;
	s >> entityID;

	Entity* e = this->findEntity(entityID);
	if(!e)
	{
		WARNING_MSG(fmt::format("Cellapp::reqBackupEntityCellData: not found entity {}.\n", entityID));
		return;
	}

	e->backupCellData();
}

//-------------------------------------------------------------------------------------
void Cellapp::reqWriteToDBFromBaseapp(Network::Channel* pChannel, KBEngine::MemoryStream& s)
{
	ENTITY_ID entityID = 0;
	CALLBACK_ID callbackID = 0;
	int8 shouldAutoLoad = -1;

	s >> entityID;
	s >> callbackID;
	s >> shouldAutoLoad;

	Entity* e = this->findEntity(entityID);
	if(!e)
	{
		WARNING_MSG(fmt::format("Cellapp::reqWriteToDBFromBaseapp: not found entity {}.\n", entityID));
		return;
	}

	e->writeToDB(&callbackID, &shouldAutoLoad, NULL);
}

//-------------------------------------------------------------------------------------
void Cellapp::onDbmgrInitCompleted(Network::Channel* pChannel,
		GAME_TIME gametime, ENTITY_ID startID, ENTITY_ID endID, COMPONENT_ORDER
		startGlobalOrder, COMPONENT_ORDER startGroupOrder, const std::string& digest)
{
	if (!Components::getSingleton().isExpectedComponentChannel(DBMGR_TYPE, pChannel))
	{
		WARNING_MSG(fmt::format("Cellapp::onDbmgrInitCompleted: rejected non-DBMgr source, addr={}.\n",
			pChannel->c_str()));
		return;
	}

	EntityApp<Entity>::onDbmgrInitCompleted(pChannel, gametime, startID, endID, startGlobalOrder, startGroupOrder, digest);
	
	// 再次同步自己的新信息(startGlobalOrder, startGroupOrder等)到machine
	Components::getSingleton().broadcastSelf();

	// 这里需要更新一下python的环境变量
	this->getScript().setenv("KBE_BOOTIDX_GLOBAL", getenv("KBE_BOOTIDX_GLOBAL"));
	this->getScript().setenv("KBE_BOOTIDX_GROUP", getenv("KBE_BOOTIDX_GROUP"));

	if (!pInitProgressHandler_)
		pInitProgressHandler_ = new InitProgressHandler(this->networkInterface());

	pInitProgressHandler_->start();
}

//-------------------------------------------------------------------------------------
void Cellapp::onBroadcastCellAppDataChanged(Network::Channel* pChannel, KBEngine::MemoryStream& s)
{
	if (!Components::getSingleton().isExpectedComponentChannel(DBMGR_TYPE, pChannel))
	{
		WARNING_MSG(fmt::format("Cellapp::onBroadcastCellAppDataChanged: rejected non-DBMgr source, addr={}.\n",
			pChannel->c_str()));
		s.done();
		return;
	}

	std::string key, value;
	bool isDelete;
	
	s >> isDelete;
	s.readBlob(key);

	if(!isDelete)
	{
		s.readBlob(value);
	}

	PyObject * pyKey = script::Pickler::unpickle(key);
	if(pyKey == NULL)
	{
		ERROR_MSG("Cellapp::onBroadcastCellAppDataChanged: no has key!\n");
		return;
	}

	if(isDelete)
	{
		if(pCellAppData_->del(pyKey))
		{
			SCOPED_PROFILE(SCRIPTCALL_PROFILE);

			// 通知脚本
			SCRIPT_OBJECT_CALL_ARGS1(getEntryScript().get(), const_cast<char*>("onCellAppDataDel"), 
				const_cast<char*>("O"), pyKey, false);
		}
	}
	else
	{
		PyObject * pyValue = script::Pickler::unpickle(value);

		if(pyValue == NULL)
		{
			ERROR_MSG("Cellapp::onBroadcastCellAppDataChanged: no has value!\n");
			Py_DECREF(pyKey);
			return;
		}

		if(pCellAppData_->write(pyKey, pyValue))
		{
			SCOPED_PROFILE(SCRIPTCALL_PROFILE);

			// 通知脚本
			SCRIPT_OBJECT_CALL_ARGS2(getEntryScript().get(), const_cast<char*>("onCellAppData"), 
				const_cast<char*>("OO"), pyKey, pyValue, false);
		}

		Py_DECREF(pyValue);
	}

	Py_DECREF(pyKey);
}

//-------------------------------------------------------------------------------------
void Cellapp::onCreateCellEntityInNewSpaceFromBaseapp(Network::Channel* pChannel, KBEngine::MemoryStream& s)
{
	if (!validateSpaceCreationForward(s))
	{
		WARNING_MSG("Cellapp::onCreateCellEntityInNewSpaceFromBaseapp: rejected malformed or oversized payload.\n");
		s.done();
		return;
	}

	std::string entityType;
	ENTITY_ID entitycallEntityID;
	COMPONENT_ID componentID;
	SPACE_ID spaceID = 1;
	bool hasClient;

	s >> entityType;
	s >> entitycallEntityID;
	s >> spaceID;
	s >> componentID;
	s >> hasClient;

	Components::ComponentInfos* cinfos = validateBaseappEntityCreationSource(
		pChannel, componentID, entityType, hasClient, true,
		"Cellapp::onCreateCellEntityInNewSpaceFromBaseapp");
	if (cinfos == NULL)
	{
		s.done();
		return;
	}

	// DEBUG_MSG("Cellapp::onCreateCellEntityInNewSpaceFromBaseapp: spaceID=%u, entityType=%s, entityID=%d, componentID=%"PRAppID".\n", 
	//	spaceID, entityType.c_str(), entitycallEntityID, componentID);

	Space* space = Spaces::createNewSpace(spaceID, entityType);
	if(space != NULL)
	{
		// 创建entity
		Entity* e = createEntity(entityType.c_str(), NULL, false, entitycallEntityID, false);
		
		if(e == NULL)
		{
			s.done();

			ERROR_MSG("Cellapp::onCreateCellEntityInNewSpaceFromBaseapp: createEntity error!\n");

			/* 目前来说除非内存或者系统问题，否则不会出现这个错误
			Network::Bundle* pBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
			pBundle->newMessage(BaseappInterface::onCreateCellFailure);
			BaseappInterface::onCreateCellFailureArgs1::staticAddToBundle(*pBundle, entitycallEntityID);
			cinfos->pChannel->send(pBundle);
			*/
			return;
		}

		// createCellEntityInNewSpace 允许普通 Entity 脚本创建空间，因此不能只依赖 Python 子类型识别空间实体。
		// createCellEntityInNewSpace allows regular Entity scripts to create spaces, so space ownership cannot rely only on the Python subtype.
		e->isSpace(true);

		PyObject* cellData = e->createCellDataFromStream(&s);

		// 设置entity的baseEntityCall
		EntityCall* entitycall = new EntityCall(e->pScriptModule(), NULL, componentID, entitycallEntityID, ENTITYCALL_TYPE_BASE);
		e->baseEntityCall(entitycall);
		
		if (hasClient)
		{
			if (!bindClientStateForCreatedEntity(e,
				"Cellapp::onCreateCellEntityInNewSpaceFromBaseapp"))
			{
				Py_XDECREF(cellData);
				destroyEntity(e->id(), false);
				Spaces::destroySpace(spaceID, entitycallEntityID);
				s.done();
				return;
			}
		}

		// 此处baseapp可能还有没初始化过来， 所以有一定概率是为None的
		if(cinfos->pChannel == NULL || cinfos->pChannel->isDestroyed())
		{
			Network::Bundle* pBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
			ForwardItem* pFI = new ForwardItem();
			pFI->pHandler = new FMH_Baseapp_onEntityGetCellFrom_onCreateCellEntityInNewSpaceFromBaseapp(e, spaceID, cellData);
			//Py_XDECREF(cellData);
			pFI->pBundle = pBundle;
			(*pBundle).newMessage(BaseappInterface::onEntityGetCell);
			BaseappInterface::onEntityGetCellArgs3::staticAddToBundle((*pBundle), entitycallEntityID, componentID_, spaceID);
			forward_messagebuffer_.push(componentID, pFI);
			
			WARNING_MSG(fmt::format("Cellapp::onCreateCellEntityInNewSpaceFromBaseapp: not found baseapp({}), message is buffered.\n",
				componentID));
			
			return;
		}

		KBE_SHA1 sha;
		uint32 digest[5];
		sha.Input(s.data(), s.length());
		sha.Result(digest);
		e->setDirty((uint32*)&digest[0]);

		space->addEntity(e);
		e->spaceID(space->id());
		e->initializeEntity(cellData, true);
		Py_XDECREF(cellData);

		// 添加到space
		space->addEntityToNode(e);

		if (hasClient)
		{
			e->onGetWitness();
		}
		else
		{
			space->onEnterWorld(e);
		}

		Network::Bundle* pBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
		(*pBundle).newMessage(BaseappInterface::onEntityGetCell);
		BaseappInterface::onEntityGetCellArgs3::staticAddToBundle((*pBundle), entitycallEntityID, componentID_, spaceID);
		cinfos->pChannel->send(pBundle);

		return;
	}
	
	ERROR_MSG(fmt::format("Cellapp::onCreateCellEntityInNewSpaceFromBaseapp: not found baseapp[{}], entityID={}, spaceID={}.\n",
		componentID, entitycallEntityID, spaceID));
}

//-------------------------------------------------------------------------------------
void Cellapp::onRestoreSpaceInCellFromBaseapp(Network::Channel* pChannel, KBEngine::MemoryStream& s)
{
	if (!validateSpaceCreationForward(s))
	{
		WARNING_MSG("Cellapp::onRestoreSpaceInCellFromBaseapp: rejected malformed or oversized payload.\n");
		s.done();
		return;
	}

	std::string entityType;
	ENTITY_ID entitycallEntityID;
	COMPONENT_ID componentID;
	SPACE_ID spaceID = 1;
	bool hasClient;

	s >> entityType;
	s >> entitycallEntityID;
	s >> spaceID;
	s >> componentID;
	s >> hasClient;

	Components::ComponentInfos* cinfos = validateBaseappEntityCreationSource(
		pChannel, componentID, entityType, hasClient, true,
		"Cellapp::onRestoreSpaceInCellFromBaseapp");
	if (cinfos == NULL)
	{
		s.done();
		return;
	}

	// DEBUG_MSG("Cellapp::onRestoreSpaceInCellFromBaseapp: spaceID=%u, entityType=%s, entityID=%d, componentID=%"PRAppID".\n", 
	//	spaceID, entityType.c_str(), entitycallEntityID, componentID);

	Space* space = Spaces::createNewSpace(spaceID, entityType);
	if(space != NULL)
	{
		// 创建entity
		Entity* e = createEntity(entityType.c_str(), NULL, false, entitycallEntityID, false);
		
		if(e == NULL)
		{
			s.done();
			return;
		}

		// 灾难恢复重建空间实体时必须恢复运行时标志，使 KBEngine.spaces() 与首次创建结果一致。
		// Disaster recovery must restore the runtime flag so KBEngine.spaces() matches the initial creation result.
		e->isSpace(true);

		PyObject* cellData = e->createCellDataFromStream(&s);

		// 设置entity的baseEntityCall
		EntityCall* entitycall = new EntityCall(e->pScriptModule(), NULL, componentID, entitycallEntityID, ENTITYCALL_TYPE_BASE);
		e->baseEntityCall(entitycall);
		
		if (hasClient)
		{
			if (!bindClientStateForCreatedEntity(e,
				"Cellapp::onRestoreSpaceInCellFromBaseapp"))
			{
				Py_XDECREF(cellData);
				destroyEntity(e->id(), false);
				Spaces::destroySpace(spaceID, entitycallEntityID);
				s.done();
				return;
			}
		}

		// 此处baseapp可能还有没初始化过来， 所以有一定概率是为None的
		if(cinfos->pChannel == NULL || cinfos->pChannel->isDestroyed())
		{
			Network::Bundle* pBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
			ForwardItem* pFI = new ForwardItem();
			pFI->pHandler = new FMH_Baseapp_onEntityGetCellFrom_onCreateCellEntityInNewSpaceFromBaseapp(e, spaceID, cellData);
			//Py_XDECREF(cellData);
			pFI->pBundle = pBundle;
			(*pBundle).newMessage(BaseappInterface::onEntityGetCell);
			BaseappInterface::onEntityGetCellArgs3::staticAddToBundle((*pBundle), entitycallEntityID, componentID_, spaceID);
			forward_messagebuffer_.push(componentID, pFI);
			
			WARNING_MSG(fmt::format("Cellapp::onRestoreSpaceInCellFromBaseapp: not found baseapp({}), message has been buffered.\n",
				componentID));
			
			return;
		}
		
		KBE_SHA1 sha;
		uint32 digest[5];
		sha.Input(s.data(), s.length());
		sha.Result(digest);
		e->setDirty((uint32*)&digest[0]);

		e->spaceID(space->id());
		e->createNamespace(cellData);
		Py_XDECREF(cellData);

		// 添加到space
		e->onRestore();

		space->addEntityAndEnterWorld(e, true);

		Network::Bundle* pBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
		(*pBundle).newMessage(BaseappInterface::onEntityGetCell);
		BaseappInterface::onEntityGetCellArgs3::staticAddToBundle((*pBundle), entitycallEntityID, componentID_, spaceID);
		cinfos->pChannel->send(pBundle);
		return;
	}
	
	ERROR_MSG(fmt::format("Cellapp::onRestoreSpaceInCellFromBaseapp: not found baseapp[{}], entityID={}, spaceID={}.\n",
		componentID, entitycallEntityID, spaceID));
}

//-------------------------------------------------------------------------------------
void Cellapp::requestRestore(Network::Channel* pChannel, KBEngine::MemoryStream& s)
{
	COMPONENT_ID cid;
	s >> cid;

	bool canRestore = idClient_.size() > 0;

	DEBUG_MSG(fmt::format("Cellapp::requestRestore: cid={}, canRestore={}, channel={}.\n", 
		cid, canRestore, pChannel->c_str()));

	Network::Bundle* pBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
	(*pBundle).newMessage(BaseappInterface::onRequestRestoreCB);
	(*pBundle) << g_componentID << cid << canRestore;
	pChannel->send(pBundle);
}

//-------------------------------------------------------------------------------------
void Cellapp::onCreateCellEntityFromBaseapp(Network::Channel* pChannel, KBEngine::MemoryStream& s)
{
	if (!validateEntityCreationForward(s))
	{
		WARNING_MSG("Cellapp::onCreateCellEntityFromBaseapp: rejected malformed or oversized payload.\n");
		s.done();
		return;
	}

	std::string entityType;
	ENTITY_ID createToEntityID, entityID;
	
	COMPONENT_ID componentID;
	SPACE_ID spaceID = 1;
	bool hasClient;
	bool inRescore = false;

	s >> createToEntityID;
	s >> entityType;
	s >> entityID;
	s >> componentID;
	s >> hasClient;
	s >> inRescore;

	Components::ComponentInfos* cinfos = validateBaseappEntityCreationSource(
		pChannel, componentID, entityType, hasClient, false,
		"Cellapp::onCreateCellEntityFromBaseapp");
	if (cinfos == NULL)
	{
		s.done();
		return;
	}

	// 此处baseapp可能还有没初始化过来， 所以有一定概率是为None的
	if(cinfos->pChannel == NULL || cinfos->pChannel->isDestroyed())
	{
		MemoryStream* pCellData = MemoryStream::createPoolObject(OBJECTPOOL_POINT);
		pCellData->append(s);

		Network::Bundle* pBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
		ForwardItem* pFI = new ForwardItem();
		pFI->pHandler = new FMH_Baseapp_onEntityGetCellFrom_onCreateCellEntityFromBaseapp(entityType, createToEntityID, 
			entityID, pCellData, hasClient, inRescore, componentID, spaceID);

		pFI->pBundle = pBundle;
		(*pBundle).newMessage(BaseappInterface::onEntityGetCell);
		BaseappInterface::onEntityGetCellArgs3::staticAddToBundle((*pBundle), entityID, componentID_, spaceID);
		forward_messagebuffer_.push(componentID, pFI);

		WARNING_MSG(fmt::format("Cellapp::onCreateCellEntityFromBaseapp: not found baseapp({}), message is buffered.\n",
			componentID));
			
		return;
	}

	_onCreateCellEntityFromBaseapp(entityType, createToEntityID, entityID, 
					&s, hasClient, inRescore, componentID, spaceID);

}

//-------------------------------------------------------------------------------------
void Cellapp::_onCreateCellEntityFromBaseapp(std::string& entityType, ENTITY_ID createToEntityID, ENTITY_ID entityID,
											MemoryStream* pCellData, bool hasClient, bool inRescore, COMPONENT_ID componentID, 
											SPACE_ID spaceID)
{
	// 注意：此处理论不会找不到组件， 因为onCreateCellEntityFromBaseapp中已经进行过一次消息缓存判断
	Components::ComponentInfos* cinfos = Components::getSingleton().findComponent(BASEAPP_TYPE, componentID);
	if (cinfos == NULL || cinfos->pChannel == NULL || cinfos->pChannel->isDestroyed())
	{
		WARNING_MSG(fmt::format("Cellapp::_onCreateCellEntityFromBaseapp: source BaseApp unavailable, componentID={}.\n",
			componentID));
		return;
	}

	Entity* pCreateToEntity = pEntities_->find(createToEntityID);

	// 可能spaceEntity已经销毁了， 但还未来得及通知到baseapp时
	// base部分在向这个space创建entity
	if(pCreateToEntity == NULL)
	{
		ERROR_MSG("Cellapp::_onCreateCellEntityFromBaseapp: not fount spaceEntity. may have been destroyed!\n");

		Network::Bundle* pBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
		pBundle->newMessage(BaseappInterface::onCreateCellFailure);
		BaseappInterface::onCreateCellFailureArgs1::staticAddToBundle(*pBundle, entityID);
		cinfos->pChannel->send(pBundle);
		return;
	}

	spaceID = pCreateToEntity->spaceID();

	//DEBUG_MSG("Cellapp::onCreateCellEntityFromBaseapp: spaceID=%u, entityType=%s, entityID=%d, componentID=%"PRAppID".\n", 
	//	spaceID, entityType.c_str(), entityID, componentID);

	Space* space = Spaces::findSpace(spaceID);
	if(space != NULL && space->isGood())
	{
		// 解包cellData信息.
		PyObject* cellData = NULL;
	
		// 创建entity
		Entity* e = createEntity(entityType.c_str(), cellData, false, entityID, false);
		
		if(e == NULL)
		{
			Py_XDECREF(cellData);
			Network::Bundle* pFailureBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
			pFailureBundle->newMessage(BaseappInterface::onCreateCellFailure);
			BaseappInterface::onCreateCellFailureArgs1::staticAddToBundle(*pFailureBundle, entityID);
			cinfos->pChannel->send(pFailureBundle);
			return;
		}

		// 设置entity的baseEntityCall
		EntityCall* entitycall = new EntityCall(e->pScriptModule(), NULL, componentID, entityID, ENTITYCALL_TYPE_BASE);
		e->baseEntityCall(entitycall);
		
		KBE_SHA1 sha;
		uint32 digest[5];
		sha.Input(pCellData->data(), pCellData->length());
		sha.Result(digest);
		e->setDirty((uint32*)&digest[0]);

		cellData = e->createCellDataFromStream(pCellData);
		e->createNamespace(cellData, true);

		if(hasClient)
		{
			if (!bindClientStateForCreatedEntity(e,
				"Cellapp::_onCreateCellEntityFromBaseapp"))
			{
				Py_XDECREF(cellData);
				destroyEntity(e->id(), false);
				Network::Bundle* pFailureBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
				pFailureBundle->newMessage(BaseappInterface::onCreateCellFailure);
				BaseappInterface::onCreateCellFailureArgs1::staticAddToBundle(*pFailureBundle, entityID);
				cinfos->pChannel->send(pFailureBundle);
				return;
			}
		}

		// Only acknowledge success after all client-facing state is valid.
		// 只有客户端相关状态全部有效后才向 BaseApp 确认创建成功。
		Network::Bundle* pBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
		pBundle->newMessage(BaseappInterface::onEntityGetCell);
		BaseappInterface::onEntityGetCellArgs3::staticAddToBundle(*pBundle, entityID, componentID_, spaceID);
		cinfos->pChannel->send(pBundle);

		space->addEntity(e);

		if(!inRescore)
		{
			e->initializeScript();
		}
		else
		{
			e->onRestore();
		}

		Py_XDECREF(cellData);
		
		// 这里增加一个引用， 因为可能在进入时被销毁
		Py_INCREF(e);

		space->addEntityToNode(e);
		bool isDestroyed = e->isDestroyed();
		Py_DECREF(e);

		if(isDestroyed == true)
			return;

		// 如果是有client的entity则设置它的cliententitycall, baseapp部分的onEntityGetCell会告知客户端enterworld.
		if(hasClient)
		{
			e->onGetWitness();
		}
		else
		{
			space->onEnterWorld(e);
		}

		return;
	}

	WARNING_MSG(fmt::format("Cellapp::_onCreateCellEntityFromBaseapp: rejected unavailable space, entityID={}, spaceID={}.\n",
		entityID, spaceID));
	Network::Bundle* pBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
	pBundle->newMessage(BaseappInterface::onCreateCellFailure);
	BaseappInterface::onCreateCellFailureArgs1::staticAddToBundle(*pBundle, entityID);
	cinfos->pChannel->send(pBundle);
}

//-------------------------------------------------------------------------------------
void Cellapp::onDestroyCellEntityFromBaseapp(Network::Channel* pChannel, ENTITY_ID eid)
{
	// DEBUG_MSG("Cellapp::onDestroyCellEntityFromBaseapp:entityID=%d.\n", eid);
	if (eid <= 0)
	{
		WARNING_MSG("Cellapp::onDestroyCellEntityFromBaseapp: rejected invalid entity ID.\n");
		return;
	}

	Components& components = Components::getSingleton();
	Components::ComponentInfos* sourceInfos = components.findComponent(pChannel);
	if (sourceInfos == NULL && pChannel != NULL && pChannel->componentID() != 0)
		sourceInfos = components.findComponent(pChannel->componentID());
	const bool sourceAvailable = pChannel != NULL && !pChannel->isExternal() &&
		!pChannel->isDestroyed() && pChannel->condemn() == 0 && sourceInfos != NULL &&
		Security::isBoundBidirectionalComponentSource(sourceInfos->cid, sourceInfos,
			pChannel, pChannel->componentID());
	if (!sourceAvailable || (sourceInfos->componentType != BASEAPP_TYPE &&
		sourceInfos->componentType != CELLAPP_TYPE))
	{
		WARNING_MSG(fmt::format("Cellapp::onDestroyCellEntityFromBaseapp: rejected unbound source, entityID={}, sourceComponentID={}.\n",
			eid, sourceInfos != NULL ? sourceInfos->cid : 0));
		return;
	}

	KBEngine::Entity* e = KBEngine::Cellapp::getSingleton().findEntity(eid);

	if (e == NULL)
	{
		WARNING_MSG(fmt::format("Cellapp::onDestroyCellEntityFromBaseapp: not found entityID:{}.\n",
			eid));

		return;
	}

	const bool fromBoundBaseapp = sourceInfos->componentType == BASEAPP_TYPE &&
		e->baseEntityCall() != NULL && e->baseEntityCall()->componentID() == sourceInfos->cid;
	const bool fromCurrentGhost = sourceInfos->componentType == CELLAPP_TYPE &&
		e->isReal() && e->ghostCell() == sourceInfos->cid;
	if (!fromBoundBaseapp && !fromCurrentGhost)
	{
		WARNING_MSG(fmt::format("Cellapp::onDestroyCellEntityFromBaseapp: rejected source not bound to entity, entityID={}, sourceComponentID={}.\n",
			eid, sourceInfos->cid));
		return;
	}

	if (!e->isReal())
	{
		// 需要做中转
		GhostManager* gm = Cellapp::getSingleton().pGhostManager();
		if (gm)
		{
			Network::Bundle* pBundle = gm->createSendBundle(e->realCell());
			pBundle->newMessage(CellappInterface::onDestroyCellEntityFromBaseapp);
			(*pBundle) << eid;
			gm->pushMessage(e->realCell(), pBundle);
		}

		return;
	}

	destroyEntity(eid, true);
}

//-------------------------------------------------------------------------------------
RemoteEntityMethod* Cellapp::createEntityCallCallEntityRemoteMethod(MethodDescription* pMethodDescription, EntityCall* pEntityCall)
{
	return new EntityRemoteMethod(pMethodDescription, pEntityCall);
}

//-------------------------------------------------------------------------------------
void Cellapp::onEntityCall(Network::Channel* pChannel, KBEngine::MemoryStream& s)
{
	ENTITY_ID eid;
	s >> eid;

	ENTITYCALL_TYPE	calltype;
	s >> calltype;

	// 在本地区尝试查找该收件人信息， 看收件人是否属于本区域
	Entity* entity = pEntities_->find(eid);
	if(entity == NULL)
	{
		if(calltype == ENTITYCALL_TYPE_CELL)
		{
			GhostManager* gm = Cellapp::getSingleton().pGhostManager();
			COMPONENT_ID cellID = gm ? gm->getRoute(eid) : 0;
			if(gm && cellID > 0)
			{
				Network::Bundle* pBundle = gm->createSendBundle(cellID);
				(*pBundle).newMessage(CellappInterface::onEntityCall);
				(*pBundle) << eid << calltype;
				(*pBundle).append(s);
				gm->pushMessage(cellID, pBundle);
				s.done();
				return;
			}
		}

		WARNING_MSG(fmt::format("Cellapp::onEntityCall: entityID {} not found.\n", eid));
		s.done();
		return;
	}

	switch(calltype)
	{
		// 本组件是cellapp，那么确认邮件的目的地是这里， 那么执行最终操作
		case ENTITYCALL_TYPE_CELL:	
			{
				if(!entity->isReal())
				{
					GhostManager* gm = Cellapp::getSingleton().pGhostManager();
					if(gm)
					{
						Network::Bundle* pBundle = gm->createSendBundle(entity->realCell());
						pBundle->newMessage(CellappInterface::onEntityCall);
						(*pBundle) << eid << calltype;
						pBundle->append(s);
						gm->pushMessage(entity->realCell(), pBundle);
					}
				}
				else
				{
					entity->onRemoteMethodCall(pChannel, s);
				}
			}

			break;

		// entity.base.cell.xxx
		case ENTITYCALL_TYPE_BASE_VIA_CELL: 
			{
				EntityCallAbstract* entitycall = static_cast<EntityCallAbstract*>(entity->baseEntityCall());
				if(entitycall == NULL)
				{ 
					//WARNING_MSG(fmt::format("Cellapp::onEntityCall: not found baseEntityCall! entityCallType={}, entityID={}.\n",
					//	calltype, eid));

					break;
				}
				
				Network::Channel* pChannel = entitycall->getChannel();
				if (pChannel)
				{
					Network::Bundle* pBundle = pChannel->createSendBundle();
					entitycall->newCall_(*pBundle);
					pBundle->append(s);
					pChannel->send(pBundle);
				}
			}
			break;
		
		// entity.cell.client
		case ENTITYCALL_TYPE_CLIENT_VIA_CELL: 
			{
				EntityCallAbstract* entitycall = static_cast<EntityCallAbstract*>(entity->clientEntityCall());
				if(entitycall == NULL)
				{
					//WARNING_MSG(fmt::format("Cellapp::onEntityCall: not found clientEntityCall! entityCallType={}, entityID={}.\n",
					//	calltype, eid));

					break;
				}
				
				Network::Channel* pChannel = entitycall->getChannel();
				if (pChannel)
				{
					Network::Bundle* pBundle = pChannel->createSendBundle();
					entitycall->newCall_(*pBundle);
					pBundle->append(s);
					pChannel->send(pBundle);
				}
			}
			break;
		default:
			{
				ERROR_MSG(fmt::format("Cellapp::onEntityCall: entityCallType {} error! must a cellType. entityID={}.\n",
					static_cast<int>(calltype), eid));
			}
	};

	s.done();
}

//-------------------------------------------------------------------------------------
void Cellapp::onRemoteCallMethodFromClient(Network::Channel* pChannel, KBEngine::MemoryStream& s)
{
	if (s.length() < sizeof(ENTITY_ID) * 2)
	{
		WARNING_MSG("Cellapp::onRemoteCallMethodFromClient: rejected incomplete fixed header.\n");
		s.done();
		return;
	}

	ENTITY_ID srcEntityID, targetID;

	s >> srcEntityID >> targetID;

	Components& components = Components::getSingleton();
	Components::ComponentInfos* sourceComponent = components.findComponent(pChannel);
	if (sourceComponent == NULL && pChannel != NULL && pChannel->componentID() != 0)
		sourceComponent = components.findComponent(pChannel->componentID());
	const bool sourceBound = sourceComponent != NULL &&
		Security::isBoundBidirectionalComponentSource(sourceComponent->cid,
			sourceComponent, pChannel, pChannel != NULL ? pChannel->componentID() : 0);
	const bool fromBaseapp = sourceBound && sourceComponent->componentType == BASEAPP_TYPE;
	const bool fromCellapp = sourceBound && sourceComponent->componentType == CELLAPP_TYPE;
	if (!fromBaseapp && !fromCellapp)
	{
		WARNING_MSG(fmt::format("Cellapp::onRemoteCallMethodFromClient: rejected unregistered source Channel, "
			"srcEntityID={}, targetID={}, addr={}.\n", srcEntityID, targetID, pChannel->c_str()));
		s.done();
		return;
	}

	KBEngine::Entity* e = KBEngine::Cellapp::getSingleton().findEntity(targetID);

	if(e == NULL)
	{	
		WARNING_MSG(fmt::format("Cellapp::onRemoteCallMethodFromClient: not found entityID:{}, srcEntityID:{}.\n", 
			targetID, srcEntityID));
		
		s.done();
		return;
	}

	uint64 relayEpoch = 0;
	if (fromCellapp)
	{
		if (s.length() < sizeof(relayEpoch))
		{
			WARNING_MSG("Cellapp::onRemoteCallMethodFromClient: rejected missing relay epoch.\n");
			s.done();
			return;
		}
		s >> relayEpoch;
		if (relayEpoch == 0 || relayEpoch != e->routingEpoch())
		{
			WARNING_MSG(fmt::format("Cellapp::onRemoteCallMethodFromClient: rejected stale relay epoch, "
				"entityID={}, received={}, current={}.\n", targetID, relayEpoch, e->routingEpoch()));
			s.done();
			return;
		}
	}

	// BaseApp is the authenticated client boundary. Bind that concrete Channel to
	// the source Entity and enforce the engine-level relationship before any
	// Ghost forwarding. Registered CellApp sources are continuations of a request
	// already admitted at the originating real Cell.
	// BaseApp 是客户端认证边界。Ghost 转发前必须把具体 Channel 绑定到来源实体，
	// 并执行引擎级关系授权；已注册 CellApp 来源仅承接起始 real Cell 已放行的请求。
	if (fromBaseapp)
	{
		KBEngine::Entity* sourceEntity = KBEngine::Cellapp::getSingleton().findEntity(srcEntityID);
		const bool sourceBoundToChannel = sourceEntity != NULL && sourceEntity->isReal() &&
			sourceEntity->baseEntityCall() != NULL &&
			sourceEntity->baseEntityCall()->componentID() == sourceComponent->cid;

		const bool controlledBySource = e->controlledBy() != NULL &&
			e->controlledBy()->id() == srcEntityID;
		const bool inSourceView = sourceEntity != NULL && sourceEntity->pWitness() != NULL &&
			sourceEntity->pWitness()->entityInView(targetID);

		if (!sourceBoundToChannel ||
			!Security::isAuthorizedClientCellTarget(srcEntityID, targetID,
				sourceEntity != NULL ? sourceEntity->spaceID() : 0, e->spaceID(),
				controlledBySource, inSourceView))
		{
			WARNING_MSG(fmt::format("Cellapp::onRemoteCallMethodFromClient: rejected unauthorized target, "
				"srcEntityID={}, targetID={}, sourceSpaceID={}, targetSpaceID={}, baseappID={}.\n",
				srcEntityID, targetID, sourceEntity != NULL ? sourceEntity->spaceID() : 0,
				e->spaceID(), sourceComponent->cid));
			s.done();
			return;
		}
	}
	else if (fromCellapp && !Security::isAuthorizedCellRelay(e->isReal(),
		sourceComponent->cid, e->ghostCell()))
	{
		WARNING_MSG(fmt::format("Cellapp::onRemoteCallMethodFromClient: rejected stale CellApp relay, "
			"srcEntityID={}, targetID={}, sourceCellappID={}, targetGhostCellID={}, targetIsReal={}.\n",
			srcEntityID, targetID, sourceComponent->cid, e->ghostCell(), e->isReal()));
		s.done();
		return;
	}

	// 这个方法呼叫如果不是这个proxy自己的方法则必须呼叫的entity和proxy的cellEntity在一个space中。
	try
	{
		if (!e->isReal())
		{
			// 需要做中转
			GhostManager* gm = Cellapp::getSingleton().pGhostManager();
			if (gm)
			{
				Network::Bundle* pBundle = gm->createSendBundle(e->realCell());
				pBundle->newMessage(CellappInterface::onRemoteCallMethodFromClient);
				(*pBundle) << srcEntityID;
				(*pBundle) << targetID;
				(*pBundle) << e->routingEpoch();
				pBundle->append(s);
				gm->pushMessage(e->realCell(), pBundle);
			}

			return;
		}

		e->onRemoteCallMethodFromClient(pChannel, srcEntityID, s);
	}catch(MemoryStreamException &)
	{
		ERROR_MSG(fmt::format("Cellapp::onRemoteCallMethodFromClient: message error! entityID:{}.\n", 
			targetID));

		s.done();
		return;
	}
}

//-------------------------------------------------------------------------------------
void Cellapp::onUpdateDataFromClient(Network::Channel* pChannel, KBEngine::MemoryStream& s)
{
	if (s.length() < sizeof(ENTITY_ID))
	{
		WARNING_MSG("Cellapp::onUpdateDataFromClient: rejected incomplete fixed header.\n");
		s.done();
		return;
	}

	ENTITY_ID srcEntityID = 0;

	s >> srcEntityID;
	if(srcEntityID <= 0)
		return;
	
	if(s.length() <= 0)
		return;

	KBEngine::Entity* e = findEntity(srcEntityID);	

	if(e == NULL)
	{
		WARNING_MSG(fmt::format("Cellapp::onUpdateDataFromClient: not found entity {}!\n", srcEntityID));
		
		s.done();
		return;
	}

	// 如果是被系统控制了，又或被别人控制了，则忽略来自自己客户端的更新消息
	if (e->controlledBy() == NULL || e->controlledBy()->id() != srcEntityID)
	{
		// phw: 经测试发现，由于controlledBy改变时通知客户端存在一定的时间差，
		//      所以客户端收到消息前仍然发送位移消息，这使得下面的错误日志变得有点多，
		//      因此注释掉这个日志，以减少不必要的日志输出。
		//ERROR_MSG(fmt::format("Cellapp::onUpdateDataFromClientForControlledEntity: entity {} has no permission to control entity {}!\n", proxiesEntityID, srcEntityID));

		s.done();
		return;
	}

	e->onUpdateDataFromClient(s);
	s.done();
}

//-------------------------------------------------------------------------------------
void Cellapp::setWitnessVolatileUpdatesEnabled(Network::Channel* pChannel,
	ENTITY_ID entityID, uint8 enabled)
{
	if (pChannel == NULL || pChannel->isExternal())
		return;

	Entity* entity = findEntity(entityID);
	if (entity == NULL)
	{
		// Backpressure transitions race with Cell migration and destruction. The next evaluation
		// targets the current Cell, so a stale control message is idempotently discarded.
		// 背压状态切换可能与 Cell 迁移或销毁竞态；下一轮评估会命中当前 Cell，因此旧控制消息可幂等丢弃。
		++staleWitnessVolatileControlDrops_;
		return;
	}

	entity->setWitnessVolatileUpdatesEnabled(pChannel, enabled);
}

//-------------------------------------------------------------------------------------
void Cellapp::onUpdateDataFromClientForControlledEntity(Network::Channel* pChannel, KBEngine::MemoryStream& s)
{
	if (s.length() < sizeof(ENTITY_ID) * 2)
	{
		WARNING_MSG("Cellapp::onUpdateDataFromClientForControlledEntity: rejected incomplete fixed header.\n");
		s.done();
		return;
	}

	ENTITY_ID proxiesEntityID = 0;
	s >> proxiesEntityID;
	if(proxiesEntityID <= 0)
		return;

	if(s.length() <= 0)
		return;

	ENTITY_ID srcEntityID = 0;

	s >> srcEntityID;
	if(srcEntityID <= 0)
		return;
	
	if(s.length() <= 0)
		return;

	KBEngine::Entity* e = findEntity(srcEntityID);	

	if(e == NULL)
	{
		ERROR_MSG(fmt::format("Cellapp::onUpdateDataFromClientForControlledEntity: not found entity {}!\n", srcEntityID));
		
		s.done();
		return;
	}

	if (e->controlledBy() == NULL || e->controlledBy()->id() != proxiesEntityID)
	{
		// phw: 经测试发现，由于controlledBy改变时通知客户端存在一定的时间差，
		//      所以客户端收到消息前仍然发送位移消息，这使得下面的错误日志变得有点多，
		//      因此注释掉这个日志，以减少不必要的日志输出。
		//ERROR_MSG(fmt::format("Cellapp::onUpdateDataFromClientForControlledEntity: entity {} has no permission to control entity {}!\n", proxiesEntityID, srcEntityID));
		
		s.done();
		return;
	}

	e->onUpdateDataFromClient(s);
	s.done();
}

//-------------------------------------------------------------------------------------
void Cellapp::onUpdateGhostPropertys(Network::Channel* pChannel, KBEngine::MemoryStream& s)
{
	if (s.length() < sizeof(ENTITY_ID))
	{
		WARNING_MSG("Cellapp::onUpdateGhostPropertys: rejected incomplete fixed header.\n");
		s.done();
		return;
	}
	Components::ComponentInfos* sourceInfos = findBoundCellappSource(pChannel);
	if (sourceInfos == NULL)
	{
		WARNING_MSG("Cellapp::onUpdateGhostPropertys: rejected unbound CellApp source.\n");
		s.done();
		return;
	}

	ENTITY_ID entityID;
	
	s >> entityID;

	Entity* entity = findEntity(entityID);
	if(entity == NULL)
	{
		GhostManager* gm = Cellapp::getSingleton().pGhostManager();
		if(gm)
		{
			COMPONENT_ID targetCell = gm->getRoute(entityID);
			if(targetCell > 0)
			{
				Network::Bundle* pForwardBundle = gm->createSendBundle(targetCell);
				(*pForwardBundle).newMessage(CellappInterface::onUpdateGhostPropertys);
				(*pForwardBundle) << entityID;
				pForwardBundle->append(s);

				gm->pushRouteMessage(entityID, targetCell, pForwardBundle);
				s.done();
				return;
			}
		}

		ERROR_MSG(fmt::format("Cellapp::onUpdateGhostPropertys: not found entity({})\n", 
			entityID));

		s.done();
		return;
	}
	if (!isCurrentGhostPeer(entity, sourceInfos->cid))
	{
		WARNING_MSG(fmt::format("Cellapp::onUpdateGhostPropertys: rejected stale CellApp source, entityID={}, sourceCellID={}.\n",
			entityID, sourceInfos->cid));
		s.done();
		return;
	}

	entity->onUpdateGhostPropertys(s);
}

//-------------------------------------------------------------------------------------
void Cellapp::onRemoteRealMethodCall(Network::Channel* pChannel, KBEngine::MemoryStream& s)
{
	if (s.length() < sizeof(ENTITY_ID))
	{
		WARNING_MSG("Cellapp::onRemoteRealMethodCall: rejected incomplete fixed header.\n");
		s.done();
		return;
	}
	Components::ComponentInfos* sourceInfos = findBoundCellappSource(pChannel);
	if (sourceInfos == NULL)
	{
		WARNING_MSG("Cellapp::onRemoteRealMethodCall: rejected unbound CellApp source.\n");
		s.done();
		return;
	}

	ENTITY_ID entityID;
	
	s >> entityID;

	Entity* entity = findEntity(entityID);
	if(entity == NULL)
	{
		GhostManager* gm = Cellapp::getSingleton().pGhostManager();
		if(gm)
		{
			COMPONENT_ID targetCell = gm->getRoute(entityID);
			if(targetCell > 0)
			{
				Network::Bundle* pForwardBundle = gm->createSendBundle(targetCell);
				(*pForwardBundle).newMessage(CellappInterface::onRemoteRealMethodCall);
				(*pForwardBundle) << entityID;
				pForwardBundle->append(s);

				gm->pushRouteMessage(entityID, targetCell, pForwardBundle);
				s.done();
				return;
			}
		}

		ERROR_MSG(fmt::format("Cellapp::onRemoteRealMethodCall: not found entity({})\n", 
			entityID));

		s.done();
		return;
	}
	if (!isCurrentGhostPeer(entity, sourceInfos->cid))
	{
		WARNING_MSG(fmt::format("Cellapp::onRemoteRealMethodCall: rejected stale CellApp source, entityID={}, sourceCellID={}.\n",
			entityID, sourceInfos->cid));
		s.done();
		return;
	}

	entity->onRemoteRealMethodCall(s);
}

//-------------------------------------------------------------------------------------
void Cellapp::onUpdateGhostVolatileData(Network::Channel* pChannel, KBEngine::MemoryStream& s)
{
	if (s.length() < sizeof(ENTITY_ID))
	{
		WARNING_MSG("Cellapp::onUpdateGhostVolatileData: rejected incomplete fixed header.\n");
		s.done();
		return;
	}
	Components::ComponentInfos* sourceInfos = findBoundCellappSource(pChannel);
	if (sourceInfos == NULL)
	{
		WARNING_MSG("Cellapp::onUpdateGhostVolatileData: rejected unbound CellApp source.\n");
		s.done();
		return;
	}

	ENTITY_ID entityID;
	
	s >> entityID;

	Entity* entity = findEntity(entityID);
	if(entity == NULL)
	{
		GhostManager* gm = Cellapp::getSingleton().pGhostManager();
		if(gm)
		{
			COMPONENT_ID targetCell = gm->getRoute(entityID);
			if(targetCell > 0)
			{
				Network::Bundle* pForwardBundle = gm->createSendBundle(targetCell);
				(*pForwardBundle).newMessage(CellappInterface::onUpdateGhostVolatileData);
				(*pForwardBundle) << entityID;
				pForwardBundle->append(s);

				gm->pushRouteMessage(entityID, targetCell, pForwardBundle);
				s.done();
				return;
			}
		}

		ERROR_MSG(fmt::format("Cellapp::onUpdateGhostVolatileData: not found entity({})\n", 
			entityID));

		s.done();
		return;
	}
	if (!isCurrentGhostPeer(entity, sourceInfos->cid))
	{
		WARNING_MSG(fmt::format("Cellapp::onUpdateGhostVolatileData: rejected stale CellApp source, entityID={}, sourceCellID={}.\n",
			entityID, sourceInfos->cid));
		s.done();
		return;
	}

	entity->onUpdateGhostVolatileData(s);
}

//-------------------------------------------------------------------------------------
void Cellapp::forwardEntityMessageToCellappFromClient(Network::Channel* pChannel, MemoryStream& s)
{
	if (s.length() < sizeof(ENTITY_ID))
	{
		WARNING_MSG("Cellapp::forwardEntityMessageToCellappFromClient: rejected incomplete fixed header.\n");
		s.done();
		return;
	}

	ENTITY_ID srcEntityID;

	s >> srcEntityID;

	Components& components = Components::getSingleton();
	Components::ComponentInfos* sourceComponent = components.findComponent(pChannel);
	if (sourceComponent == NULL && pChannel != NULL && pChannel->componentID() != 0)
		sourceComponent = components.findComponent(pChannel->componentID());
	const bool sourceBound = sourceComponent != NULL &&
		Security::isBoundBidirectionalComponentSource(sourceComponent->cid,
			sourceComponent, pChannel, pChannel != NULL ? pChannel->componentID() : 0);
	const bool fromBaseapp = sourceBound && sourceComponent->componentType == BASEAPP_TYPE;
	const bool fromCellapp = sourceBound && sourceComponent->componentType == CELLAPP_TYPE;
	if (!fromBaseapp && !fromCellapp)
	{
		WARNING_MSG(fmt::format("Cellapp::forwardEntityMessageToCellappFromClient: rejected unregistered "
			"source Channel, srcEntityID={}, addr={}.\n", srcEntityID,
			pChannel != NULL ? pChannel->c_str() : "none"));
		s.done();
		return;
	}

	KBEngine::Entity* e = KBEngine::Cellapp::getSingleton().findEntity(srcEntityID);

	if(e == NULL)
	{	
		WARNING_MSG(fmt::format("Cellapp::forwardEntityMessageToCellappFromClient: not found entityID:{}.\n",
			srcEntityID));
		
		s.done();
		return;
	}

	// BaseApp is the authenticated client boundary. The entity may already be a
	// Ghost during migration, so bind its Base EntityCall before forwarding it.
	// BaseApp 是客户端认证边界。迁移期间实体可能已经成为 Ghost，因此必须先把
	// 实际 BaseApp Channel 绑定到实体的 Base EntityCall，再允许继续转发。
	if (fromBaseapp && (e->baseEntityCall() == NULL ||
		e->baseEntityCall()->componentID() != sourceComponent->cid))
	{
		WARNING_MSG(fmt::format("Cellapp::forwardEntityMessageToCellappFromClient: rejected unbound "
			"BaseApp source, srcEntityID={}, baseappID={}, entityBaseappID={}.\n",
			srcEntityID, sourceComponent->cid,
			e->baseEntityCall() != NULL ? e->baseEntityCall()->componentID() : 0));
		s.done();
		return;
	}

	uint64 relayEpoch = 0;
	if (fromCellapp)
	{
		if (s.length() < sizeof(relayEpoch))
		{
			WARNING_MSG("Cellapp::forwardEntityMessageToCellappFromClient: rejected missing relay epoch.\n");
			s.done();
			return;
		}
		s >> relayEpoch;
		if (relayEpoch == 0 || relayEpoch != e->routingEpoch())
		{
			WARNING_MSG(fmt::format("Cellapp::forwardEntityMessageToCellappFromClient: rejected stale relay epoch, "
				"entityID={}, received={}, current={}.\n", srcEntityID, relayEpoch, e->routingEpoch()));
			s.done();
			return;
		}
	}

	if(e->isDestroyed())
	{
		ERROR_MSG(fmt::format("{}::forwardEntityMessageToCellappFromClient: {} is destroyed!\n",	
			e->scriptName(), e->id()));

		s.done();
		return;
	}

	if (!e->isReal())
	{
		// 需要做中转
		GhostManager* gm = Cellapp::getSingleton().pGhostManager();
		if (gm)
		{
			Network::Bundle* pBundle = gm->createSendBundle(e->realCell());
			pBundle->newMessage(CellappInterface::forwardEntityMessageToCellappFromClient);
			(*pBundle) << srcEntityID;
			(*pBundle) << e->routingEpoch();
			pBundle->append(s);
			gm->pushMessage(e->realCell(), pBundle);
		}

		return;
	}

	// 检查是否是entity消息， 否则不合法.
	while(s.length() > 0 && !e->isDestroyed())
	{
		Network::MessageID currMsgID;
		Network::MessageLength currMsgLen;

		s >> currMsgID;

		Network::MessageHandler* pMsgHandler = CellappInterface::messageHandlers.find(currMsgID);

		if(pMsgHandler == NULL)
		{
			ERROR_MSG(fmt::format("Cellapp::forwardEntityMessageToCellappFromClient: invalide msgID={}, msglen={}, from {}.\n", 
				currMsgID, s.wpos(), pChannel->c_str()));

			s.done();
			return;
		}

		if(pMsgHandler->type() != Network::NETWORK_MESSAGE_TYPE_ENTITY)
		{
			WARNING_MSG(fmt::format("Cellapp::forwardEntityMessageToCellappFromClient: msgID={} not is entitymsg.\n",
				currMsgID));

			s.done();
			return;
		}

		if(pMsgHandler->msgLen == NETWORK_VARIABLE_MESSAGE)
			s >> currMsgLen;
		else
			currMsgLen = pMsgHandler->msgLen;

		if(s.length() < currMsgLen || currMsgLen >  NETWORK_MESSAGE_MAX_SIZE / 2)
		{
			ERROR_MSG(fmt::format("Cellapp::forwardEntityMessageToCellappFromClient: msgID={}, invalide msglen={}, from {}.\n", 
				currMsgID, s.wpos(), pChannel->c_str()));

			s.done();
			return;
		}

		// 临时设置有效读取位， 防止接口中溢出操作
		size_t wpos = s.wpos();
		// size_t rpos = s.rpos();
		size_t frpos = s.rpos() + currMsgLen;
		s.wpos((int)frpos);

		try
		{
			pMsgHandler->handle(pChannel, s);
		}catch(MemoryStreamException &)
		{
			ERROR_MSG(fmt::format("Cellapp::forwardEntityMessageToCellappFromClient: message({}) error! entityID:{}.\n", 
				pMsgHandler->name.c_str(), srcEntityID));

			s.done();
			return;
		}

		// 防止handle中没有将数据导出获取非法操作
		if(currMsgLen > 0)
		{
			if(frpos != s.rpos())
			{
				ERROR_MSG(fmt::format("Cellapp::forwardEntityMessageToCellappFromClient[{}]: rejected under-consumed message, rpos={}, expect={}, msgID={}, msglen={}.\n",
					pMsgHandler->name.c_str(), s.rpos(), frpos, currMsgID, currMsgLen));
				s.done();
				return;
			}
		}

		s.wpos((int)wpos);
	}
}

//-------------------------------------------------------------------------------------
bool Cellapp::addUpdatable(Updatable* pObject)
{
	return updatables_.add(pObject);
}

//-------------------------------------------------------------------------------------
bool Cellapp::removeUpdatable(Updatable* pObject)
{
	return updatables_.remove(pObject);
}

//-------------------------------------------------------------------------------------
void Cellapp::lookApp(Network::Channel* pChannel)
{
	if(pChannel->isExternal())
		return;

	//DEBUG_MSG(fmt::format("Cellapp::lookApp: {}\n", pChannel->c_str()));

	Network::Bundle* pBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
	
	(*pBundle) << g_componentType;
	(*pBundle) << componentID_;

	ShutdownHandler::SHUTDOWN_STATE state = shuttingdown();
	int8 istate = int8(state);
	(*pBundle) << istate;
	(*pBundle) << this->entitiesSize();
	(*pBundle) << (int32)Spaces::size();

	uint32 port = 0;
	if(pTelnetServer_)
		port = pTelnetServer_->port();

	(*pBundle) << port;

	pChannel->send(pBundle);
}

//-------------------------------------------------------------------------------------
PyObject* Cellapp::__py_reloadScript(PyObject* self, PyObject* args)
{
	bool fullReload = true;
	int argCount = (int)PyTuple_Size(args);
	if(argCount == 1)
	{
		if(!PyArg_ParseTuple(args, "b", &fullReload))
		{
			PyErr_Format(PyExc_TypeError, "KBEngine::reloadScript(fullReload): args error!");
			PyErr_PrintEx(0);
			return 0;
		}
	}

	Cellapp::getSingleton().reloadScript(fullReload);
	S_Return;
}

//-------------------------------------------------------------------------------------
void Cellapp::reloadScript(bool fullReload)
{
	EntityApp<Entity>::reloadScript(fullReload);
}

//-------------------------------------------------------------------------------------
void Cellapp::onReloadScript(bool fullReload)
{
	Entities<Entity>::ENTITYS_MAP& entities = pEntities_->getEntities();
	Entities<Entity>::ENTITYS_MAP::iterator eiter = entities.begin();
	for(; eiter != entities.end(); ++eiter)
	{
		static_cast<Entity*>(eiter->second.get())->reload(fullReload);
	}

	EntityApp<Entity>::onReloadScript(fullReload);
}

//-------------------------------------------------------------------------------------
PyObject* Cellapp::__py_isShuttingDown(PyObject* self, PyObject* args)
{
	return PyBool_FromLong(Cellapp::getSingleton().isShuttingdown() ? 1 : 0);
}

//-------------------------------------------------------------------------------------
PyObject* Cellapp::__py_address(PyObject* self, PyObject* args)
{
	PyObject* pyobj = PyTuple_New(2);
	const Network::Address& addr = Cellapp::getSingleton().networkInterface().intEndpoint().addr();
	PyTuple_SetItem(pyobj, 0,  PyLong_FromUnsignedLong(addr.ip));
	PyTuple_SetItem(pyobj, 1,  PyLong_FromUnsignedLong(addr.port));
	return pyobj;
}

//-------------------------------------------------------------------------------------
void Cellapp::reqTeleportToCellApp(Network::Channel* pChannel, MemoryStream& s)
{
	const size_t teleportHeaderSize = sizeof(ENTITY_ID) * 2 + sizeof(SPACE_ID) +
		sizeof(ENTITY_SCRIPT_UID) + sizeof(Position3D) + sizeof(Direction3D) +
		sizeof(COMPONENT_ID);
	if (s.length() < teleportHeaderSize)
	{
		WARNING_MSG("Cellapp::reqTeleportToCellApp: rejected incomplete fixed header.\n");
		s.done();
		return;
	}
	Components::ComponentInfos* sourceInfos = findBoundCellappSource(pChannel);
	if (sourceInfos == NULL)
	{
		WARNING_MSG("Cellapp::reqTeleportToCellApp: rejected unbound CellApp source.\n");
		s.done();
		return;
	}

	size_t rpos = s.rpos();

	ENTITY_ID nearbyMBRefID = 0, teleportEntityID = 0;
	Position3D pos;
	Direction3D dir;
	ENTITY_SCRIPT_UID entityType;
	SPACE_ID spaceID = 0;
	COMPONENT_ID entityBaseappID = 0;

	s >> teleportEntityID >> nearbyMBRefID >> spaceID;
	s >> entityType;
	s >> pos.x >> pos.y >> pos.z;
	s >> dir.dir.x >> dir.dir.y >> dir.dir.z;

	COMPONENT_ID ghostCell;
	s >> ghostCell;
	if (ghostCell == 0 || ghostCell != sourceInfos->cid)
	{
		WARNING_MSG(fmt::format("Cellapp::reqTeleportToCellApp: rejected sourceCellID={}, payloadSourceCellID={}.\n",
			sourceInfos->cid, ghostCell));
		s.done();
		return;
	}

	bool success = false;

	Entity* refEntity = Cellapp::getSingleton().findEntity(nearbyMBRefID);
	if (refEntity == NULL || refEntity->isDestroyed())
	{
		s.rpos((int)rpos);

		Network::Bundle* pBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
		(*pBundle).newMessage(CellappInterface::reqTeleportToCellAppCB);
		(*pBundle) << ghostCell << g_componentID << entityBaseappID;
		(*pBundle) << teleportEntityID;
		(*pBundle) << success;
		(*pBundle).append(&s);
		pChannel->send(pBundle);

		ERROR_MSG(fmt::format("Cellapp::reqTeleportToCellApp: not found refEntity({}), spaceID({}), reqTeleportEntity({})!\n",
			nearbyMBRefID, spaceID, teleportEntityID));

		s.done();
		return;
	}

	Space* space = Spaces::findSpace(refEntity->spaceID());
	if (space == NULL || !space->isGood())
	{
		s.rpos((int)rpos);

		Network::Bundle* pBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
		(*pBundle).newMessage(CellappInterface::reqTeleportToCellAppCB);
		(*pBundle) << ghostCell << g_componentID << entityBaseappID;
		(*pBundle) << teleportEntityID;
		(*pBundle) << success;
		(*pBundle).append(&s);
		pChannel->send(pBundle);

		ERROR_MSG(fmt::format("Cellapp::reqTeleportToCellApp: not found space({}),  reqTeleportEntity({})!\n", spaceID, teleportEntityID));
		s.done();
		return;
	}

	// 创建entity
	Entity* e = createEntity(EntityDef::findScriptModule(entityType)->getName(), NULL, false, teleportEntityID, false);
	if (e == NULL)
	{
		s.rpos((int)rpos);

		Network::Bundle* pBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
		(*pBundle).newMessage(CellappInterface::reqTeleportToCellAppCB);
		(*pBundle) << ghostCell << g_componentID << entityBaseappID;
		(*pBundle) << teleportEntityID;
		(*pBundle) << success;
		(*pBundle).append(&s);
		pChannel->send(pBundle);

		ERROR_MSG(fmt::format("Cellapp::reqTeleportToCellApp: create reqTeleportEntity({}) error!\n", teleportEntityID));
		s.done();
		return;
	}
	
	Py_INCREF(e);
	e->createFromStream(s);

	// 有可能序列化过来的ghost内容包含移动控制器，之所以序列化过来是为了
	// 在传送失败时可以用于恢复现场, 那么传送成功了我们应该停止以前的移动行为
	e->stopMove();

	// 对于传送操作来说，实体传送过来就不会有ghost部分了
	// 当前实体做出的任何改变不需要同步到原有cell，这可能会产生网络消息死循环
	//ghostCell = 0;
	e->ghostCell(0);

	e->spaceID(space->id());
	e->setPositionAndDirection(pos, dir);

	if (e->baseEntityCall())
	{
		e->addFlags(ENTITY_FLAGS_TELEPORT_START);
		
		// 如果是有base的实体，需要将baseappID填入，以便在reqTeleportToCellAppCB中回调给baseapp传输结束状态
		entityBaseappID = e->baseEntityCall()->componentID();

		// 向baseapp发送传送到达通知
		Network::Bundle* pBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
		(*pBundle).newMessage(BaseappInterface::onMigrationCellappEnd);
		(*pBundle) << e->id();
		(*pBundle) << ghostCell << g_componentID;
		e->baseEntityCall()->sendCall(pBundle);
	}

	// 进入新space之前必须通知客户端leaveSpace
	if (e->clientEntityCall())
	{
		Network::Bundle* pSendBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
		NETWORK_ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(e->id(), (*pSendBundle));
		ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pSendBundle, ClientInterface::onEntityLeaveSpace, leaveSpace);
		(*pSendBundle) << e->id();
		ENTITY_MESSAGE_FORWARD_CLIENT_END(pSendBundle, ClientInterface::onEntityLeaveSpace, leaveSpace);
		e->clientEntityCall()->sendCall(pSendBundle);
	}

	// 进入新的space中
	space->addEntityAndEnterWorld(e);

	Entity* nearbyMBRef = Cellapp::getSingleton().findEntity(nearbyMBRefID);
	e->onTeleportSuccess(nearbyMBRef, space->id());

	success = true;

	{
		Network::Bundle* pBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
		(*pBundle).newMessage(CellappInterface::reqTeleportToCellAppCB);
		(*pBundle) << ghostCell << g_componentID << entityBaseappID;
		(*pBundle) << teleportEntityID;
		(*pBundle) << success;
		pChannel->send(pBundle);
	}
	
	Py_DECREF(e);
}

//-------------------------------------------------------------------------------------
void Cellapp::reqTeleportToCellAppCB(Network::Channel* pChannel, MemoryStream& s)
{
	const size_t callbackHeaderSize = sizeof(COMPONENT_ID) * 3 + sizeof(ENTITY_ID) + sizeof(bool);
	if (s.length() < callbackHeaderSize)
	{
		WARNING_MSG("Cellapp::reqTeleportToCellAppCB: rejected incomplete fixed header.\n");
		s.done();
		return;
	}
	if (pChannel == NULL || pChannel->isDestroyed() || pChannel->condemn() != 0)
	{
		WARNING_MSG("Cellapp::reqTeleportToCellAppCB: rejected unavailable source Channel.\n");
		s.done();
		return;
	}

	bool success;
	COMPONENT_ID sourceCellappID, targetCellappID, entityBaseappID;
	ENTITY_ID teleportEntityID = 0;

	s >> sourceCellappID >> targetCellappID >> entityBaseappID >> teleportEntityID >> success;

	Components::ComponentInfos* targetCellapp =
		Components::getSingleton().findComponent(CELLAPP_TYPE, targetCellappID);
	if (!Security::isBoundComponentSource(targetCellappID, targetCellapp, pChannel))
	{
		WARNING_MSG(fmt::format("Cellapp::reqTeleportToCellAppCB: rejected unbound targetCellappID={}, addr={}.\n",
			targetCellappID, pChannel->c_str()));
		s.done();
		return;
	}

	// 正常情况下， 应该传送结果返回时传送前的实体应该在当前cell上， 如果到其他cellapp上了， 说明在此期间被迁移走了
	// 此时被迁移很可能会有问题
	if (sourceCellappID != g_componentID)
	{
		ERROR_MSG(fmt::format("Cellapp::reqTeleportToCellAppCB(): sourceCellappID={} != currCellappID={}, targetCellappID={}\n",
			sourceCellappID, g_componentID, targetCellappID));
		s.done();
		return;
	}

	// 传送成功，我们销毁这个entity
	if(success)
	{
		destroyEntity(teleportEntityID, false);
		return;
	}

	// 实体可能没有base部分，那么不需要通知baseapp
	if (entityBaseappID > 0)
	{
		Components::ComponentInfos* pInfos = Components::getSingleton().findComponent(BASEAPP_TYPE, entityBaseappID);
		if (pInfos && pInfos->pChannel)
		{
			Network::Bundle* pBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
			(*pBundle).newMessage(BaseappInterface::onMigrationCellappEnd);
			(*pBundle) << teleportEntityID;
			(*pBundle) << sourceCellappID << sourceCellappID;
			pInfos->pChannel->send(pBundle);
		}
		else
		{
			ERROR_MSG(fmt::format("Cellapp::reqTeleportToCellAppCB: not found baseapp({}), entity({})!\n",
				entityBaseappID, teleportEntityID));
		}
	}

	// 某些情况下实体可能此时找不到了，例如：副本销毁了
	Entity* entity = Cellapp::getSingleton().findEntity(teleportEntityID);
	if(entity == NULL)
	{
		ERROR_MSG(fmt::format("Cellapp::reqTeleportToCellAppCB: not found reqTeleportEntity({}), lose entity!\n", 
			teleportEntityID));

		s.done();
		return;
	}

	// 传送失败了，我们需要重恢复entity
	ENTITY_ID nearbyMBRefID = 0;
	Position3D pos;
	Direction3D dir;
	ENTITY_SCRIPT_UID entityType;
	SPACE_ID lastSpaceID = 0;
	COMPONENT_ID cid;

	s >> teleportEntityID >> nearbyMBRefID >> lastSpaceID;
	s >> entityType;
	s >> pos.x >> pos.y >> pos.z;
	s >> dir.dir.x >> dir.dir.y >> dir.dir.z;
	s >> cid;

	Py_INCREF(entity);
	entity->changeToReal(0, s);
	entity->onTeleportFailure();
	Py_DECREF(entity);
	
	s.done();
}

//-------------------------------------------------------------------------------------
void Cellapp::reqTeleportToCellAppOver(Network::Channel* pChannel, MemoryStream& s)
{
	if (s.length() < sizeof(ENTITY_ID))
	{
		WARNING_MSG("Cellapp::reqTeleportToCellAppOver: rejected incomplete fixed header.\n");
		s.done();
		return;
	}
	if (!Components::getSingleton().isExpectedComponentChannel(BASEAPP_TYPE, pChannel))
	{
		WARNING_MSG("Cellapp::reqTeleportToCellAppOver: rejected non-BaseApp source.\n");
		s.done();
		return;
	}

	ENTITY_ID teleportEntityID = 0;

	s >> teleportEntityID;
	
	// 某些情况下实体可能此时找不到了，例如：副本销毁了
	Entity* entity = Cellapp::getSingleton().findEntity(teleportEntityID);
	if(entity == NULL)
	{
		ERROR_MSG(fmt::format("Cellapp::reqTeleportToCellAppOver: not found reqTeleportEntity({}), lose entity!\n", 
			teleportEntityID));

		s.done();
		return;
	}
	if (entity->baseEntityCall() == NULL ||
		entity->baseEntityCall()->componentID() != pChannel->componentID())
	{
		WARNING_MSG(fmt::format("Cellapp::reqTeleportToCellAppOver: rejected unbound BaseApp entity={}, sourceBaseAppID={}.\n",
			teleportEntityID, pChannel->componentID()));
		s.done();
		return;
	}

	entity->removeFlags(ENTITY_FLAGS_TELEPORT_START);
}

//-------------------------------------------------------------------------------------
int Cellapp::raycast(SPACE_ID spaceID, int layer, const Position3D& start, const Position3D& end, std::vector<Position3D>& hitPos)
{
	Space* pSpace = Spaces::findSpace(spaceID);
	if(pSpace == NULL)
	{
		ERROR_MSG(fmt::format("Cellapp::raycast: not found space({})!\n", 
			spaceID));

		return -1;
	}
	
	if(pSpace->pNavHandle() == NULL)
	{
		ERROR_MSG(fmt::format("Cellapp::raycast: space({}) not addSpaceGeometryMapping! layer={}\n", 
			spaceID, layer));

		return -1;
	}

	return pSpace->pNavHandle()->raycast(layer, start, end, hitPos);
}

//-------------------------------------------------------------------------------------
PyObject* Cellapp::__py_raycast(PyObject* self, PyObject* args)
{
	uint16 currargsSize = (uint16)PyTuple_Size(args);

	int layer = 0;
	SPACE_ID spaceID = 0;

	PyObject* pyStartPos = NULL;
	PyObject* pyEndPos = NULL;

	if(currargsSize == 3)
	{
		if(!PyArg_ParseTuple(args, "IOO", &spaceID, &pyStartPos, &pyEndPos))
		{
			PyErr_Format(PyExc_TypeError, "Cellapp::raycast: args error!");
			PyErr_PrintEx(0);
			return 0;
		}
	}
	else if(currargsSize == 4)
	{
		if(!PyArg_ParseTuple(args, "IiOO", &spaceID, &layer, &pyStartPos, &pyEndPos))
		{
			PyErr_Format(PyExc_TypeError, "Cellapp::raycast: args error!");
			PyErr_PrintEx(0);
			return 0;
		}
	}
	else
	{
		PyErr_Format(PyExc_TypeError, "Cellapp::raycast: args error!");
		PyErr_PrintEx(0);
		return 0;
	}

	if(!PySequence_Check(pyStartPos))
	{
		PyErr_Format(PyExc_TypeError, "Cellapp::raycast: args1(startPos) not is PySequence!");
		PyErr_PrintEx(0);
		return 0;
	}

	if(!PySequence_Check(pyEndPos))
	{
		PyErr_Format(PyExc_TypeError, "Cellapp::raycast: args2(endPos) not is PySequence!");
		PyErr_PrintEx(0);
		return 0;
	}

	if(PySequence_Size(pyStartPos) != 3)
	{
		PyErr_Format(PyExc_TypeError, "Cellapp::raycast: args1(startPos) invalid!");
		PyErr_PrintEx(0);
		return 0;
	}

	if(PySequence_Size(pyEndPos) != 3)
	{
		PyErr_Format(PyExc_TypeError, "Cellapp::raycast: args2(endPos) invalid!");
		PyErr_PrintEx(0);
		return 0;
	}

	Position3D startPos;
	Position3D endPos;
	std::vector<Position3D> hitPosVec;
	//float hitPos[3];

	script::ScriptVector3::convertPyObjectToVector3(startPos, pyStartPos);
	script::ScriptVector3::convertPyObjectToVector3(endPos, pyEndPos);
	if(Cellapp::getSingleton().raycast(spaceID, layer, startPos, endPos, hitPosVec) <= 0)
	{
		S_Return;
	}

	int idx = 0;
	PyObject* pyHitpos = PyTuple_New(hitPosVec.size());
	for(std::vector<Position3D>::iterator iter = hitPosVec.begin(); iter != hitPosVec.end(); ++iter)
	{
		PyObject* pyHitposItem = PyTuple_New(3);
		PyTuple_SetItem(pyHitposItem, 0, ::PyFloat_FromDouble((*iter).x));
		PyTuple_SetItem(pyHitposItem, 1, ::PyFloat_FromDouble((*iter).y));
		PyTuple_SetItem(pyHitposItem, 2, ::PyFloat_FromDouble((*iter).z));

		PyTuple_SetItem(pyHitpos, idx++, pyHitposItem);
	}

	return pyHitpos;
}

//-------------------------------------------------------------------------------------
PyObject* Cellapp::__py_getFlags(PyObject* self, PyObject* args)
{
	return PyLong_FromUnsignedLong(Cellapp::getSingleton().flags());
}

//-------------------------------------------------------------------------------------
PyObject* Cellapp::__py_setFlags(PyObject* self, PyObject* args)
{
	if(PyTuple_Size(args) != 1)
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::setFlags: argsSize != 1!");
		PyErr_PrintEx(0);
		return NULL;
	}

	uint32 flags;

	if(!PyArg_ParseTuple(args, "I", &flags))
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::setFlags: args error!");
		PyErr_PrintEx(0);
		return NULL;
	}

	Cellapp::getSingleton().flags(flags);
	S_Return;
}

//-------------------------------------------------------------------------------------
void Cellapp::setSpaceViewer(Network::Channel* pChannel, MemoryStream& s)
{
	bool del = false;
	s >> del;

	SPACE_ID spaceID;
	s >> spaceID;

	// 如果为0，则查看所有cell
	CELL_ID cellID;
	s >> cellID;

	spaceViewers_.updateSpaceViewer(pChannel->addr(), spaceID, cellID, del);
}

//-------------------------------------------------------------------------------------

}
