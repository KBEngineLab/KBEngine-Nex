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

#ifndef KBE_WITNESS_H
#define KBE_WITNESS_H

// common include
#include "updatable.h"
#include "entityref.h"
#include "witness_dirty_queue.h"
#include "witness_delayed_queue.h"
#include "witness_load_metrics.h"
#include "helper/debug_helper.h"
#include "common/common.h"
#include "common/objectpool.h"
#include "math/math.h"

// #define NDEBUG
// windows include	
#if KBE_PLATFORM == PLATFORM_WIN32	
#else
// linux include
#endif

namespace KBEngine{

namespace Network
{
	class Bundle;
	class MessageHandler;
}

class Entity;
class MemoryStream;
class ViewTrigger;
class Space;

/**
	这个模块用来监视我们感兴趣的entity数据， 如：view， 属性更新， 调用entity的方法
	并将其传输给监视者。
*/
class Witness : public PoolObject, public Updatable
{
public:
	typedef std::list<EntityRef*> VIEW_ENTITIES;
	typedef std::map<ENTITY_ID, EntityRef*> VIEW_ENTITIES_MAP;

	Witness();
	~Witness();
	
	virtual uint8 updatePriority() const {
		return 1;
	}

	void addToStream(KBEngine::MemoryStream& s);
	void createFromStream(KBEngine::MemoryStream& s);

	typedef KBEShared_ptr< SmartPoolObject< Witness > > SmartPoolObjectPtr;
	static SmartPoolObjectPtr createSmartPoolObj(const std::string& logPoint);

	static ObjectPool<Witness>& ObjPool();
	static Witness* createPoolObject(const std::string& logPoint);
	static void reclaimPoolObject(Witness* obj);
	static void destroyObjPool();
	void onReclaimObject();

	virtual size_t getPoolObjectBytes()
	{
		size_t bytes = sizeof(pEntity_)
		 + sizeof(viewRadius_) + sizeof(viewHysteresisArea_)
		 + sizeof(pViewTrigger_) + sizeof(pViewHysteresisAreaTrigger_) + sizeof(clientViewSize_)
		 + sizeof(lastBasePos_) + (sizeof(EntityRef*) * viewEntities_map_.size());

		return bytes;
	}

	INLINE void pEntity(Entity* pEntity);
	INLINE Entity* pEntity();

	void attach(Entity* pEntity);
	void detach(Entity* pEntity);
	void clear(Entity* pEntity);
	void onAttach(Entity* pEntity);

	void setViewRadius(float radius, float hyst = 5.0f);
	
	INLINE float viewRadius() const;
	INLINE float viewHysteresisArea() const;

	typedef std::vector<Network::Bundle*> Bundles;
	bool pushBundle(Network::Bundle* pBundle);

	/**
		基础位置， 如果有坐骑基础位置可能是坐骑等
	*/
	INLINE const Position3D& basePos();

	/**
	基础朝向， 如果有坐骑基础朝向可能是坐骑等
	*/
	INLINE const Direction3D& baseDir();

	bool update();
	
	void onEnterSpace(Space* pSpace, bool recordMigrationStages = false);
	void onLeaveSpace(Space* pSpace);

	void onEnterView(ViewTrigger* pViewTrigger, Entity* pEntity);
	void onLeaveView(ViewTrigger* pViewTrigger, Entity* pEntity);
	void _onLeaveView(EntityRef* pEntityRef);

	/**
		获得实体本次同步Volatile数据的标记
	*/
	uint32 getEntityVolatileDataUpdateFlags(Entity* otherEntity);
	

	const Network::MessageHandler& getViewEntityMessageHandler(const Network::MessageHandler& normalMsgHandler,
											   const Network::MessageHandler& optimizedMsgHandler, ENTITY_ID entityID, int& ialiasID);

	bool entityID2AliasID(ENTITY_ID id, uint8& aliasID);

	/**
		使用何种协议来更新客户端
	*/
	void addUpdateToStream(Network::Bundle* pForwardBundle, uint32 flags, EntityRef* pEntityRef);

	/**
		添加基础位置到更新包
	*/
	void addBaseDataToStream(Network::Bundle* pSendBundle);

