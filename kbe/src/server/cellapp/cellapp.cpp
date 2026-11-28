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
#include "spaces.h"
#include "space_entity.h"
#include "profile.h"
#include "witness.h"
#include "coordinate_node.h"
#include "coordinate_system.h"
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
#include "server/cell_backup.h"
#include "cell_handoff_snapshot.h"
#include "server/cell_process_watch.h"
#include "server/component_routing_guard.h"
#include "server/space_partitioning.h"
#include "network/event_poller.h"
#include "server/components.h"
#include "server/telnet_server.h"
#include "server/py_file_descriptor.h"
#include "server/script_stage_timing.h"
#include "server/teleport_status.h"
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

bool canReconnectCellPeer(const Components::ComponentInfos* current, const Components::ComponentInfos& peer)
{
	return !current || (current->componentType == CELLAPP_TYPE && !current->pChannel &&
		current->pIntAddr && *current->pIntAddr == *peer.pIntAddr);
}

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
	if (entity == NULL || entity->isBoundaryGhost() || sourceCellID == 0)
		return false;

	const COMPONENT_ID peerCellID = entity->isReal() ? entity->ghostCell() : entity->realCell();
	return peerCellID != 0 && peerCellID == sourceCellID;
}

bool forwardRemoteRealMethodCall(GhostManager* ghostManager, ENTITY_ID entityID,
	COMPONENT_ID sourceCellID, COMPONENT_ID targetCellID, MemoryStream& stream)
{
	if (ghostManager == NULL || targetCellID == 0 || targetCellID == g_componentID ||
		targetCellID == sourceCellID)
	{
		return false;
	}

	Network::Bundle* pForwardBundle = ghostManager->createSendBundle(targetCellID);
	(*pForwardBundle).newMessage(CellappInterface::onRemoteRealMethodCall);
	(*pForwardBundle) << entityID;
	pForwardBundle->append(stream);

	// 旧 Ghost 可以跨越多个迁移代际继续调用 real。每一跳都刷新 route，直到
	// 上游 Ghost/缓存自然失效；禁止回发给来源 Cell，避免陈旧环路自激。
	// An old Ghost may call the real entity across several migration generations.
	// Refresh the route at every hop while rejecting a bounce to the source Cell.
	ghostManager->pushRouteMessage(entityID, targetCellID, pForwardBundle);
	stream.done();
	return true;
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

bool bindClientEntityCallForCreatedEntity(Entity* entity, const char* operation)
{
	if (entity == NULL || entity->baseEntityCall() == NULL ||
		entity->clientEntityCall() != NULL || entity->pWitness() != NULL)
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

	// Keep the client EntityCall visible to initialization scripts, matching the 2.5 lifecycle.
	// Witness attachment is deliberately deferred because it immediately sends EnterWorld;
	// BaseApp must process onEntityGetCell and queue the initial Cell properties first.
	// 保留 2.5 的生命周期：初始化脚本仍可访问客户端 EntityCall。
	// Witness::attach 会立即发送 EnterWorld，因此必须延迟到 BaseApp 处理
	// onEntityGetCell 并将 Cell 初始属性排入客户端队列之后。
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
	staleWitnessVolatileControlDrops_(0),
	staleClientInputEntityDrops_(0),
	staleControlledClientInputEntityDrops_(0),
	spacePartitioningMode_(0),
	spacePartitioningConfirmed_(false),
	spacePartitioningRejected_(false),
	spacePartitioningRequestTime_(0)
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
	WATCH_OBJECT("spacePartitioning/enabled", this, &Cellapp::spacePartitioningEnabled);
	WATCH_OBJECT("spacePartitioning/confirmed", this, &Cellapp::spacePartitioningConfirmed);
	ProfileVal::setWarningPeriod(stampsPerSecond() / g_kbeSrvConfig.gameUpdateHertz());
	// Publish zero-sample latency windows before the first script, timer, or client update.
	// 在首次脚本、Timer 或客户端更新前发布零样本延迟窗口，避免空载场景缺少指标目录。
	SCRIPTCALL_PROFILE.initializeWatcher();
	ONTIMER_PROFILE.initializeWatcher();
	CLIENT_UPDATE_PROFILE.initializeWatcher();
	UPDATABLES_PROFILE.initializeWatcher();
	SPACES_UPDATE_PROFILE.initializeWatcher();

	ScriptStageMetrics& scriptMetrics = scriptStageMetrics();
	WATCH_OBJECT("scriptStages/rpcCalls", &scriptMetrics, &ScriptStageMetrics::rpcCalls);
	WATCH_OBJECT("scriptStages/rpcSampleRate", &scriptMetrics, &ScriptStageMetrics::rpcSampleRate);
	for (int stageValue = 0; stageValue < SCRIPT_STAGE_COUNT; ++stageValue)
	{
		const ScriptStage stage = static_cast<ScriptStage>(stageValue);
		ScriptStageStats* pStats = &scriptMetrics.stats(stage);
		const std::string prefix = std::string("scriptStages/") + ScriptStageMetrics::stageName(stage);
		WATCH_OBJECT(prefix + "Calls", pStats, &ScriptStageStats::calls);
		WATCH_OBJECT(prefix + "SampledCalls", pStats, &ScriptStageStats::sampledCalls);
		WATCH_OBJECT(prefix + "TotalNanos", pStats, &ScriptStageStats::totalNanos);
		WATCH_OBJECT(prefix + "AverageNanos", pStats, &ScriptStageStats::averageNanos);
		WATCH_OBJECT(prefix + "MaxNanos", pStats, &ScriptStageStats::maxNanos);
		WATCH_OBJECT(prefix + "SlowOver1ms", pStats, &ScriptStageStats::slowOver1ms);
	}
	for (size_t slot = 0; slot < ScriptStageMetrics::SLOW_TOP_CAPACITY; ++slot)
	{
		SlowScriptStage* pSlow = const_cast<SlowScriptStage*>(&scriptMetrics.slow(slot));
		const std::string prefix = std::string("scriptStages/slow") + std::to_string(slot);
		WATCH_OBJECT(prefix + "Name", pSlow->name);
		WATCH_OBJECT(prefix + "Stage", pSlow->stageName);
		WATCH_OBJECT(prefix + "DurationNanos", pSlow->durationNanos);
	}

	WATCH_OBJECT("load", this, &Cellapp::_getLoad);
	WATCH_OBJECT("spaceSize", this, &Cellapp::spaceSize);
	WATCH_OBJECT("spaces/count", &Spaces::snapshotSpaceCount);
	WATCH_OBJECT("spaces/entities/total", &Spaces::snapshotTotalEntities);
	WATCH_OBJECT("spaces/entities/max", &Spaces::snapshotMaxEntities);
	WATCH_OBJECT("spaces/entities/hottestSpaceID", &Spaces::snapshotMaxEntitiesSpaceID);
	WATCH_OBJECT("spaces/witnesses/total", &Spaces::snapshotTotalWitnesses);
	WATCH_OBJECT("spaces/witnesses/max", &Spaces::snapshotMaxWitnesses);
	WATCH_OBJECT("spaces/witnesses/hottestSpaceID", &Spaces::snapshotMaxWitnessesSpaceID);
	WATCH_OBJECT("spaces/pendingWitnesses/total", &Spaces::snapshotTotalPendingWitnesses);
	WATCH_OBJECT("spaces/pendingWitnesses/max", &Spaces::snapshotMaxPendingWitnesses);
	WATCH_OBJECT("spaces/pendingWitnesses/hottestSpaceID", &Spaces::snapshotMaxPendingWitnessesSpaceID);
	WATCH_OBJECT("spaces/aoiRelations/total", &Spaces::snapshotTotalAoiRelations);
	WATCH_OBJECT("spaces/aoiRelations/max", &Spaces::snapshotMaxAoiRelations);
	WATCH_OBJECT("spaces/aoiRelations/hottestSpaceID", &Spaces::snapshotMaxAoiRelationsSpaceID);
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
	WATCH_OBJECT("witness/scheduler/pending", &Witness::globalPendingCount);
	WATCH_OBJECT("witness/scheduler/pendingSnapshot", &Witness::globalPendingSnapshot);
	WATCH_OBJECT("witness/scheduler/pendingAdmitted", &Witness::globalPendingAdmittedCount);
	WATCH_OBJECT("witness/scheduler/pendingDeferred", &Witness::globalPendingDeferredCount);
	WATCH_OBJECT("witness/scheduler/pendingArrivals", &Witness::globalPendingArrivalCount);
	WATCH_OBJECT("witness/messages/enterCount", &Witness::enterUpdateCount);
	WATCH_OBJECT("witness/messages/enterBytes", &Witness::enterBytesCount);
	WATCH_OBJECT("witness/messages/leaveCount", &Witness::leaveUpdateCount);
	WATCH_OBJECT("witness/messages/leaveBytes", &Witness::leaveBytesCount);
	WATCH_OBJECT("witness/messages/enterProcessingSampleRate", &Witness::enterProcessingSampleRate);
	WATCH_OBJECT("witness/messages/enterProcessingSamples", &Witness::enterProcessingSamples);
	WATCH_OBJECT("witness/messages/enterProcessingTotalNanos", &Witness::enterProcessingTotalNanos);
	WATCH_OBJECT("witness/messages/enterProcessingAverageNanos", &Witness::enterProcessingAverageNanos);
	WATCH_OBJECT("witness/messages/enterProcessingMaxNanos", &Witness::enterProcessingMaxNanos);
	WATCH_OBJECT("witness/messages/enterProcessingSlowSamplesOver1ms", &Witness::enterProcessingSlowSamplesOver1ms);
	WATCH_OBJECT("witness/messages/leaveProcessingSampleRate", &Witness::leaveProcessingSampleRate);
	WATCH_OBJECT("witness/messages/leaveProcessingSamples", &Witness::leaveProcessingSamples);
	WATCH_OBJECT("witness/messages/leaveProcessingTotalNanos", &Witness::leaveProcessingTotalNanos);
	WATCH_OBJECT("witness/messages/leaveProcessingAverageNanos", &Witness::leaveProcessingAverageNanos);
	WATCH_OBJECT("witness/messages/leaveProcessingMaxNanos", &Witness::leaveProcessingMaxNanos);
	WATCH_OBJECT("witness/messages/leaveProcessingSlowSamplesOver1ms", &Witness::leaveProcessingSlowSamplesOver1ms);
	WATCH_OBJECT("witness/messages/volatileCount", &Witness::volatileUpdateCount);
	WATCH_OBJECT("witness/messages/volatileBytes", &Witness::volatileUpdateBytesCount);
	WATCH_OBJECT("witness/lod/nearUpdates", &Witness::lodNearUpdateCount);
	WATCH_OBJECT("witness/lod/mediumUpdates", &Witness::lodMediumUpdateCount);
	WATCH_OBJECT("witness/lod/farUpdates", &Witness::lodFarUpdateCount);
	WATCH_OBJECT("witness/lod/deferredRelations", &Witness::lodDeferredRelationCount);
	WATCH_OBJECT("witness/lod/distanceFilteredFields", &Witness::lodDistanceFilteredFieldCount);
	WATCH_OBJECT("coordinateSystem/equalCorrectionMoves",
		&CoordinateSystem::equalCoordinateCorrectionMoves);
	WATCH_OBJECT("coordinateSystem/equalCorrectionCallbacksSuppressed",
		&CoordinateSystem::equalCoordinateCallbacksSuppressed);
	WATCH_OBJECT("coordinateSystem/releasedNodes", &CoordinateSystem::releasedNodes);
	WATCH_OBJECT("coordinateSystem/releaseBatches", &CoordinateSystem::releaseBatches);
	WATCH_OBJECT("coordinateSystem/maxReleaseBatch", &CoordinateSystem::maxReleaseBatch);
	WATCH_OBJECT("witness/backpressure/activeSuppressed", &Witness::activeSuppressedCount);
	WATCH_OBJECT("witness/backpressure/suppressionTransitions", &Witness::suppressionTransitionCount);
	WATCH_OBJECT("witness/backpressure/resumeTransitions", &Witness::resumeTransitionCount);
	WATCH_OBJECT("witness/backpressure/staleControlDrops", this, &Cellapp::staleWitnessVolatileControlDrops);
	WATCH_OBJECT("network/clientInput/staleEntityDrops", this, &Cellapp::staleClientInputEntityDrops);
	WATCH_OBJECT("cellRecovery/stagedBackups", this, &Cellapp::stagedRecoveryBackups);
	WATCH_OBJECT("network/clientInput/staleControlledEntityDrops", this, &Cellapp::staleControlledClientInputEntityDrops);
	WATCH_OBJECT("witness/backpressure/suppressedUpdateSkips", &Witness::suppressedUpdateSkipCount);
	WATCH_OBJECT("witness/backpressure/suppressedVolatileRefreshes", &Witness::suppressedVolatileRefreshCount);
	WATCH_OBJECT("witness/backpressure/structuralWhileSuppressed", &Witness::structuralWhileSuppressedCount);
	WATCH_OBJECT("witness/backpressure/producerDeferred", &Witness::producerBackpressureDeferredCount);
	WATCH_OBJECT("witness/immediate/bundles", &Witness::immediateBundleCount);
	WATCH_OBJECT("witness/immediate/bytes", &Witness::immediateBytesCount);
	WATCH_OBJECT("witness/immediate/backpressuredBundles", &Witness::immediateBackpressuredBundleCount);
	WATCH_OBJECT("witness/immediate/propertyBundles", &Witness::immediatePropertyBundleCount);
	WATCH_OBJECT("witness/immediate/propertyBytes", &Witness::immediatePropertyBytesCount);
	WATCH_OBJECT("witness/immediate/rpcBundles", &Witness::immediateRpcBundleCount);
	WATCH_OBJECT("witness/immediate/rpcBytes", &Witness::immediateRpcBytesCount);
	WATCH_OBJECT("witness/immediate/positionBundles", &Witness::immediatePositionBundleCount);
	WATCH_OBJECT("witness/immediate/positionBytes", &Witness::immediatePositionBytesCount);
	WATCH_OBJECT("witness/immediate/spaceDataBundles", &Witness::immediateSpaceDataBundleCount);
	WATCH_OBJECT("witness/immediate/spaceDataBytes", &Witness::immediateSpaceDataBytesCount);
	WATCH_OBJECT("witness/immediate/otherBundles", &Witness::immediateOtherBundleCount);
	WATCH_OBJECT("witness/immediate/otherBytes", &Witness::immediateOtherBytesCount);
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
	APPEND_SCRIPT_MODULE_METHOD(getScript().getModule(), setSpaceLayout, __py_setSpaceLayout, METH_VARARGS, 0);
	APPEND_SCRIPT_MODULE_METHOD(getScript().getModule(), moveSpaceBoundary, __py_moveSpaceBoundary, METH_VARARGS, 0);
	APPEND_SCRIPT_MODULE_METHOD(getScript().getModule(), getSpaceLayout, __py_getSpaceLayout, METH_VARARGS, 0);
	APPEND_SCRIPT_MODULE_METHOD(getScript().getModule(), cellForPosition, __py_cellForPosition, METH_VARARGS, 0);
	APPEND_SCRIPT_MODULE_METHOD(getScript().getModule(), requestCellHandoff, __py_requestCellHandoff, METH_VARARGS, 0);
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
	const bool succeeded = EntityApp<Entity>::run();
	return succeeded && !spacePartitioningRejected_;
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
	if (!disconnectedCellPeers_.empty())
		updateCellPeerConnections();
	if (!pendingCellHandoffs_.empty() || !pendingAutoCellHandoffs_.empty())
		processCellHandoffs();
	if (!outgoingCellHandoffs_.empty() && timestamp() >= nextCellHandoffStartRetry_)
		retryCellHandoffMessages();
	if ((!pendingTeleportDispatches_.empty() || !outgoingTeleports_.empty() ||
		!acceptedCellOnlyTeleports_.empty()) && timestamp() >= nextTeleportRetry_)
		retryTeleports();
	// 等 Space 和坐标节点更新完成后再执行脚本，避免回调中销毁或再次传送。
	if (pWitnessedTimeoutHandler_ && spacePartitioningEnabled())
		pWitnessedTimeoutHandler_->dispatchPendingExits();
}

//-------------------------------------------------------------------------------------
void Cellapp::onRemoveComponent(const Components::ComponentInfos* component)
{
	EntityApp<Entity>::onRemoveComponent(component);
	if (component->componentType == CELLAPP_TYPE)
		teleportPeers_.erase(component->cid);

	// 未激活的布局由 COMPLETE 检查连接。活动布局断线后，只让编号较小的一端重连。
	if (!isShuttingdown() && spacePartitioningConfirmed_ && spacePartitioningEnabled() &&
		component->componentType == CELLAPP_TYPE && component->cid > componentID_ &&
		component->pChannel && component->pIntAddr && component->pExtAddr &&
		(Spaces::hasActiveCellPeer(component->cid) || hasPendingPeerCellHandoff(component->cid)))
		queueCellPeerReconnect(*component);
}

//-------------------------------------------------------------------------------------
void Cellapp::queueCellPeerReconnect(const Components::ComponentInfos& component)
{
	if (disconnectedCellPeers_.empty())
		nextCellPeerAttempt_ = timestamp() + stampsPerSecond();
	auto& peer = disconnectedCellPeers_[component.cid];
	peer = component;
	peer.pChannel = NULL;
	peer.username[MAX_NAME] = '\0';
	peer.externalAddressEx[MAX_NAME] = '\0';
}

//-------------------------------------------------------------------------------------
void Cellapp::updateCellPeerConnections()
{
	if (isShuttingdown())
	{
		reconnectingEndpoint_.reset();
		disconnectedCellPeers_.clear();
		return;
	}

	auto& components = Components::getSingleton();
	const uint64 now = timestamp();
	if (!reconnectingEndpoint_)
	{
		if (now < nextCellPeerAttempt_)
			return;

		const auto nextPeer = disconnectedCellPeers_.upper_bound(reconnectingPeer_);
		const auto next = nextPeer != disconnectedCellPeers_.end() ? nextPeer : disconnectedCellPeers_.begin();
		reconnectingPeer_ = next->first;
		if (!canReconnectCellPeer(components.findComponent(reconnectingPeer_), next->second) ||
			(!Spaces::hasActiveCellPeer(reconnectingPeer_) && !hasPendingPeerCellHandoff(reconnectingPeer_)))
		{
			disconnectedCellPeers_.erase(next);
			return;
		}

		// 每个 CellApp 同时只尝试一条连接；连不上不能阻塞 Tick 或挤占其他节点的重试。
		nextCellPeerAttempt_ = now + stampsPerSecond();
		reconnectingEndpoint_.reset(Network::EndPoint::createPoolObject(OBJECTPOOL_POINT));
		auto& endpoint = *reconnectingEndpoint_;
		endpoint.socket(SOCK_STREAM);
		if (!endpoint.good() || endpoint.setnonblocking(true) != 0)
		{
			reconnectingEndpoint_.reset();
			return;
		}
		endpoint.addr(*next->second.pIntAddr);
		endpoint.setnodelay(true);
		if (endpoint.connect(false) != 0)
		{
			const int error = kbe_lasterror();
#if KBE_PLATFORM == PLATFORM_WIN32
			const bool pending = error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
#else
			const bool pending = error == EINPROGRESS || error == EINTR;
#endif
			if (!pending)
			{
				reconnectingEndpoint_.reset();
				return;
			}
		}
		reconnectDeadline_ = now + 2 * stampsPerSecond();
	}

	auto& endpoint = *reconnectingEndpoint_;
	if (endpoint.getremoteaddress(NULL, NULL) != 0)
	{
		int error = 0;
		socklen_t length = sizeof(error);
		if (now >= reconnectDeadline_ ||
			getsockopt(endpoint.socket(), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length) != 0 ||
			error != 0)
			reconnectingEndpoint_.reset();
		return;
	}

	const auto entry = disconnectedCellPeers_.find(reconnectingPeer_);
	const auto& peer = entry->second;
	auto* route = components.findComponent(reconnectingPeer_);
	if (!canReconnectCellPeer(route, peer) ||
		(!Spaces::hasActiveCellPeer(reconnectingPeer_) && !hasPendingPeerCellHandoff(reconnectingPeer_)))
	{
		reconnectingEndpoint_.reset();
		disconnectedCellPeers_.erase(entry);
		return;
	}
	// 有新的 COMPLETE 更新地址时，不能把已经拨通的旧端点注册为新路由。
	if (!(endpoint.addr() == *peer.pIntAddr))
	{
		reconnectingEndpoint_.reset();
		return;
	}

	const bool newRoute = route == NULL;
	if (newRoute)
	{
		std::string externalAddress(peer.externalAddressEx);
		components.addComponent(peer.uid, peer.username, CELLAPP_TYPE, peer.cid, peer.globalOrderid,
			peer.groupOrderid, peer.gus, peer.pIntAddr->ip, peer.pIntAddr->port, peer.pExtAddr->ip,
			peer.pExtAddr->port, externalAddress, peer.pid, peer.cpu, peer.mem, peer.usedmem,
			peer.extradata, peer.extradata1, peer.extradata2, peer.extradata3);
		route = components.findComponent(CELLAPP_TYPE, peer.uid, peer.cid);
	}
	if (route && components.registerConnectedComponent(*route, reconnectingEndpoint_.release(),
		reconnectingPeer_, false) == 0)
	{
		INFO_MSG(fmt::format("Cellapp::updateCellPeerConnections: reconnected CellApp {}.\n", reconnectingPeer_));
		disconnectedCellPeers_.erase(entry);
	}
	else
	{
		reconnectingEndpoint_.reset();
		if (newRoute && route)
			components.delComponent(peer.uid, CELLAPP_TYPE, peer.cid, false, false);
	}
}

//-------------------------------------------------------------------------------------
bool Cellapp::initializeBegin()
{
	spacePartitioningMode_ = SpacePartitioning::modeFlags(
		g_serverConfig.getCellAppMgr().cellappmgr_space_partitioning_enabled,
		g_serverConfig.getCellApp().use_coordinate_system,
		g_serverConfig.getCellApp().coordinateSystem_hasY);
	const auto& config = g_serverConfig.getCellApp();
	if (spacePartitioningEnabled() && (!std::isfinite(config.ghostDistance) ||
		config.ghostDistance <= 0.f || config.ghostDistance < config.defaultViewRadius + config.defaultViewHysteresisArea ||
		config.ghostUpdateHertz == 0 || config.ghostingMaxPerCheck == 0))
	{
		ERROR_MSG("Cellapp::initializeBegin: invalid boundary Ghost configuration.\n");
		return false;
	}
	INFO_MSG(fmt::format("Cellapp::initializeBegin: spacePartitioning.enabled={}.\n",
		spacePartitioningEnabled()));
	return true;
}

//-------------------------------------------------------------------------------------
bool Cellapp::spacePartitioningEnabled() const
{
	return (spacePartitioningMode_ & SpacePartitioning::ENABLED) != 0;
}

//-------------------------------------------------------------------------------------
void Cellapp::onBoundaryGhost(Network::Channel* pChannel, MemoryStream& stream)
{
	auto* source = spacePartitioningEnabled() ? findBoundCellappSource(pChannel) : NULL;
	SPACE_ID id = 0;
	BoundedStreamReader reader(stream);
	if (source && reader.read(id))
	{
		auto* space = Spaces::findSpace(id);
		if (space && (space->canAcceptEntities() || space->canReceiveSplitGhosts()) &&
			(space->layoutState() == SpaceLayout::ACTIVE || space->layoutState() == SpaceLayout::COMMITTING))
		{
			stream.rpos(stream.rpos() + sizeof(id));
			space->boundaryGhosts().receive(*space, source->cid, *pChannel, stream);
		}
	}
	stream.done();
}

//-------------------------------------------------------------------------------------
bool Cellapp::confirmSpacePartitioningMode(Network::Channel* pChannel)
{
	if (spacePartitioningConfirmed_)
		return true;
	if (spacePartitioningRejected_)
		return false;

	if (spacePartitioningRequestTime_ == 0)
	{
		// 复用已注册的内部连接，不通过额外端口或发现消息确认运行模式。
		Network::Bundle* bundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
		bundle->newMessage(CellappmgrInterface::reportSpacePartitioningMode);
		const auto identity = spacePartitioningEnabled() ? CellRecovery::ProcessIdentity::current() :
			CellRecovery::ProcessIdentity{};
		CellappmgrInterface::reportSpacePartitioningModeArgs6::staticAddToBundle(*bundle,
			componentID_, spacePartitioningMode_, identity.pid, identity.started, identity.pidNamespace,
			spacePartitioningEnabled() ? CellRecovery::recoveryProcessID() : 0);
		spacePartitioningRequestTime_ = timestamp();
		pChannel->send(bundle);
	}
	else if (timestamp() - spacePartitioningRequestTime_ > stampsPerSecond() * 30)
	{
		ERROR_MSG("Cellapp::confirmSpacePartitioningMode: manager confirmation timed out.\n");
		spacePartitioningRejected_ = true;
		dispatcher().breakProcessing();
	}

	return false;
}

//-------------------------------------------------------------------------------------
void Cellapp::onSpacePartitioningMode(Network::Channel* pChannel, COMPONENT_ID managerID,
	uint8 mode, bool accepted, bool autoHandoff, float handoffMargin)
{
	Components& components = Components::getSingleton();
	Components::ComponentInfos* source = components.findComponent(CELLAPPMGR_TYPE, managerID);
	if (pChannel == NULL || pChannel->isExternal() || pChannel->isDestroyed() ||
		pChannel->condemn() != 0 || spacePartitioningRequestTime_ == 0 ||
		!Security::isBoundBidirectionalComponentSource(managerID, source, pChannel, pChannel->componentID()))
	{
		WARNING_MSG("Cellapp::onSpacePartitioningMode: rejected unbound manager confirmation.\n");
		return;
	}

	const bool automatic = spacePartitioningEnabled() && autoHandoff;
	if (!accepted || !SpacePartitioning::compatible(spacePartitioningMode_, mode) ||
		!SpacePartitioning::validHandoffMargin(automatic, handoffMargin, g_serverConfig.getCellApp().ghostDistance))
	{
		ERROR_MSG(fmt::format(
			"Cellapp::onSpacePartitioningMode: incompatible manager, localMode={}, remoteMode={}, handoffMargin={}, ghostDistance={}.\n",
			spacePartitioningMode_, mode, handoffMargin, g_serverConfig.getCellApp().ghostDistance));
		spacePartitioningRejected_ = true;
		spacePartitioningConfirmed_ = false;
		dispatcher().breakProcessing();
		return;
	}

	autoCellHandoffEnabled_ = automatic;
	cellHandoffMargin_ = handoffMargin;
	spacePartitioningConfirmed_ = true;
	INFO_MSG(fmt::format("Cellapp::onSpacePartitioningMode: confirmed manager({}), enabled={}, autoHandoff={}, handoffMargin={}.\n",
		managerID, spacePartitioningEnabled(), autoCellHandoffEnabled_, cellHandoffMargin_));
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
	reconnectingEndpoint_.reset();
	disconnectedCellPeers_.clear();
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
		CellappmgrInterface::updateCellappArgs6::staticAddToBundle((*pBundle),
			componentID_, (ENTITY_ID)pEntities_->getEntities().size(), getLoad(), flags_,
			Witness::activeCount(), Witness::globalPendingSnapshot());

		pChannel->send(pBundle);
	}
}

//-------------------------------------------------------------------------------------
PyObject* Cellapp::__py_createEntity(PyObject* self, PyObject* args)
{
	PyObject* params = NULL;
	char* entityType = NULL;
	SPACE_ID spaceID = 0;
	PyObject* position = NULL;
	PyObject* direction = NULL;
	
	// spaceID、position和direction是创建Cell实体的必要上下文；params保持可选。
	// spaceID, position, and direction are required Cell creation context; params remains optional.
	if(!PyArg_ParseTuple(args, "sIOO|O:createEntity", &entityType, &spaceID, &position, &direction, &params))
		return NULL;
	
	if(entityType[0] == '\0')
	{
		PyErr_SetString(PyExc_ValueError, "KBEngine.createEntity: entityType must not be empty");
		return NULL;
	}

	Space* space = Spaces::findSpace(spaceID);
	if(space == NULL || !space->isGood())
	{
		PyErr_Format(PyExc_ValueError, "KBEngine.createEntity: spaceID %u was not found", spaceID);
		return NULL;
	}

	if (!space->canAcceptNewEntities())
	{
		PyErr_Format(PyExc_RuntimeError, "KBEngine.createEntity: spaceID %u is %s", spaceID,
			space->isDrainingCell() ? "retiring" : "closing");
		return NULL;
	}
	
	if(Cellapp::getSingleton().isShuttingdown())
	{
		// 运行状态拒绝不是参数类型错误，并且异常必须保留给Python调用方。
		// A runtime-state rejection is not a type error, and the exception must remain visible to Python.
		PyErr_Format(PyExc_RuntimeError,
			"KBEngine.createEntity: cellapp is shutting down, entityType=%s", entityType);
		return NULL;
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
	else if(!PyErr_Occurred())
	{
		// 旧的底层创建路径可能记录并清除具体异常；Python C API返回NULL时仍必须存在异常。
		// Legacy creation paths may log and clear the specific error; the Python C API still requires an exception with NULL.
		PyErr_Format(PyExc_RuntimeError,
			"KBEngine.createEntity: failed to create entityType=%s", entityType);
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

void Cellapp::reqRecoveryCellBackup(Network::Channel* channel, MemoryStream& stream)
{
	BoundedStreamReader reader(stream);
	ENTITY_ID id = 0;
	CellRecovery::BackupStamp stamp;
	CellRecovery::BackupDigest knownDigest{};
	uint8 hasBackup = 0;
	const bool parsed = reader.read(id) && id > 0 && stamp.read(reader) &&
		reader.read(hasBackup) && hasBackup <= 1 &&
		(!hasBackup || CellRecovery::readBackupDigest(reader, knownDigest)) && reader.empty();
	stream.done();
	if (!parsed || !spacePartitioningEnabled() || !channel || !channel->isInternal() ||
		channel->isDestroyed() || channel->condemn() != 0)
		return;

	EntityPtr entity(findEntity(id));
	if (!entity || !entity->baseEntityCall())
		return;
	const auto base = entity->baseEntityCall()->componentID();
	auto* peer = Components::getSingleton().findComponent(BASEAPP_TYPE, base);
	if (!Security::isBoundBidirectionalComponentSource(base, peer, channel, channel->componentID()))
		return;

	const auto current = [&]()
	{
		return !entity->isDestroyed() && entity->isReal() && entity->spaceID() == stamp.space &&
			!entity->hasFlags(ENTITY_FLAGS_TELEPORT_START);
	};
	if (!current())
		return;
	const auto epoch = entity->routingEpoch();
	auto data = MemoryStream::createSmartPoolObj(OBJECTPOOL_POINT);
	try
	{
		entity->addCellDataToStream(BASEAPP_TYPE, ENTITY_CELL_DATA_FLAGS, data->get());
	}
	catch (const MemoryStreamWriteOverflow&)
	{
		return;
	}
	if (!current() || entity->routingEpoch() != epoch)
		return;

	// 和 Base 已收到的摘要比较；普通存档写过的摘要不代表 Base 持有同一份恢复数据。
	CellRecovery::BackupDigest digest;
	KBE_SHA1 sha;
	sha.Input(data->get()->data(), data->get()->length());
	sha.Result(digest.data());
	const bool full = !hasBackup || digest != knownDigest;
	auto* bundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
	bundle->newMessage(BaseappInterface::onRecoveryCellBackup);
	(*bundle) << id;
	stamp.write(*bundle);
	(*bundle) << static_cast<uint8>(full);
	if (full)
	{
		CellRecovery::writeBackupDigest(*bundle, digest);
		bundle->append(data->get());
	}
	// 回包沿原连接发送，Base 不会接受同编号的另一条连接送来的回包。
	channel->send(bundle);
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
			if (!bindClientEntityCallForCreatedEntity(e,
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

		// BaseApp must initialize the client's cell properties before the Witness sends EnterWorld.
		// 必须先通知 BaseApp 初始化客户端 Cell 属性，再由 Witness 发送 EnterWorld，确保初始化顺序稳定。
		Network::Bundle* pBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
		(*pBundle).newMessage(BaseappInterface::onEntityGetCell);
		BaseappInterface::onEntityGetCellArgs3::staticAddToBundle((*pBundle), entitycallEntityID, componentID_, spaceID);
		cinfos->pChannel->send(pBundle);

		if (hasClient)
		{
			// Re-send the authoritative post-__init__ Cell snapshot before Witness emits EnterWorld.
			// This preserves 2.5's initialization semantics without relying on internal Channel timing.
			// Witness 发送 EnterWorld 前重发 __init__ 后的 Cell 权威快照。
			// 这既保留 2.5 的初始化语义，又不再依赖内部 Channel 时序。
			e->onGetWitness(true);
		}
		else
		{
			space->onEnterWorld(e);
		}

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
			if (!bindClientEntityCallForCreatedEntity(e,
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

		// Keep the BaseApp acknowledgement ahead of restoration's Witness EnterWorld message.
		// 恢复流程同样必须让 BaseApp 先排入客户端初始化属性，避免 EnterWorld 抢先到达。
		Network::Bundle* pBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
		(*pBundle).newMessage(BaseappInterface::onEntityGetCell);
		BaseappInterface::onEntityGetCellArgs3::staticAddToBundle((*pBundle), entitycallEntityID, componentID_, spaceID);
		cinfos->pChannel->send(pBundle);

		if (hasClient)
			e->setWitness(Witness::createPoolObject(OBJECTPOOL_POINT));

		space->addEntityAndEnterWorld(e, true);
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
		// 延迟处理使用独立副本；原始网络消息必须在当前 ingress 内完整消费。
		// Deferred processing owns an independent copy; consume the original network message in this ingress.
		s.done();

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
	// Helper 的生命周期失败分支可能无需读取 cellData，但 PacketReader 仍要求变量消息完整消费。
	// Helper lifecycle failures may not need cellData, while PacketReader still requires full variable-message consumption.
	s.done();

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
		// Space destruction may race an already accepted BaseApp create request. The
		// explicit failure reply completes the protocol, so this is a controlled lifecycle race.
		// Space 销毁可能与已经接收的 BaseApp 创建请求并发；显式失败回包会闭合协议，因此属于受控生命周期竞态。
		WARNING_MSG("Cellapp::_onCreateCellEntityFromBaseapp: target space entity was destroyed before creation.\n");

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
	if(space != NULL && space->canAcceptNewEntities())
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
			if (!bindClientEntityCallForCreatedEntity(e,
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
			e->onGetWitness(true);
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
		const bool sourceBoundToChannel = sourceEntity != NULL &&
			sourceEntity->baseEntityCall() != NULL &&
			sourceEntity->baseEntityCall()->componentID() == sourceComponent->cid;
		const bool migrationSelfCall = sourceEntity == e && srcEntityID == targetID &&
			!sourceEntity->isReal();
		const bool sourceCanAuthorize = sourceBoundToChannel &&
			(sourceEntity->isReal() || migrationSelfCall);

		const bool controlledBySource = e->controlledBy() != NULL &&
			e->controlledBy()->id() == srcEntityID;
		const bool inSourceView = sourceEntity != NULL && sourceEntity->pWitness() != NULL &&
			sourceEntity->pWitness()->entityInView(targetID);

		// BaseApp may still have self-RPC bytes queued on the old CellApp Channel
		// while migration switches the authoritative route. The old Source Ghost
		// remains bound to the same Base EntityCall and may forward only that
		// entity's self call; cross-entity authorization still requires a real Source.
		// BaseApp 切换权威路由时，旧 CellApp Channel 中可能仍有已排队的 self RPC。
		// 旧 Source Ghost 仍绑定同一个 Base EntityCall，因此只允许转发该实体的
		// 自调用；跨实体授权仍必须由 real Source 执行。
		if (!sourceCanAuthorize ||
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
		sourceComponent->cid, e->ghostCell(), e->migrationRelayCell()))
	{
		WARNING_MSG(fmt::format("Cellapp::onRemoteCallMethodFromClient: rejected stale CellApp relay, "
			"srcEntityID={}, targetID={}, sourceCellappID={}, targetGhostCellID={}, "
			"migrationRelayCellID={}, targetIsReal={}.\n",
			srcEntityID, targetID, sourceComponent->cid, e->ghostCell(),
			e->migrationRelayCell(), e->isReal()));
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
				// Bundle::append copies bytes without advancing the source stream. Consume
				// the ingress message so PacketReader does not treat successful forwarding
				// as a malformed partial read. / Bundle::append 只复制字节，不推进源流；
				// 转发成功后必须消费入口消息，避免 PacketReader 误报部分读取。
				s.done();
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
	if (!Components::getSingleton().isExpectedComponentChannel(BASEAPP_TYPE, pChannel))
	{
		WARNING_MSG(fmt::format(
			"Cellapp::onUpdateDataFromClient: rejected non-BaseApp source, addr={}.\n",
			pChannel != NULL ? pChannel->c_str() : "none"));
		s.done();
		return;
	}

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
		// Movement is volatile and cannot be replayed after the BaseApp route changes.
		// A missing target from an authenticated BaseApp is therefore a migration/lifecycle
		// race, not a per-packet warning. / 移动数据不可重放；认证 BaseApp 发来的缺失目标
		// 属于迁移或生命周期竞态，不应逐包放大为 WARNING。
		++staleClientInputEntityDrops_;
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
	if (!Components::getSingleton().isExpectedComponentChannel(BASEAPP_TYPE, pChannel))
	{
		WARNING_MSG(fmt::format(
			"Cellapp::onUpdateDataFromClientForControlledEntity: rejected non-BaseApp source, addr={}.\n",
			pChannel != NULL ? pChannel->c_str() : "none"));
		s.done();
		return;
	}

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
		++staleControlledClientInputEntityDrops_;
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
		const COMPONENT_ID targetCell = gm ? gm->getRoute(entityID) : 0;
		if (forwardRemoteRealMethodCall(gm, entityID, sourceInfos->cid, targetCell, s))
		{
			return;
		}

		// A route can expire after the final real Entity is destroyed while an older
		// Ghost still has a queued call. This is a terminal lifecycle race, not a
		// process integrity failure. / 最终 real Entity 销毁后，旧 Ghost 仍可能有
		// 已排队调用；路由已失效属于终态生命周期竞态，不是进程完整性错误。
		WARNING_MSG(fmt::format("Cellapp::onRemoteRealMethodCall: not found entity({})\n",
			entityID));

		s.done();
		return;
	}

	if (!entity->isReal())
	{
		GhostManager* gm = Cellapp::getSingleton().pGhostManager();
		if (forwardRemoteRealMethodCall(gm, entityID, sourceInfos->cid, entity->realCell(), s))
		{
			return;
		}

		WARNING_MSG(fmt::format("Cellapp::onRemoteRealMethodCall: rejected invalid Ghost route, "
			"entityID={}, sourceCellID={}, targetCellID={}.\n",
			entityID, sourceInfos->cid, entity->realCell()));
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
void Cellapp::reqSetFlags(Network::Channel* pChannel, MemoryStream& s)
{
	if(pChannel->isExternal())
		return;

	uint32 flags = APP_FLAGS_NONE;
	s >> flags;
	if (!validateManagementAdminToken(pChannel, s, "reqSetFlags"))
	{
		Network::Bundle* pBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
		(*pBundle) << false;
		pChannel->send(pBundle);
		return;
	}

	const uint32 allowedFlags = APP_FLAGS_NOT_PARTCIPATING_LOAD_BALANCING;
	bool success = (flags & ~allowedFlags) == 0;
	if(success)
		this->flags(flags);
	else
		WARNING_MSG(fmt::format("Cellapp::reqSetFlags: reject unsupported flags=0x{:08x}.\n", flags));

	Network::Bundle* pBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
	(*pBundle) << success;
	pChannel->send(pBundle);
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
	// 无参数默认只刷新行为层，完整定义重建必须显式传 True。
	// No-argument reload updates behavior only; rebuilding definitions requires an explicit True.
	bool fullReload = false;
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

	// onReload 必须在 Entity、EntityCall 和 EntityComponent 都切换完成后触发。
	// onReload runs only after Entity, EntityCall, and EntityComponent have all switched.
	eiter = entities.begin();
	for(; eiter != entities.end(); ++eiter)
		callEntityOnReload(static_cast<Entity*>(eiter->second.get()), fullReload);
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
PyObject* Cellapp::__py_requestCellHandoff(PyObject* self, PyObject* args)
{
	ENTITY_ID id = 0;
	if (!PyArg_ParseTuple(args, "i:requestCellHandoff", &id))
		return NULL;
	auto& app = getSingleton();
	if (!app.spacePartitioningEnabled() || !app.spacePartitioningConfirmed() || app.isShuttingdown())
	{
		PyErr_SetString(PyExc_RuntimeError, "Space partitioning is not available.");
		return NULL;
	}
	auto* entity = id > 0 ? app.findEntity(id) : NULL;
	if (!entity || entity->isDestroyed() || !entity->isReal() || entity->isSpace() ||
		!entity->baseEntityCall() || entity->hasFlags(ENTITY_FLAGS_TELEPORT_START))
	{
		PyErr_SetString(PyExc_ValueError, "Cell handoff requires a local Real with a BaseApp.");
		return NULL;
	}
	// 脚本可在 AOI 回调中请求交接，不能在坐标系统正在遍历节点时改动实体。
	if (app.pendingCellHandoffs_.size() >= SpaceLayout::MAX_CELLS)
	{
		PyErr_SetString(PyExc_RuntimeError, "Too many Cell handoff requests for this tick.");
		return NULL;
	}
	return PyBool_FromLong(app.pendingCellHandoffs_.insert(id).second);
}

namespace
{
bool canSendCellHandoff(Network::Channel* channel)
{
	return channel && !channel->isDestroyed() && channel->condemn() == 0;
}

void delayAutoCellHandoff(Entity& entity)
{
	entity.autoHandoffRetryAt(timestamp() + stampsPerSecond());
}
}

//-------------------------------------------------------------------------------------
void Cellapp::processCellHandoffs()
{
	std::set<ENTITY_ID> batch;
	batch.swap(pendingCellHandoffs_);
	for (ENTITY_ID id : batch)
	{
		pendingAutoCellHandoffs_.erase(id);
		if (!beginCellHandoff(id, false))
			WARNING_MSG(fmt::format("Cellapp::beginCellHandoff: skipped entity={}.\n", id));
	}

	for (ENTITY_ID id : pendingAutoCellHandoffs_)
	{
		if (outgoingCellHandoffs_.size() >= SpaceLayout::MAX_CELLS)
			break;
		if (!beginCellHandoff(id, true))
		{
			// 候选条件可能在扫描后改变，失败的实体先让出后续 Tick 的名额。
			if (auto* entity = findEntity(id))
				delayAutoCellHandoff(*entity);
		}
	}
	pendingAutoCellHandoffs_.clear();
}

//-------------------------------------------------------------------------------------
void Cellapp::considerAutoCellHandoff(Space& space, Entity& entity)
{
	// 快照和重试都需要占用资源，每个 Tick 只发起少量自动交接。
	constexpr size_t maxStartsPerTick = 8;
	if (pendingAutoCellHandoffs_.size() >= maxStartsPerTick ||
		pendingAutoCellHandoffs_.size() + outgoingCellHandoffs_.size() >= SpaceLayout::MAX_CELLS ||
		isShuttingdown() || space.isCellMergeHandoffStopped() || !entity.isReal() || entity.isDestroyed() || entity.isSpace() ||
		entity.spaceID() != space.id() ||
		entity.hasFlags(ENTITY_FLAGS_TELEPORT_START) ||
		(entity.controlledBy() && entity.controlledBy()->id() != entity.id()))
		return;

	const auto& pos = entity.position();
	if ((!space.isDrainingCell() && !space.localCellBounds().crossedBy(pos.x, pos.z, cellHandoffMargin_)) ||
		outgoingCellHandoffs_.count(entity.id()) ||
		(entity.autoHandoffRetryAt() != 0 && timestamp() < entity.autoHandoffRetryAt()))
		return;

	const auto* destination = space.handoffDestination(pos.x, pos.z, componentID_);
	if (!destination || destination->cellappID == componentID_)
		return;
	auto* target = Components::getSingleton().findComponentChannel(CELLAPP_TYPE, destination->cellappID);
	auto* baseCall = entity.baseEntityCall();
	if (canSendCellHandoff(target) &&
		space.boundaryGhosts().handoffCopyGeneration(entity.id(), destination->cellappID, target->sessionEpoch()) &&
		(baseCall ? canSendCellHandoff(baseCall->getChannel()) : queryCellPeerProcess(*target) != 0))
		pendingAutoCellHandoffs_.insert(entity.id());
}

//-------------------------------------------------------------------------------------
namespace
{
void sendCellHandoff(Network::Channel* base, ENTITY_ID id, CellHandoff::Action action,
	const CellHandoff::Route& route)
{
	if (!canSendCellHandoff(base))
		return;
	auto* bundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
	bundle->newMessage(BaseappInterface::onCellHandoff);
	(*bundle) << id << static_cast<uint8>(action);
	route.write(*bundle);
	base->send(bundle);
}

void sendPeerCellHandoff(Network::Channel& peer, ENTITY_ID id, CellHandoff::Action action,
	const CellHandoff::Route& route, const CellHandoff::PeerProcesses& processes)
{
	auto* bundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
	bundle->newMessage(CellappInterface::onCellHandoff);
	(*bundle) << id << static_cast<uint8>(action);
	route.write(*bundle);
	processes.write(*bundle);
	peer.send(bundle);
}
}

bool Cellapp::beginCellHandoff(ENTITY_ID id, bool automatic)
{
	auto* entity = findEntity(id);
	if (isShuttingdown() || !entity || entity->isDestroyed() || !entity->isReal() ||
		entity->isSpace() || entity->hasFlags(ENTITY_FLAGS_TELEPORT_START) ||
		(!automatic && !entity->baseEntityCall()) || (entity->controlledBy() && entity->controlledBy()->id() != id))
		return false;
	auto* space = Spaces::findSpace(entity->spaceID());
	if (!space || !space->hasManagedCell() || !space->canAcceptEntities() || space->isCellMergeHandoffStopped() ||
		space->layoutState() != SpaceLayout::ACTIVE)
		return false;
	const Position3D pos = entity->position();
	if (automatic && (!autoCellHandoffEnabled() ||
		(!space->isDrainingCell() && !space->localCellBounds().crossedBy(pos.x, pos.z, cellHandoffMargin_))))
		return false;
	const auto* destination = space->handoffDestination(pos.x, pos.z, componentID_);
	if (!destination || destination->cellappID == componentID_ || outgoingCellHandoffs_.count(id))
		return false;

	auto* peer = Components::getSingleton().findComponent(CELLAPP_TYPE, destination->cellappID);
	auto* target = peer ? peer->pChannel : NULL;
	auto* baseCall = entity->baseEntityCall();
	auto* base = baseCall ? baseCall->getChannel() : NULL;
	if (!canSendCellHandoff(target) || (baseCall && !canSendCellHandoff(base)) ||
		!space->boundaryGhosts().handoffCopyGeneration(id, destination->cellappID, target->sessionEpoch()))
		return false;
	const uint64 targetProcess = baseCall ? 0 : queryCellPeerProcess(*target);
	if (!baseCall && !targetProcess)
		return false;

	const uint64 epoch = entity->reserveRoutingEpoch();
	if (!epoch)
		return false;
	auto pending = std::make_unique<OutgoingCellHandoff>();
	pending->route = {componentID_, peer->cid, epoch, static_cast<uint32>(getProcessPID()), 0,
		space->id(), space->layoutVersion()};
	pending->base = baseCall ? baseCall->componentID() : 0;
	if (!baseCall)
		pending->processes = {CellRecovery::recoveryProcessID(), targetProcess};
	pending->session = target->sessionEpoch();
	pending->automatic = automatic;
	auto& entry = *outgoingCellHandoffs_.emplace(id, std::move(pending)).first->second;
	entity->addFlags(ENTITY_FLAGS_TELEPORT_START);
	if (baseCall)
	{
		// BaseApp 记录本轮交接后再发送快照，等待期间源仍是 Real。
		sendCellHandoff(base, id, CellHandoff::START, entry.route);
	}
	else
	{
		EntityPtr held(entity);
		Entity::bufferCallback(true);
		dispatchCellHandoff(*entity, entry);
		if (entry.unsent)
		{
			auto restored = std::move(outgoingCellHandoffs_.at(id));
			outgoingCellHandoffs_.erase(id);
			restoreCellHandoffSource(*entity, *restored);
		}
		Entity::bufferCallback(false);
	}
	return true;
}

//-------------------------------------------------------------------------------------
void Cellapp::dispatchCellHandoff(Entity& entity, OutgoingCellHandoff& pending)
{
	const auto& route = pending.route;
	auto* space = Spaces::findSpace(route.space);
	auto* peer = Components::getSingleton().findComponent(CELLAPP_TYPE, route.target);
	auto* target = peer ? peer->pChannel : NULL;
	const auto pos = entity.position();
	const auto dir = entity.direction();
	const auto* destination = space ? space->handoffDestination(pos.x, pos.z, componentID_) : NULL;
	const uint64 generation = space ? space->boundaryGhosts().handoffCopyGeneration(entity.id(), route.target, pending.session) : 0;
	pending.unsent = pending.unsent || isShuttingdown() || !entity.isReal() ||
		entity.spaceID() != route.space ||
		(entity.controlledBy() && entity.controlledBy()->id() != entity.id()) ||
		!space || !space->canAcceptEntities() || space->layoutState() != SpaceLayout::ACTIVE ||
		space->layoutVersion() != route.layoutVersion || !destination || destination->cellappID != route.target ||
		(pending.automatic && !space->isDrainingCell() && !space->localCellBounds().crossedBy(pos.x, pos.z, cellHandoffMargin_)) ||
		!canSendCellHandoff(target) || (route.targetPid && peer->pid && peer->pid != route.targetPid) ||
		target->sessionEpoch() != pending.session || !generation;
	if (pending.unsent)
		return;

	auto& body = pending.snapshot;
	size_t snapshotOffset;
	try
	{
		const auto peers = space->boundaryGhosts().handoffPeers(entity.id(), route.target);
		snapshotOffset = CellHandoff::writeSnapshotBody(body, entity.id(), entity.pScriptModule()->getUType(),
			pos, dir, pending.base, generation, route, pending.processes, peers,
			[&](MemoryStream& stream) { entity.changeToGhost(route.target, stream, route.epoch, true); });
	}
	catch (const MemoryStreamWriteOverflow&)
	{
		pending.unsent = true;
		MemoryStream empty(0);
		pending.snapshot.swap(empty);
		// 完整交接比 Ghost 快照多出私有属性和运行状态；退役端必须报告失败，
		// 让管理器按原顺序收尾在途交接并恢复接收，而不是反复尝试同一快照。
		if (space->isDrainingCell())
			space->reportCellMergeBlock(entity.id(), "full handoff snapshot exceeds MemoryStream size limit");
		return;
	}
	auto* request = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
	request->newMessage(CellappInterface::reqCellHandoff);
	request->append(body);
	// 回退只读取实体快照，复用发送缓冲，避免再复制整份状态。
	body.rpos(snapshotOffset);
	pending.dispatched = true;
	pending.checkAt = timestamp() + 2 * stampsPerSecond();
	target->send(request);
	DEBUG_MSG(fmt::format("Cellapp::cellHandoff: snapshot sent, entity={}, epoch={}.\n", entity.id(), route.epoch));
}

//-------------------------------------------------------------------------------------
void Cellapp::releaseCellHandoffSource(Entity& entity, const CellHandoff::Route& route, uint64 session)
{
	auto* space = Spaces::findSpace(route.space);
	if (space && entity.spaceID() == route.space && space->layoutVersion() == route.layoutVersion)
		space->boundaryGhosts().remove(*space, entity, route.target);
	if (!space || entity.spaceID() != route.space || space->layoutVersion() != route.layoutVersion ||
		!space->canAcceptEntities() || space->layoutState() != SpaceLayout::ACTIVE ||
		!space->boundaryGhosts().retainHandoffSource(*space, entity, session))
		destroyEntity(entity.id(), false);
}

//-------------------------------------------------------------------------------------
void Cellapp::restoreCellHandoffSource(Entity& entity, OutgoingCellHandoff& pending)
{
	if (pending.automatic)
		delayAutoCellHandoff(entity);
	if (pending.dispatched)
	{
		if (auto* space = Spaces::findSpace(pending.route.space))
			space->noteCellMergeRollback(entity.id());
		entity.changeToReal(0, pending.snapshot, true);
		entity.restoreProximitys();
	}
	entity.removeFlags(ENTITY_FLAGS_TELEPORT_START);
	entity.reconcileCellHandoffWitness();
}

//-------------------------------------------------------------------------------------
void Cellapp::retryCellHandoffMessages()
{
	nextCellHandoffStartRetry_ = timestamp() + stampsPerSecond();
	for (auto it = outgoingCellHandoffs_.begin(); it != outgoingCellHandoffs_.end();)
	{
		const auto& pending = *it->second;
		if (!pending.base)
		{
			auto* peer = Components::getSingleton().findComponentChannel(CELLAPP_TYPE, pending.route.target);
			if (timestamp() >= pending.checkAt && canSendCellHandoff(peer) &&
				queryCellPeerProcess(*peer) == pending.processes.target)
			{
				sendPeerCellHandoff(*peer, it->first, pending.released ? CellHandoff::SOURCE_RELEASED :
					CellHandoff::CHECK_TARGET, pending.route, pending.processes);
			}
			++it;
			continue;
		}
		auto* entity = findEntity(it->first);
		if (!entity)
		{
			it = outgoingCellHandoffs_.erase(it);
			continue;
		}
		if (!pending.route.targetPid && entity->baseEntityCall())
			sendCellHandoff(entity->baseEntityCall()->getChannel(), it->first, CellHandoff::START, pending.route);
		else if (pending.dispatched && timestamp() >= pending.checkAt)
		{
			auto* peer = Components::getSingleton().findComponent(CELLAPP_TYPE, pending.route.target);
			if (peer && (!peer->pid || peer->pid == pending.route.targetPid) && peer->pChannel &&
				!peer->pChannel->isDestroyed() && peer->pChannel->condemn() == 0)
			{
				auto* check = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
				check->newMessage(CellappInterface::checkCellHandoffSnapshot);
				(*check) << it->first << pending.base;
				pending.route.write(*check);
				peer->pChannel->send(check);
			}
		}
		++it;
	}
}

//-------------------------------------------------------------------------------------
void Cellapp::onCellHandoff(Network::Channel* channel, MemoryStream& stream)
{
	using namespace CellHandoff;
	BoundedStreamReader reader(stream);
	ENTITY_ID id = 0;
	uint8 action = NONE;
	Route route;
	const bool peer = findBoundCellappSource(channel) != NULL;
	PeerProcesses processes;
	const bool parsed = reader.read(id) && id > 0 && reader.read(action) && route.read(reader) &&
		(!peer || processes.read(reader)) && reader.empty();
	stream.done();
	if (!parsed || !spacePartitioningEnabled())
		return;
	if (peer)
	{
		onPeerCellHandoff(*channel, id, action, route, processes);
		return;
	}
	if (!Components::getSingleton().isExpectedComponentChannel(BASEAPP_TYPE, channel))
		return;
	auto* entity = findEntity(id);
	const auto baseID = channel->componentID();
	const auto localPid = static_cast<uint32>(getProcessPID());
	const auto matchesBase = entity && !entity->isDestroyed() && entity->baseEntityCall() &&
		entity->baseEntityCall()->componentID() == baseID;
	if (route.target == componentID_ && action == IDENTIFY_TARGET && route.targetPid == 0)
	{
		// 直接注册的组件表可能没有 PID；由目标在已绑定的连接上确认。
		route.targetPid = localPid;
		sendCellHandoff(channel, id, TARGET_IDENTIFIED, route);
		return;
	}
	if (route.target == componentID_ && route.targetPid == localPid)
	{
		if (!matchesBase || !entity->isReal() || entity->spaceID() != route.space ||
			entity->routingEpoch() != route.epoch ||
			entity->migrationRelayCell() != route.source)
			return;
		auto* space = Spaces::findSpace(route.space);
		if (!space || space->layoutVersion() != route.layoutVersion)
			return;
		if (action == CHECK_TARGET && entity->isCellHandoffTarget() && space->isGood())
			sendCellHandoff(channel, id, TARGET_READY, route);
		else if (action == FINISH_TARGET)
		{
			// 回调内可以排入下一轮，先让 BaseApp 收到本轮完成通知。
			sendCellHandoff(channel, id, TARGET_FINISHED, route);
			if (entity->isCellHandoffTarget())
			{
				admittedCellHandoffs_.erase(id);
				EntityPtr held(entity);
				entity->completeCellHandoff();
			}
		}
		return;
	}
	if (route.source != componentID_ || route.sourcePid != localPid)
		return;
	auto entry = outgoingCellHandoffs_.find(id);
	if (entry == outgoingCellHandoffs_.end())
	{
		// 释放或回退已经执行，但确认可能随连接丢失。只补发确认，不再修改实体。
		if (action == RELEASE_SOURCE && (!entity ||
			(entity->isBoundaryGhost() && entity->realCell() == route.target)))
			sendCellHandoff(channel, id, SOURCE_RELEASED, route);
		else if (action == RESTORE_SOURCE && matchesBase && entity->isReal() && entity->spaceID() == route.space &&
			entity->cellHandoffEpoch() == route.epoch)
			sendCellHandoff(channel, id, SOURCE_RESTORED, route);
		return;
	}
	auto& pending = *entry->second;
	if (!matchesBase || pending.base != baseID || !pending.route.sameAttempt(route))
		return;
	// 目标可能在源收到 DISPATCH 前退出。这时源仍是 Real，可以直接结束这一轮。
	if ((action == DISPATCH || action == RESTORE_SOURCE) && route.targetPid && !pending.route.targetPid)
		pending.route.targetPid = route.targetPid;
	if (!(pending.route == route))
		return;

	if (action == DISPATCH && !pending.dispatched)
	{
		EntityPtr held(entity);
		Entity::bufferCallback(true);
		dispatchCellHandoff(*entity, pending);
		if (pending.unsent)
			sendCellHandoff(channel, id, SOURCE_UNSENT, route);
		Entity::bufferCallback(false);
	}
	else if (action == RELEASE_SOURCE && pending.dispatched &&
		entity->isCellHandoffSource() && entity->realCell() == route.target &&
		entity->routingEpoch() == route.epoch)
	{
		const auto session = pending.session;
		outgoingCellHandoffs_.erase(entry);
		releaseCellHandoffSource(*entity, route, session);
		sendCellHandoff(channel, id, SOURCE_RELEASED, route);
	}
	else if (action == RESTORE_SOURCE &&
		(!pending.dispatched || (entity->isCellHandoffSource() && entity->realCell() == route.target &&
			entity->routingEpoch() == route.epoch)))
	{
		auto* space = Spaces::findSpace(route.space);
		if (!space || entity->spaceID() != route.space || space->layoutVersion() != route.layoutVersion)
			return;
		auto restored = std::move(entry->second);
		outgoingCellHandoffs_.erase(entry);
		EntityPtr held(entity);
		Entity::bufferCallback(true);
		restoreCellHandoffSource(*entity, *restored);
		sendCellHandoff(channel, id, SOURCE_RESTORED, route);
		Entity::bufferCallback(false);
	}
}

//-------------------------------------------------------------------------------------
void Cellapp::onPeerCellHandoff(Network::Channel& channel, ENTITY_ID id, uint8 action,
	const CellHandoff::Route& route, const CellHandoff::PeerProcesses& processes)
{
	using namespace CellHandoff;
	const auto* peer = findBoundCellappSource(&channel);
	if (!peer || peer->pChannel != &channel || route.targetPid != 0)
		return;

	const auto localProcess = CellRecovery::recoveryProcessID();
	if (route.target == componentID_ && route.source == peer->cid && processes.target == localProcess &&
		(!peer->pid || peer->pid == route.sourcePid))
	{
		const auto entry = incomingPeerCellHandoffs_.find(id);
		const bool matches = entry != incomingPeerCellHandoffs_.end() &&
			entry->second.route == route && entry->second.processes == processes;
		if (action == CHECK_TARGET)
		{
			if (matches)
				sendPeerCellHandoff(channel, id, TARGET_INSTALLED, route, processes);
			else
			{
				// 原连接按序处理。换连接后旧通道不能安装快照，同一进程仍保留已安装记录。
				auto* entity = findEntity(id);
				if (!entity || !entity->isReal())
					sendPeerCellHandoff(channel, id, TARGET_REJECTED, route, processes);
			}
		}
		else if (action == SOURCE_RELEASED)
		{
			sendPeerCellHandoff(channel, id, TARGET_FINISHED, route, processes);
			if (matches)
			{
				incomingPeerCellHandoffs_.erase(entry);
				finishPeerCellHandoff(id, route);
			}
		}
		return;
	}

	if (route.source != componentID_ || route.sourcePid != static_cast<uint32>(getProcessPID()) ||
		route.target != peer->cid || processes.source != localProcess)
		return;
	auto entry = outgoingCellHandoffs_.find(id);
	if (entry == outgoingCellHandoffs_.end())
		return;
	auto& pending = *entry->second;
	if (pending.base || !pending.dispatched || !(pending.route == route) || !(pending.processes == processes))
		return;

	auto* entity = findEntity(id);
	const bool ownsSource = entity && !entity->isDestroyed() && entity->isCellHandoffSource() &&
		entity->spaceID() == route.space && entity->realCell() == route.target && entity->routingEpoch() == route.epoch;
	if (action == TARGET_INSTALLED)
	{
		// 先记下已释放，之后的拒绝或迟到回包都不能再把源副本恢复成 Real。
		Entity::bufferCallback(true);
		if (!pending.released)
		{
			pending.released = true;
			MemoryStream empty(0);
			pending.snapshot.swap(empty);
			if (ownsSource)
				releaseCellHandoffSource(*entity, route, pending.session);
		}
		sendPeerCellHandoff(channel, id, SOURCE_RELEASED, route, processes);
		Entity::bufferCallback(false);
	}
	else if (action == TARGET_REJECTED && !pending.released)
	{
		auto restored = std::move(entry->second);
		outgoingCellHandoffs_.erase(entry);
		if (ownsSource)
		{
			EntityPtr held(entity);
			Entity::bufferCallback(true);
			restoreCellHandoffSource(*entity, *restored);
			Entity::bufferCallback(false);
		}
	}
	else if (action == TARGET_FINISHED && pending.released)
		outgoingCellHandoffs_.erase(entry);
}

//-------------------------------------------------------------------------------------
void Cellapp::finishPeerCellHandoff(ENTITY_ID id, const CellHandoff::Route& route)
{
	auto* entity = findEntity(id);
	if (entity && !entity->isDestroyed() && entity->isReal() && entity->isCellHandoffTarget() &&
		!entity->baseEntityCall() && entity->spaceID() == route.space && entity->routingEpoch() == route.epoch &&
		entity->migrationRelayCell() == route.source)
	{
		EntityPtr held(entity);
		entity->completeCellHandoff();
	}
}

//-------------------------------------------------------------------------------------
bool Cellapp::hasUnreleasedCellHandoff(SPACE_ID space, uint64 layoutVersion, COMPONENT_ID target) const
{
	return std::any_of(outgoingCellHandoffs_.begin(), outgoingCellHandoffs_.end(), [&](const auto& entry)
	{
		const auto& pending = *entry.second;
		return !pending.released && pending.route.space == space &&
			pending.route.layoutVersion == layoutVersion && (!target || pending.route.target == target);
	});
}

//-------------------------------------------------------------------------------------
bool Cellapp::hasUnfinishedIncomingCellHandoff(SPACE_ID space, uint64 layoutVersion, COMPONENT_ID source)
{
	const auto isWaiting = [&](ENTITY_ID id)
	{
		const auto* entity = findEntity(id);
		return entity && !entity->isDestroyed() && entity->isCellHandoffTarget() &&
			entity->spaceID() == space && entity->migrationRelayCell() == source;
	};
	// 接收记录可能比实体存活得久，不能仅凭记录判断目标仍在等待交接。
	return std::any_of(admittedCellHandoffs_.begin(), admittedCellHandoffs_.end(), isWaiting) ||
		std::any_of(incomingPeerCellHandoffs_.begin(), incomingPeerCellHandoffs_.end(), [&](const auto& entry)
		{
			const auto& route = entry.second.route;
			return route.space == space && route.layoutVersion == layoutVersion && route.source == source &&
				isWaiting(entry.first);
		});
}

//-------------------------------------------------------------------------------------
bool Cellapp::hasPendingPeerCellHandoff(COMPONENT_ID peer) const
{
	return std::any_of(outgoingCellHandoffs_.begin(), outgoingCellHandoffs_.end(),
		[peer](const auto& entry) { return !entry.second->base && entry.second->route.target == peer; }) ||
		std::any_of(incomingPeerCellHandoffs_.begin(), incomingPeerCellHandoffs_.end(),
		[peer](const auto& entry) { return entry.second.route.source == peer; });
}

//-------------------------------------------------------------------------------------
void Cellapp::onCellProcessExit(Network::Channel* channel, COMPONENT_ID cid, uint64 process)
{
	if (!spacePartitioningEnabled() || isShuttingdown() || !cid || !process ||
		!canSendCellHandoff(channel) || channel->isExternal() ||
		channel != Components::getSingleton().getCellappmgrChannel())
		return;
	resolvePeerCellHandoffsAfterExit(cid, process);
}

//-------------------------------------------------------------------------------------
void Cellapp::resolvePeerCellHandoffsAfterExit(COMPONENT_ID lost, uint64 process, SPACE_ID space, uint64 version)
{
	const auto matches = [&](const CellHandoff::Route& route, uint64 peerProcess)
	{
		// 进程退出通知按进程编号处理；漏收通知时，接管检查可清理旧布局的记录。
		return process ? peerProcess == process : route.space == space && route.layoutVersion <= version;
	};
	// 管理器已确认进程退出。没有可用备份时，源不能恢复可能已在目标执行过的快照。
	Entity::bufferCallback(true);
	for (auto it = outgoingCellHandoffs_.begin(); it != outgoingCellHandoffs_.end();)
	{
		const auto& pending = *it->second;
		if (pending.base || pending.route.target != lost || !matches(pending.route, pending.processes.target))
		{
			++it;
			continue;
		}
		const auto id = it->first;
		const auto route = pending.route;
		it = outgoingCellHandoffs_.erase(it);
		auto* entity = findEntity(id);
		if (entity && entity->isCellHandoffSource() && entity->spaceID() == route.space &&
			entity->routingEpoch() == route.epoch && entity->realCell() == lost)
			destroyEntity(id, false);
	}
	for (auto it = incomingPeerCellHandoffs_.begin(); it != incomingPeerCellHandoffs_.end();)
	{
		const auto route = it->second.route;
		if (route.source != lost || !matches(route, it->second.processes.source))
		{
			++it;
			continue;
		}
		const auto id = it->first;
		it = incomingPeerCellHandoffs_.erase(it);
		finishPeerCellHandoff(id, route);
	}
	Entity::bufferCallback(false);
}

//-------------------------------------------------------------------------------------
void Cellapp::reqCellHandoff(Network::Channel* channel, MemoryStream& stream)
{
	using namespace CellHandoff;
	BoundedStreamReader header(stream);
	ENTITY_ID id = 0;
	ENTITY_SCRIPT_UID type = 0, snapshotType = 0;
	Position3D pos;
	Direction3D dir;
	COMPONENT_ID baseID = 0, snapshotBase = 0;
	uint64 generation = 0, epoch = 0;
	SPACE_ID snapshotSpace = 0;
	bool destroyed = false;
	Route route;
	PeerProcesses processes;
	const auto* sender = findBoundCellappSource(channel);
	if (!sender || sender->pChannel != channel ||
		!header.read(id) || !header.read(type) ||
		!header.read(pos.x) || !header.read(pos.y) || !header.read(pos.z) ||
		!header.read(dir.dir.x) || !header.read(dir.dir.y) || !header.read(dir.dir.z) ||
		!header.read(baseID) || !header.read(generation) || !route.read(header) ||
		sender->cid != route.source || (sender->pid && sender->pid != route.sourcePid) ||
		componentID_ != route.target ||
		(baseID ? static_cast<uint32>(getProcessPID()) != route.targetPid :
			(route.targetPid != 0 || !processes.read(header) || processes.target != CellRecovery::recoveryProcessID())))
	{
		stream.done();
		return;
	}
	CopyPeers copies;
	const bool validCopies = readCopyPeers(header, route, copies);
	const size_t snapshotPos = stream.rpos() + stream.length() - header.remaining();
	auto* base = Components::getSingleton().findComponentChannel(BASEAPP_TYPE, baseID);
	auto* space = Spaces::findSpace(route.space);
	auto* entity = findEntity(id);
	if (!baseID)
	{
		const auto entry = incomingPeerCellHandoffs_.find(id);
		if (entry != incomingPeerCellHandoffs_.end() || (entity && entity->isReal()))
		{
			if (entry != incomingPeerCellHandoffs_.end() && entry->second.route == route &&
				entry->second.processes == processes)
				sendPeerCellHandoff(*channel, id, TARGET_INSTALLED, route, processes);
			stream.done();
			return;
		}
	}
	if (admittedCellHandoffs_.count(id))
	{
		stream.done();
		return;
	}
	const auto* destination = space && space->hasManagedCell() ?
		space->handoffDestination(pos.x, pos.z, route.source) : NULL;
	std::unique_ptr<CopyReservation> reservation;
	auto reserveCopies = [&]()
	{
		copies.emplace_back(route.source, space->layout().cellForApp(route.source)->cellID);
		reservation = handoffCopyBudget_.tryReserve(std::move(copies), [](COMPONENT_ID app) -> uint64
		{
			auto* channel = Components::getSingleton().findComponentChannel(CELLAPP_TYPE, app);
			if (!canSendCellHandoff(channel))
				return 0;
			const auto* endpoint = channel->pEndPoint();
			auto* poller = channel->networkInterface().dispatcher().pPoller();
			return static_cast<uint64>(channel->bundlesLength()) +
				(endpoint && poller ? poller->tcpSendPendingBytes(*endpoint) : 0);
		});
		if (!reservation)
			WARNING_MSG(fmt::format("Cellapp::cellHandoff: copy budget exhausted, entity={}, epoch={}, space={}.\n",
				id, route.epoch, route.space));
		return reservation != nullptr;
	};
	if (!validCopies || !spacePartitioningEnabled() || !spacePartitioningConfirmed() || isShuttingdown() ||
		!space || !space->canAcceptNewEntities() || space->layoutState() != SpaceLayout::ACTIVE ||
		!destination || destination->cellappID != componentID_ || !space->layout().cellForApp(route.source) ||
		(baseID ? !canSendCellHandoff(base) :
			(!autoCellHandoffEnabled() || incomingPeerCellHandoffs_.size() >= SpaceLayout::MAX_CELLS)) ||
		!entity || !entity->isBoundaryGhost() ||
		entity->spaceID() != route.space || entity->realCell() != route.source ||
		entity->pScriptModule()->getUType() != type || route.layoutVersion != space->layoutVersion() ||
		!header.read(snapshotType) || snapshotType != type || !header.read(epoch) || epoch != route.epoch ||
		!header.read(snapshotSpace) || snapshotSpace != route.space || !header.read(destroyed) || destroyed ||
		!header.skip(sizeof(bool) + sizeof(float) * 2 + sizeof(int8)) ||
		!header.read(snapshotBase) || snapshotBase != baseID || !reserveCopies() ||
		// 同一连接上也会重建 Ghost，旧快照不能把新副本转为 Real。
		!space->boundaryGhosts().forgetHandoffTarget(id, route.source, channel->sessionEpoch(), generation))
	{
		if (baseID)
			sendCellHandoff(base, id, TARGET_REJECTED, route);
		else
			sendPeerCellHandoff(*channel, id, TARGET_REJECTED, route, processes);
		WARNING_MSG(fmt::format("Cellapp::cellHandoff: target rejected, entity={}, epoch={}, space={}.\n", id, route.epoch, route.space));
		stream.done();
		return;
	}

	// 即使目标随后被销毁，也不能把已经安装过的快照当成没有收到。
	if (baseID)
		admittedCellHandoffs_.insert(id);
	else
		incomingPeerCellHandoffs_.emplace(id, IncomingPeerCellHandoff{route, processes});
	stream.rpos(snapshotPos);
	EntityPtr held(entity);
	Entity::bufferCallback(true);
	entity->removeFlags(ENTITY_FLAGS_BOUNDARY_GHOST);
	entity->addFlags(ENTITY_FLAGS_INITING);
	entity->beginCellHandoffTarget(route.source, std::move(reservation));
	entity->changeToReal(0, stream, true);
	entity->migrationRelayCell(route.source);
	entity->setPositionAndDirection(pos, dir);
	entity->restoreProximitys();
	if (!baseID)
		sendPeerCellHandoff(*channel, id, TARGET_INSTALLED, route, processes);
	Entity::bufferCallback(false);
	if (baseID)
		sendCellHandoff(base, id, TARGET_INSTALLED, route);
	stream.done();
}

//-------------------------------------------------------------------------------------
void Cellapp::checkCellHandoffSnapshot(Network::Channel* channel, MemoryStream& stream)
{
	using namespace CellHandoff;
	BoundedStreamReader reader(stream);
	ENTITY_ID id = 0;
	COMPONENT_ID base = 0;
	Route route;
	const bool parsed = reader.read(id) && reader.read(base) && route.read(reader) && reader.empty();
	stream.done();
	const auto* sender = findBoundCellappSource(channel);
	if (!parsed || !spacePartitioningEnabled() || !sender || sender->pChannel != channel ||
		sender->cid != route.source || (sender->pid && sender->pid != route.sourcePid) ||
		route.target != componentID_ || route.targetPid != static_cast<uint32>(getProcessPID()))
		return;

	// 快照只发送一次。同一连接按序处理；重连后旧连接不再是有效来源。
	// 因而只有从未安装过快照，才可确认源端能够恢复。
	if (!admittedCellHandoffs_.count(id))
	{
		auto* baseChannel = Components::getSingleton().findComponentChannel(BASEAPP_TYPE, base);
		sendCellHandoff(baseChannel, id, TARGET_REJECTED, route);
	}
}

//-------------------------------------------------------------------------------------
namespace
{
enum class TeleportCheck : uint8
{
	QUERY,
	ORDERED_QUERY,
	ACKNOWLEDGE
};

void sendTeleportCheck(Network::Channel* channel, ENTITY_ID id, COMPONENT_ID base, uint64 epoch, TeleportCheck check)
{
	if (!canSendCellHandoff(channel))
		return;
	auto* bundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
	bundle->newMessage(CellappInterface::reqTeleportToCellAppCheck);
	(*bundle) << id << base << epoch << static_cast<uint8>(check);
	channel->send(bundle);
}

template<typename... Details>
void sendTeleportBase(Network::Channel* channel, const Network::MessageHandler& handler,
	ENTITY_ID id, COMPONENT_ID source, COMPONENT_ID target, uint64 epoch, Details... details)
{
	if (!canSendCellHandoff(channel))
		return;
	auto* bundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
	bundle->newMessage(handler);
	(*bundle) << id << source << target << epoch;
	((*bundle) << ... << details);
	channel->send(bundle);
}

void sendTeleportReply(Network::Channel* channel, ENTITY_ID id, COMPONENT_ID source,
	COMPONENT_ID base, uint64 epoch, bool success)
{
	auto* bundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
	bundle->newMessage(CellappInterface::reqTeleportToCellAppCB);
	(*bundle) << source << g_componentID << base << id << success << epoch;
	channel->send(bundle);
}

void sendTeleportStatus(Network::Channel* channel, ENTITY_ID id, COMPONENT_ID source,
	uint64 epoch, TeleportStatus::Value status)
{
	if (!canSendCellHandoff(channel))
		return;
	auto* bundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
	bundle->newMessage(BaseappInterface::onMigrationCellappPrepared);
	(*bundle) << id << source << g_componentID << epoch << static_cast<uint8>(status);
	channel->send(bundle);
}

void sendTeleportReservation(Network::Channel& base, ENTITY_ID id, const TeleportStatus::Dispatch& route)
{
	auto* bundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
	bundle->newMessage(BaseappInterface::onMigrationCellappReserve);
	(*bundle) << id;
	route.write(*bundle);
	base.send(bundle);
}
}

bool Cellapp::requestTeleportDispatch(Entity& entity, EntityCall& destination, const Position3D& pos,
	const Direction3D& dir)
{
	auto* baseCall = entity.baseEntityCall();
	auto* base = baseCall ? baseCall->getChannel() : nullptr;
	auto* target = destination.getChannel();
	const auto* space = Spaces::findSpace(entity.spaceID());
	if (isShuttingdown() || !entity.isReal() || !canSendCellHandoff(base) || !canSendCellHandoff(target) ||
		!space || space->isCheckingReplacement() ||
		pendingTeleportDispatches_.count(entity.id()) || outgoingTeleports_.count(entity.id()))
		return false;
	const uint64 epoch = entity.reserveRoutingEpoch();
	if (!epoch)
		return false;
	PendingTeleportDispatch pending;
	pending.route = {destination.componentID(), entity.spaceID(), epoch};
	pending.targetEntity = destination.id();
	pending.base = baseCall->componentID();
	pending.baseSession = base->sessionEpoch();
	pending.targetSession = target->sessionEpoch();
	pending.deadline = timestamp() + 5 * stampsPerSecond();
	pending.pos = pos;
	pending.dir = dir;
	pendingTeleportDispatches_.emplace(entity.id(), pending);
	// 等待确认期间原实体仍是 Real，但不能开始另一轮迁移。
	entity.addFlags(ENTITY_FLAGS_TELEPORT_START);
	sendPendingTeleportDispatch(entity.id(), pendingTeleportDispatches_.at(entity.id()));
	return true;
}

void Cellapp::sendPendingTeleportDispatch(ENTITY_ID id, PendingTeleportDispatch& pending)
{
	auto& components = Components::getSingleton();
	auto* base = components.findComponentChannel(BASEAPP_TYPE, pending.base);
	auto* target = components.findComponentChannel(CELLAPP_TYPE, pending.route.target);
	if (!canSendCellHandoff(base) || base->sessionEpoch() != pending.baseSession ||
		!canSendCellHandoff(target) || target->sessionEpoch() != pending.targetSession)
		return;

	const uint64 process = queryCellPeerProcess(*target);
	if (process)
	{
		pending.route.targetProcess = process;
		sendTeleportReservation(*base, id, pending.route);
	}
}

uint64 Cellapp::queryCellPeerProcess(Network::Channel& channel)
{
	auto& peer = teleportPeers_[channel.componentID()];
	if (peer.session != channel.sessionEpoch())
		peer = {channel.sessionEpoch()};
	if (!peer.process && timestamp() >= peer.queryAt)
	{
		// 传送和交接共用同一条连接上确认的进程编号。
		peer.queryAt = timestamp() + stampsPerSecond();
		auto* bundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
		bundle->newMessage(CellappInterface::onTeleportPeerProcess);
		(*bundle) << uint64(0);
		channel.send(bundle);
	}
	return peer.process;
}

void Cellapp::onTeleportPeerProcess(Network::Channel* channel, uint64 process)
{
	if (!spacePartitioningEnabled() || !canSendCellHandoff(channel) ||
		!Components::getSingleton().isExpectedComponentChannel(CELLAPP_TYPE, channel))
		return;
	if (!process)
	{
		auto* bundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
		bundle->newMessage(CellappInterface::onTeleportPeerProcess);
		(*bundle) << CellRecovery::recoveryProcessID();
		channel->send(bundle);
		return;
	}
	const auto peer = teleportPeers_.find(channel->componentID());
	if (peer != teleportPeers_.end() && peer->second.session == channel->sessionEpoch() &&
		!peer->second.process)
		peer->second.process = process;
}

void Cellapp::onTeleportDispatchPermission(Network::Channel* channel, MemoryStream& stream)
{
	BoundedStreamReader reader(stream);
	ENTITY_ID id = 0;
	TeleportStatus::Dispatch route;
	uint8 accepted = 0;
	const bool parsed = reader.read(id) && route.read(reader) && reader.read(accepted) && accepted <= 1 && reader.empty();
	stream.done();
	auto& components = Components::getSingleton();
	if (!parsed || !canSendCellHandoff(channel) ||
		!components.isExpectedComponentChannel(BASEAPP_TYPE, channel))
		return;
	const auto it = pendingTeleportDispatches_.find(id);
	if (it == pendingTeleportDispatches_.end())
		return;
	const auto pending = it->second;
	if (pending.base != channel->componentID() || pending.baseSession != channel->sessionEpoch() ||
		pending.route.epoch != route.epoch || pending.route.target != route.target ||
		pending.route.origin != route.origin || pending.route.targetProcess != route.targetProcess)
		return;
	pendingTeleportDispatches_.erase(it);
	auto* entity = findEntity(id);
	if (!entity || entity->isDestroyed() || !entity->isReal() || entity->spaceID() != route.origin ||
		!entity->baseEntityCall() || entity->baseEntityCall()->componentID() != pending.base)
		return;
	EntityPtr held(entity);
	if (!accepted)
	{
		entity->removeFlags(ENTITY_FLAGS_TELEPORT_START);
		entity->onTeleportFailure();
		return;
	}
	auto* target = components.findComponentChannel(CELLAPP_TYPE, route.target);
	if (!isShuttingdown() && timestamp() < pending.deadline && canSendCellHandoff(target) &&
		target->sessionEpoch() == pending.targetSession &&
		entity->dispatchTeleport(pending.targetEntity, route.target, pending.pos, pending.dir, route.epoch))
		return;
	if (!entity->isDestroyed())
		cancelTeleportDispatch(*entity, pending);
}

void Cellapp::cancelTeleportDispatch(Entity& entity, const PendingTeleportDispatch& pending)
{
	// 未向目标发送快照，复用源端回滚确认，避免 Base 留着已经取消的迁移。
	auto cancelled = std::make_unique<OutgoingTeleport>();
	cancelled->target = pending.route.target;
	cancelled->base = pending.base;
	cancelled->epoch = pending.route.epoch;
	cancelled->restored = true;
	cancelled->checkAt = timestamp() + stampsPerSecond();
	entity.routingEpoch(pending.route.epoch);
	outgoingTeleports_.emplace(entity.id(), std::move(cancelled));
	auto* base = Components::getSingleton().findComponentChannel(BASEAPP_TYPE, pending.base);
	sendTeleportBase(base, BaseappInterface::onMigrationCellappEnd, entity.id(),
		componentID_, componentID_, pending.route.epoch, entity.spaceID());
}

void Cellapp::trackTeleportSource(Entity& entity, COMPONENT_ID target, uint64 session, MemoryStream& snapshot)
{
	auto pending = std::make_unique<OutgoingTeleport>();
	pending->target = target;
	pending->base = entity.baseEntityCall() ? entity.baseEntityCall()->componentID() : 0;
	pending->epoch = entity.routingEpoch();
	pending->session = session;
	pending->checkAt = timestamp() + 2 * stampsPerSecond();
	pending->snapshot.swap(snapshot);
	auto* base = Components::getSingleton().findComponentChannel(BASEAPP_TYPE, pending->base);
	sendTeleportBase(base, BaseappInterface::onMigrationCellappStart, entity.id(),
		componentID_, target, pending->epoch, uint8(0));
	outgoingTeleports_[entity.id()] = std::move(pending);
}

bool Cellapp::deferTeleportWitness(Entity& entity, Network::Channel* base)
{
	auto it = outgoingTeleports_.find(entity.id());
	if (it == outgoingTeleports_.end() || it->second->restored || it->second->epoch != entity.routingEpoch() ||
		!base || base->componentID() != it->second->base ||
		!Components::getSingleton().isExpectedComponentChannel(BASEAPP_TYPE, base))
		return false;

	// 重登可能赶在 Base 收到 Start 之前。把这次绑定交回 Base，在路由确定后重发。
	sendTeleportBase(base, BaseappInterface::onMigrationCellappStart, entity.id(),
		componentID_, it->second->target, it->second->epoch, uint8(1));
	return true;
}

void Cellapp::retryTeleports()
{
	const uint64 now = timestamp();
	nextTeleportRetry_ = now + stampsPerSecond();
	auto& components = Components::getSingleton();
	for (auto it = pendingTeleportDispatches_.begin(); it != pendingTeleportDispatches_.end();)
	{
		if (now < it->second.deadline)
		{
			sendPendingTeleportDispatch(it->first, it->second);
			++it;
			continue;
		}
		const auto id = it->first;
		const auto pending = it->second;
		it = pendingTeleportDispatches_.erase(it);
		auto* entity = findEntity(id);
		if (entity && !entity->isDestroyed() && entity->isReal() &&
			entity->spaceID() == pending.route.origin && entity->baseEntityCall() &&
			entity->baseEntityCall()->componentID() == pending.base)
			cancelTeleportDispatch(*entity, pending);
	}
	for (auto& [attempt, retryAt] : acceptedCellOnlyTeleports_)
	{
		if (now < retryAt)
			continue;
		const auto& [source, id, epoch] = attempt;
		auto* channel = components.findComponentChannel(CELLAPP_TYPE, source);
		if (canSendCellHandoff(channel))
			sendTeleportReply(channel, id, source, 0, epoch, true);
	}
	for (auto it = outgoingTeleports_.begin(); it != outgoingTeleports_.end();)
	{
		const auto& pending = *it->second;
		auto* entity = findEntity(it->first);
		if (!entity || entity->routingEpoch() != pending.epoch)
		{
			it = outgoingTeleports_.erase(it);
			continue;
		}
		const ENTITY_ID id = it++->first;
		if (now < pending.checkAt)
			continue;

		auto* base = components.findComponentChannel(BASEAPP_TYPE, pending.base);
		if (pending.restored)
		{
			sendTeleportBase(base, BaseappInterface::onMigrationCellappEnd, id,
				componentID_, componentID_, pending.epoch, entity->spaceID());
			continue;
		}
		sendTeleportBase(base, BaseappInterface::onMigrationCellappStart, id,
			componentID_, pending.target, pending.epoch, uint8(0));
		auto* target = components.findComponentChannel(CELLAPP_TYPE, pending.target);
		if (canSendCellHandoff(target))
			sendTeleportCheck(target, id, pending.base, pending.epoch,
				target->sessionEpoch() == pending.session ? TeleportCheck::ORDERED_QUERY : TeleportCheck::QUERY);
	}
}

void Cellapp::restoreTeleportSource(ENTITY_ID id, COMPONENT_ID target, uint64 epoch)
{
	auto it = outgoingTeleports_.find(id);
	auto* entity = findEntity(id);
	if (it == outgoingTeleports_.end() || !entity || entity->isDestroyed())
		return;
	auto& pending = *it->second;
	if (pending.target != target || pending.epoch != epoch || entity->routingEpoch() != epoch)
		return;
	if (!pending.restored)
	{
		if (!entity->isMigrationSource() || entity->isReal() || entity->realCell() != target)
			return;
		EntityPtr held(entity);
		Entity::bufferCallback(true);
		entity->changeToReal(0, pending.snapshot);
		entity->restoreProximitys();
		pending.restored = true;
		// Base 确认前不允许下一次迁移覆盖本轮编号。
		if (pending.base)
			entity->addFlags(ENTITY_FLAGS_TELEPORT_START);
		auto* base = Components::getSingleton().findComponentChannel(BASEAPP_TYPE, pending.base);
		sendTeleportBase(base, BaseappInterface::onMigrationCellappEnd, id,
			componentID_, componentID_, epoch, entity->spaceID());
		if (!pending.base)
		{
			outgoingTeleports_.erase(it);
			entity->onTeleportFailure();
		}
		Entity::bufferCallback(false);
	}
}

void Cellapp::reqTeleportToCellApp(Network::Channel* pChannel, MemoryStream& s)
{
	const size_t teleportHeaderSize = sizeof(ENTITY_ID) * 2 + sizeof(SPACE_ID) +
		sizeof(ENTITY_SCRIPT_UID) + sizeof(Position3D) + sizeof(Direction3D) +
		sizeof(COMPONENT_ID) * 2;
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

	s >> entityBaseappID;
	COMPONENT_ID ghostCell;
	s >> ghostCell;
	if (ghostCell == 0 || ghostCell != sourceInfos->cid)
	{
		WARNING_MSG(fmt::format("Cellapp::reqTeleportToCellApp: rejected sourceCellID={}, payloadSourceCellID={}.\n",
			sourceInfos->cid, ghostCell));
		s.done();
		return;
	}

	BoundedStreamReader snapshot(s);
	ENTITY_SCRIPT_UID snapshotType = 0;
	uint64 epoch = 0;
	if (!snapshot.read(snapshotType) || !snapshot.read(epoch) || !epoch)
	{
		s.done();
		return;
	}
	const CellOnlyTeleport attempt{ghostCell, teleportEntityID, epoch};
	if (!entityBaseappID && acceptedCellOnlyTeleports_.count(attempt))
	{
		sendTeleportReply(pChannel, teleportEntityID, ghostCell, 0, epoch, true);
		s.done();
		return;
	}
	const auto reject = [&]
	{
		sendTeleportReply(pChannel, teleportEntityID, ghostCell, entityBaseappID, epoch, false);
		s.done();
	};

	Entity* existing = findEntity(teleportEntityID);
	if (existing && !existing->isDestroyed() && existing->isReal())
	{
		// 目标已有 Real 时不能回滚源端；同一轮由状态查询重新确认。
		s.done();
		return;
	}
	Entity* refEntity = findEntity(nearbyMBRefID);
	if (refEntity == NULL || refEntity->isDestroyed() || refEntity->isBoundaryGhost())
	{
		reject();
		WARNING_MSG(fmt::format("Cellapp::reqTeleportToCellApp: refEntity({}) was destroyed before migration, spaceID({}), reqTeleportEntity({}).\n",
			nearbyMBRefID, spaceID, teleportEntityID));
		return;
	}

	Space* space = Spaces::findSpace(refEntity->spaceID());
	auto* module = EntityDef::findScriptModule(entityType);
	if (space == NULL || !space->canAcceptNewEntities() || !module || snapshotType != entityType ||
		(existing && (!existing->isBoundaryGhost() || existing->realCell() != ghostCell ||
			existing->pScriptModule()->getUType() != entityType)))
	{
		reject();
		WARNING_MSG(fmt::format("Cellapp::reqTeleportToCellApp: target space({}) is not accepting new entities, reqTeleportEntity({}).\n", spaceID, teleportEntityID));
		return;
	}

	// 目标 Cell 的反序列化/创建阶段从实体构造开始，到进入新 Space 为止；回调单独计量。
	// Target-Cell deserialize/create timing starts at construction and stops on entering the new Space; callbacks are measured separately.
	const uint64 deserializeCreateStart = scriptStageStartStamps();
	// 创建entity
	const uint64 entityConstructStart = scriptStageStartStamps();
	// 显式传送仍创建完整的 Real。目标进程已有边界副本时先释放同一源端的副本。
	if (existing)
		destroyEntity(teleportEntityID, false);
	Entity* e = createEntity(module->getName(), NULL, false, teleportEntityID, false);
	scriptStageMetrics().record(SCRIPT_STAGE_MIGRATION_ENTITY_CONSTRUCT,
		scriptStageDurationNanos(entityConstructStart), true, "reqTeleportToCellApp");
	if (e == NULL)
	{
		reject();

		WARNING_MSG(fmt::format("Cellapp::reqTeleportToCellApp: create reqTeleportEntity({}) failed during migration.\n", teleportEntityID));
		scriptStageMetrics().record(SCRIPT_STAGE_MIGRATION_DESERIALIZE_CREATE,
			scriptStageDurationNanos(deserializeCreateStart), true, "reqTeleportToCellApp");
		s.done();
		return;
	}

	Py_INCREF(e);
	const uint64 streamRestoreStart = scriptStageStartStamps();
	e->createFromStream(s);
	scriptStageMetrics().record(SCRIPT_STAGE_MIGRATION_STREAM_RESTORE,
		scriptStageDurationNanos(streamRestoreStart), true, "reqTeleportToCellApp");

	// 有可能序列化过来的ghost内容包含移动控制器，之所以序列化过来是为了
	// 在传送失败时可以用于恢复现场, 那么传送成功了我们应该停止以前的移动行为
	e->stopMove();

	// 对于传送操作来说，实体传送过来就不会有ghost部分了
	// 当前实体做出的任何改变不需要同步到原有cell，这可能会产生网络消息死循环
	//ghostCell = 0;
	e->ghostCell(0);
	// ghostCell must stay clear to avoid replication loops. Retain only the
	// authenticated predecessor used to drain client RPCs queued before the
	// BaseApp route switch. / ghostCell 必须保持为空以避免同步回环；这里只
	// 保留已认证的前驱 Cell，用于排空 BaseApp 切路由前已入队的客户端 RPC。
	e->migrationRelayCell(ghostCell);

	e->spaceID(space->id());
	e->setPositionAndDirection(pos, dir);

	if (e->baseEntityCall())
	{
		e->addFlags(ENTITY_FLAGS_TELEPORT_START);
		
		// 如果是有base的实体，需要将baseappID填入，以便在reqTeleportToCellAppCB中回调给baseapp传输结束状态
		entityBaseappID = e->baseEntityCall()->componentID();
	}
	else if (!entityBaseappID)
	{
		// 进入 Space 的脚本可以销毁或再次传送实体，回包不能只依赖目标仍有 Real。
		acceptedCellOnlyTeleports_.emplace(attempt, timestamp() + 2 * stampsPerSecond());
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
	const uint64 spaceAoiStart = scriptStageStartStamps();
	space->addEntityAndEnterWorld(e, false, true);
	scriptStageMetrics().record(SCRIPT_STAGE_MIGRATION_SPACE_AOI,
		scriptStageDurationNanos(spaceAoiStart), true, "reqTeleportToCellApp");
	scriptStageMetrics().record(SCRIPT_STAGE_MIGRATION_DESERIALIZE_CREATE,
		scriptStageDurationNanos(deserializeCreateStart), true, "reqTeleportToCellApp");

	Entity* nearbyMBRef = Cellapp::getSingleton().findEntity(nearbyMBRefID);
	e->onTeleportSuccess(nearbyMBRef, space->id());

	if (e->baseEntityCall())
	{
		// Target 的实体、Space、Witness 和脚本回调全部就绪后才能允许 BaseApp 切换 RPC 路由。
		// BaseApp may switch RPC routing only after Target's Entity, Space, Witness, and script callbacks are ready.
		Network::Bundle* pBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
		(*pBundle).newMessage(BaseappInterface::onMigrationCellappEnd);
		(*pBundle) << e->id();
		(*pBundle) << ghostCell << g_componentID << epoch << e->spaceID();
		e->baseEntityCall()->sendCall(pBundle);
	}

	sendTeleportReply(pChannel, teleportEntityID, ghostCell, entityBaseappID, epoch, true);
	
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

	const auto* targetCellapp = findBoundCellappSource(pChannel);
	if (!targetCellapp || targetCellapp->cid != targetCellappID)
	{
		WARNING_MSG(fmt::format("Cellapp::reqTeleportToCellAppCB: rejected unbound targetCellappID={}, addr={}.\n",
			targetCellappID, pChannel->c_str()));
		s.done();
		return;
	}
	const uint64 callbackStart = scriptStageStartStamps();

	// 正常情况下， 应该传送结果返回时传送前的实体应该在当前cell上， 如果到其他cellapp上了， 说明在此期间被迁移走了
	// 此时被迁移很可能会有问题
	if (sourceCellappID != g_componentID)
	{
		ERROR_MSG(fmt::format("Cellapp::reqTeleportToCellAppCB(): sourceCellappID={} != currCellappID={}, targetCellappID={}\n",
			sourceCellappID, g_componentID, targetCellappID));
		s.done();
		return;
	}

	BoundedStreamReader payload(s);
	uint64 epoch = 0;
	const bool parsed = payload.read(epoch) && payload.empty() && teleportEntityID > 0 && epoch;
	s.done();
	if (!parsed)
		return;
	const auto it = outgoingTeleports_.find(teleportEntityID);
	if (it != outgoingTeleports_.end() && it->second->base == entityBaseappID &&
		it->second->target == targetCellappID && it->second->epoch == epoch)
	{
		if (!success)
			restoreTeleportSource(teleportEntityID, targetCellappID, epoch);
		else if (!entityBaseappID)
		{
			outgoingTeleports_.erase(it);
			auto* entity = findEntity(teleportEntityID);
			if (entity && entity->isMigrationSource() && entity->realCell() == targetCellappID &&
				entity->routingEpoch() == epoch)
				destroyEntity(teleportEntityID, false);
		}
	}
	// 源副本已经清掉时也要确认，避免确认包丢失后目标一直保留记录。
	if (success && !entityBaseappID)
		sendTeleportCheck(pChannel, teleportEntityID, 0, epoch, TeleportCheck::ACKNOWLEDGE);
	scriptStageMetrics().record(SCRIPT_STAGE_MIGRATION_CALLBACK,
		scriptStageDurationNanos(callbackStart), true, "reqTeleportToCellAppCB");
}

//-------------------------------------------------------------------------------------
void Cellapp::reqTeleportToCellAppCheck(Network::Channel* channel, MemoryStream& stream)
{
	auto* source = findBoundCellappSource(channel);
	BoundedStreamReader reader(stream);
	ENTITY_ID id = 0;
	COMPONENT_ID baseID = 0;
	uint64 epoch = 0;
	uint8 action = 0;
	const bool parsed = reader.read(id) && reader.read(baseID) && reader.read(epoch) &&
		reader.read(action) && reader.empty() && id > 0 && epoch &&
		action <= static_cast<uint8>(TeleportCheck::ACKNOWLEDGE);
	stream.done();
	if (!source || !parsed)
		return;
	const auto check = static_cast<TeleportCheck>(action);
	const CellOnlyTeleport attempt{source->cid, id, epoch};
	if (check == TeleportCheck::ACKNOWLEDGE)
	{
		if (!baseID)
			acceptedCellOnlyTeleports_.erase(attempt);
		return;
	}
	if (!baseID && acceptedCellOnlyTeleports_.count(attempt))
	{
		sendTeleportReply(channel, id, source->cid, 0, epoch, true);
		return;
	}
	auto* entity = findEntity(id);
	auto* base = Components::getSingleton().findComponentChannel(BASEAPP_TYPE, baseID);
	if (entity && !entity->isDestroyed() && entity->isReal() && entity->routingEpoch() == epoch &&
		entity->migrationRelayCell() == source->cid &&
		(baseID ? (entity->baseEntityCall() && entity->baseEntityCall()->componentID() == baseID) : !entity->baseEntityCall()))
	{
		if (baseID)
			sendTeleportBase(base, BaseappInterface::onMigrationCellappEnd, id,
				source->cid, componentID_, epoch, entity->spaceID());
		else
			sendTeleportReply(channel, id, source->cid, 0, epoch, true);
	}
	else if (check == TeleportCheck::ORDERED_QUERY && (!entity || entity->isDestroyed() || !entity->isReal()))
	{
		// 只有原 TCP 连接上的查询才排在快照之后。新连接只能确认已经安装，不能据此回滚。
		if (baseID)
			sendTeleportStatus(base, id, source->cid, epoch, TeleportStatus::NOT_INSTALLED);
		else
			sendTeleportReply(channel, id, source->cid, 0, epoch, false);
	}
}

//-------------------------------------------------------------------------------------
void Cellapp::reqTeleportToCellAppPrepare(Network::Channel* pChannel, MemoryStream& s)
{
	const size_t prepareHeaderSize = sizeof(ENTITY_ID) + sizeof(COMPONENT_ID) * 2 + sizeof(uint64);
	if (s.length() < prepareHeaderSize)
	{
		WARNING_MSG("Cellapp::reqTeleportToCellAppPrepare: rejected incomplete fixed header.\n");
		s.done();
		return;
	}
	if (!Components::getSingleton().isExpectedComponentChannel(BASEAPP_TYPE, pChannel))
	{
		WARNING_MSG("Cellapp::reqTeleportToCellAppPrepare: rejected non-BaseApp source.\n");
		s.done();
		return;
	}

	ENTITY_ID teleportEntityID = 0;
	COMPONENT_ID sourceCellAppID = 0;
	COMPONENT_ID targetCellAppID = 0;
	uint64 epoch = 0;
	s >> teleportEntityID >> sourceCellAppID >> targetCellAppID >> epoch;
	if (targetCellAppID != componentID_ || !sourceCellAppID || !epoch)
	{
		s.done();
		return;
	}

	Entity* entity = Cellapp::getSingleton().findEntity(teleportEntityID);
	Space* space = entity != NULL ? Spaces::findSpace(entity->spaceID()) : NULL;
	const bool live = entity != NULL && !entity->isDestroyed() && entity->isReal();
	const bool ready = live && entity->routingEpoch() == epoch &&
		!entity->isCellHandoffTarget() && outgoingCellHandoffs_.count(teleportEntityID) == 0 &&
		entity->hasFlags(ENTITY_FLAGS_TELEPORT_START) &&
		entity->migrationRelayCell() == sourceCellAppID &&
		entity->baseEntityCall() != NULL &&
		entity->baseEntityCall()->componentID() == pChannel->componentID() &&
		space != NULL && space->isGood();

	sendTeleportStatus(pChannel, teleportEntityID, sourceCellAppID, epoch,
		ready ? TeleportStatus::READY : (live ? TeleportStatus::WAIT : TeleportStatus::REJECTED));
	s.done();
}

//-------------------------------------------------------------------------------------
void Cellapp::reqTeleportToCellAppOver(Network::Channel* pChannel, MemoryStream& s)
{
	const size_t completionHeaderSize = sizeof(ENTITY_ID) + sizeof(COMPONENT_ID) * 2 + sizeof(uint64) + sizeof(uint8);
	if (s.length() < completionHeaderSize)
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
	COMPONENT_ID sourceCellAppID = 0;
	COMPONENT_ID targetCellAppID = 0;

	uint64 epoch = 0;
	uint8 abort = 0;
	s >> teleportEntityID >> sourceCellAppID >> targetCellAppID >> epoch >> abort;
	if (sourceCellAppID == 0 || targetCellAppID == 0 || !epoch || abort > 1 ||
		(g_componentID != sourceCellAppID && g_componentID != targetCellAppID))
	{
		WARNING_MSG(fmt::format("Cellapp::reqTeleportToCellAppOver: rejected invalid route, "
			"entity={}, sourceCellAppID={}, targetCellAppID={}, localCellAppID={}.\n",
			teleportEntityID, sourceCellAppID, targetCellAppID, g_componentID));
		s.done();
		return;
	}
	
	// 某些情况下实体可能此时找不到了，例如：副本销毁了
	Entity* entity = Cellapp::getSingleton().findEntity(teleportEntityID);
	if(entity == NULL)
	{
		WARNING_MSG(fmt::format("Cellapp::reqTeleportToCellAppOver: entity({}) was destroyed before the late migration confirmation.\n",
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
	if (entity->isCellHandoffTarget() || outgoingCellHandoffs_.count(teleportEntityID))
	{
		s.done();
		return;
	}
	if (entity->routingEpoch() != epoch)
	{
		s.done();
		return;
	}
	if (abort)
	{
		if (g_componentID == sourceCellAppID && sourceCellAppID != targetCellAppID)
			restoreTeleportSource(teleportEntityID, targetCellAppID, epoch);
		s.done();
		return;
	}

	if (sourceCellAppID == targetCellAppID)
	{
		const auto it = outgoingTeleports_.find(teleportEntityID);
		if (!entity->isReal() || it == outgoingTeleports_.end() ||
			it->second->epoch != epoch || !it->second->restored)
		{
			s.done();
			return;
		}

		// 脚本可以在失败回调里立即重试，先解除本轮限制并清掉原快照。
		EntityPtr held(entity);
		entity->removeFlags(ENTITY_FLAGS_TELEPORT_START);
		outgoingTeleports_.erase(teleportEntityID);
		s.done();
		entity->onTeleportFailure();
		return;
	}

	if (g_componentID == sourceCellAppID)
	{
		if (entity->isReal() || entity->realCell() != targetCellAppID)
		{
			WARNING_MSG(fmt::format("Cellapp::reqTeleportToCellAppOver: ignored stale Source release, "
				"entity={}, isReal={}, realCell={}, targetCellAppID={}.\n",
				teleportEntityID, entity->isReal(), entity->realCell(), targetCellAppID));
			s.done();
			return;
		}

		outgoingTeleports_.erase(teleportEntityID);
		destroyEntity(teleportEntityID, false);
	}
	else
	{
		if (!entity->isReal())
		{
			WARNING_MSG(fmt::format("Cellapp::reqTeleportToCellAppOver: ignored stale Target unlock, "
				"entity={}, realCell={}, sourceCellAppID={}.\n",
				teleportEntityID, entity->realCell(), sourceCellAppID));
			s.done();
			return;
		}

		entity->removeFlags(ENTITY_FLAGS_TELEPORT_START);
	}
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
	const uint32 viewerV2Magic = 0x3253584E;
	if (s.length() >= sizeof(uint32) + sizeof(uint16) + sizeof(uint8) + sizeof(SPACE_ID) + sizeof(uint16))
	{
		const size_t initialReadPosition = s.rpos();
		uint32 magic = 0;
		s >> magic;
		if (magic == viewerV2Magic)
		{
			uint16 version = 0;
			uint8 enabled = 0;
			SPACE_ID spaceID = 0;
			uint16 sampleIntervalMs = 0;
			s >> version >> enabled >> spaceID >> sampleIntervalMs;
			if (version != 3 ||
				(sampleIntervalMs != 100 && sampleIntervalMs != 250 &&
				 sampleIntervalMs != 500 && sampleIntervalMs != 1000))
			{
				WARNING_MSG(fmt::format("Cellapp::setSpaceViewer: invalid V2 request(version={}, interval={}).\n",
					version, sampleIntervalMs));
				return;
			}

			if (!this->validateManagementAdminToken(pChannel, s, "setSpaceViewer"))
			{
				return;
			}

			spaceViewers_.updateSpaceViewer(
				pChannel->addr(), spaceID, 0, enabled == 0, true, sampleIntervalMs);
			return;
		}

		s.rpos(initialReadPosition);
	}

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