	/**
		向witness客户端推送一条消息
	*/
	bool sendToClient(const Network::MessageHandler& msgHandler, Network::Bundle* pBundle);
	Network::Channel* pChannel();
		
	INLINE VIEW_ENTITIES_MAP& viewEntitiesMap();
	INLINE VIEW_ENTITIES& viewEntities();

	/** 获得viewentity的引用 */
	INLINE EntityRef* getViewEntityRef(ENTITY_ID entityID);

	/** entityID是否在view内 */
	INLINE bool entityInView(ENTITY_ID entityID);

	INLINE ViewTrigger* pViewTrigger();
	INLINE ViewTrigger* pViewHysteresisAreaTrigger();
	
	void installViewTrigger();
	void uninstallViewTrigger();

	/**
		重置View范围内的entities， 使其同步状态恢复到最初未同步的状态
	*/
	void resetViewEntities();

	/** 标记可见实体的易变数据需要在下一个更新批次同步。 */
	/** Marks a visible entity's volatile data for synchronization in the next update batch. */
	void markViewEntityVolatileDirty(ENTITY_ID entityID);
	void onOwnerPositionChanged();
	void setVolatileUpdatesEnabled(bool enabled);
	bool schedulerPending() const { return schedulerPending_; }

	static uint64 activeCount();
	static uint64 dirtyQueuedCount();
	static uint64 fullScanCount();
	static uint64 fullScanEntityCount();
	static uint64 dirtyProcessedCount();
	static uint64 maxQueueDepth();
	static uint64 viewEntityCount();
	static uint64 maxViewEntityCount();
	static uint64 dirtyEnqueuedCount();
	static uint64 dirtyRequeueCount();
	static uint64 staleDiscardCount();
	static uint64 stateSkipCount();
	static uint64 volatileBytesSentCount();
	static uint64 volatileBudgetDeferredCount();
	static uint64 volatileBudgetExhaustionCount();
	static uint64 sendBytesCount();
	static uint64 sendBudgetExhaustionCount();
	static uint64 structuralProcessedCount();
	static uint64 structuralQueuedCount();
	static uint64 volatileQueuedCount();
	static uint64 structuralEnqueuedCount();
	static uint64 volatileEnqueuedCount();
	static uint64 queueDeduplicatedCount();
	static uint64 producerCoalescedCount();
	static void recordProducerCoalesced();
	static uint64 structuralPromotionCount();
	static uint64 promotedVolatileSkipCount();
	static uint64 cancelledPendingLeaveCount();
	static void beginUpdateTick();
	static uint64 globalPendingCount();
	static uint64 globalPendingSnapshot();
	static uint64 globalPendingAdmittedCount();
	static uint64 globalPendingDeferredCount();
	static uint64 globalPendingArrivalCount();
	static uint64 globalAdmittedCount();
	static uint64 globalDeferredCount();
	static uint64 globalUpdateLimit();
	static uint64 enterUpdateCount();
	static uint64 enterBytesCount();
	static uint64 leaveUpdateCount();
	static uint64 leaveBytesCount();
	static uint64 enterProcessingSampleRate();
	static uint64 enterProcessingSamples();
	static uint64 enterProcessingTotalNanos();
	static uint64 enterProcessingAverageNanos();
	static uint64 enterProcessingMaxNanos();
	static uint64 enterProcessingSlowSamplesOver1ms();
	static uint64 leaveProcessingSampleRate();
	static uint64 leaveProcessingSamples();
	static uint64 leaveProcessingTotalNanos();
	static uint64 leaveProcessingAverageNanos();
	static uint64 leaveProcessingMaxNanos();
	static uint64 leaveProcessingSlowSamplesOver1ms();
	static uint64 volatileUpdateCount();
	static uint64 volatileUpdateBytesCount();
	static uint64 lodNearUpdateCount();
	static uint64 lodMediumUpdateCount();
	static uint64 lodFarUpdateCount();
	static uint64 lodDeferredRelationCount();
	static uint64 lodDistanceFilteredFieldCount();
	static uint64 activeSuppressedCount();
	static uint64 suppressionTransitionCount();
	static uint64 resumeTransitionCount();
	static uint64 suppressedUpdateSkipCount();
	static uint64 suppressedVolatileRefreshCount();
	static uint64 structuralWhileSuppressedCount();
	static uint64 producerBackpressureDeferredCount();
	static uint64 immediateBundleCount();
	static uint64 immediateBytesCount();
	static uint64 immediateBackpressuredBundleCount();
	static uint64 immediatePropertyBundleCount();
	static uint64 immediatePropertyBytesCount();
	static uint64 immediateRpcBundleCount();
	static uint64 immediateRpcBytesCount();
	static uint64 immediatePositionBundleCount();
	static uint64 immediatePositionBytesCount();
	static uint64 immediateSpaceDataBundleCount();
	static uint64 immediateSpaceDataBytesCount();
	static uint64 immediateOtherBundleCount();
	static uint64 immediateOtherBytesCount();
	static uint64 bundlesSentCount();
	static uint64 maxBundleBytes();

private:
	/**
		如果view中entity数量小于256则只发送索引位置
	*/
	INLINE void _addViewEntityIDToBundle(Network::Bundle* pBundle, EntityRef* pEntityRef);
	
	/**
		当update执行时view列表有改变的时候需要更新entityRef的aliasID
	*/
	void updateEntitiesAliasID(int removedAliasID = -1);
	void requireFullScan();
	void prepareFullScanQueue();
	void clearVolatileDirtyQueue();
	void synchronizeViewEntityMetrics();
	void initializeEntityRefLifecycle(EntityRef* pEntityRef);
	DETAIL_TYPE resolveEntityRefDetailLevel(EntityRef* pEntityRef) const;
	uint32 appendEntityRefDetailLevelProperties(Network::Bundle* pSendBundle,
		EntityRef* pEntityRef, DETAIL_TYPE minimumDetailLevel, DETAIL_TYPE maximumDetailLevel);
	void updateEntityRefDetailLevel(Network::Bundle* pSendBundle, EntityRef* pEntityRef);
	void processDetailLevelScan(Network::Bundle* pSendBundle);
	bool queueEntityRefVolatile(EntityRef* pEntityRef, bool requeue = false);
	void scheduleEntityRefVolatile(EntityRef* pEntityRef);
	void activateDueVolatileUpdates();
	void setSchedulerPending(bool pending);
	void refreshSchedulerPending();
	uint32 volatileUpdateIntervalTicks(Entity* pEntity) const;
	void releaseVolatileProducerIfDelivered(EntityRef* pEntityRef);
	bool needsVolatileUpdate(Entity* pEntity);
	bool isStructuralUpdate(const EntityRef* pEntityRef) const;
	bool processEntityRefUpdate(Network::Bundle* pSendBundle, EntityRef* pEntityRef);
	void removeViewEntityRef(EntityRef* pEntityRef);
	void processVolatileDirtyQueue(Network::Bundle* pSendBundle);
		
private:
	Entity*									pEntity_;

	// 当前entity的view半径
	float									viewRadius_;
	// 当前entityview的一个滞后范围
	float									viewHysteresisArea_;

	ViewTrigger*							pViewTrigger_;
	ViewTrigger*							pViewHysteresisAreaTrigger_;

	VIEW_ENTITIES							viewEntities_;
	VIEW_ENTITIES_MAP						viewEntities_map_;

	Position3D								lastBasePos_;
	Direction3D								lastBaseDir_;

	uint16									clientViewSize_;
	bool									fullScanRequired_;
	bool									detailLevelScanRequired_;
	bool									volatileQueueCompactionPending_;
	size_t								trackedViewEntityCount_;
	uint64									nextEntityRefGeneration_;
	WitnessDirtyQueue						volatileDirtyQueue_;
	WitnessDelayedQueue						delayedVolatileQueue_;
	WitnessDirtyQueue						structuralDirtyQueue_;
	bool									volatileUpdatesEnabled_;
	bool									schedulerPending_;
};

}

#ifdef CODE_INLINE
#include "witness.inl"
#endif
#endif
