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

#include "witness.h"
#include "witness_volatile_budget.h"
#include "witness_update_scheduler.h"
#include "entity.h"	
#include "profile.h"
#include "cellapp.h"
#include "view_trigger.h"
#include "server/asyncio_helper.h"
#include "server/script_stage_timing.h"
#include "network/channel.h"	
#include "network/bundle.h"
#include "network/network_stats.h"
#include "math/math.h"
#include "client_lib/client_interface.h"
#include "common/timestamp.h"

#include "../../server/baseapp/baseapp_interface.h"

#ifndef CODE_INLINE
#include "witness.inl"
#endif

#define UPDATE_FLAG_NULL				0x00000000
#define UPDATE_FLAG_XZ					0x00000001
#define UPDATE_FLAG_XYZ					0x00000002
#define UPDATE_FLAG_YAW					0x00000004
#define UPDATE_FLAG_ROLL				0x00000008
#define UPDATE_FLAG_PITCH				0x00000010
#define UPDATE_FLAG_YAW_PITCH_ROLL		0x00000020
#define UPDATE_FLAG_YAW_PITCH			0x00000040
#define UPDATE_FLAG_YAW_ROLL			0x00000080
#define UPDATE_FLAG_PITCH_ROLL			0x00000100
#define UPDATE_FLAG_ONGOUND				0x00000200

namespace KBEngine{	

namespace
{
uint64 g_witnessActiveCount = 0;
WitnessLoadMetrics g_witnessLoadMetrics;
WitnessUpdateScheduler g_witnessUpdateScheduler;
}

//-------------------------------------------------------------------------------------
Witness::Witness():
pEntity_(NULL),
viewRadius_(0.0f),
viewHysteresisArea_(5.0f),
pViewTrigger_(NULL),
pViewHysteresisAreaTrigger_(NULL),
viewEntities_(),
viewEntities_map_(),
clientViewSize_(0),
fullScanRequired_(true),
trackedViewEntityCount_(0),
nextEntityRefGeneration_(1),
volatileDirtyQueue_(),
structuralDirtyQueue_(),
volatileUpdatesEnabled_(true)
{
	updatableName = "Witness";
}

//-------------------------------------------------------------------------------------
Witness::~Witness()
{
	pEntity_ = NULL;
	SAFE_RELEASE(pViewTrigger_);
	SAFE_RELEASE(pViewHysteresisAreaTrigger_);
}

//-------------------------------------------------------------------------------------
void Witness::addToStream(KBEngine::MemoryStream& s)
{
	/**
	 * @TODO(phw): 注释下面的原始代码，简单修正如下的问题：
	 * 想象一下：A、B、C三个玩家互相能看见对方，那么它们的viewEntities_里面必须会互相记录着对方的entityID，
	 * 那么假如三个玩家都在同一时间传送到另一个cellapp的地图的同一点上，
	 * 这时三个玩家还原的时候都会为另两个玩家生成一个flags_ == ENTITYREF_FLAG_UNKONWN的EntityRef实例，
	 * 把它们记录在自己的viewEntities_，
	 * 但是，Witness::update()并没有针对flags_ == ENTITYREF_FLAG_UNKONWN的情况做特殊处理——把玩家entity数据发送给客户端，
	 * 所以进入了默认的updateVolatileData()流程，
	 * 使得客户端在没有别的玩家entity的情况下就收到了别的玩家的坐标更新的信息，导致客户端错误发生。
	
	s << viewRadius_ << viewHysteresisArea_ << clientViewSize_;	
	
	uint32 size = viewEntitiesmap_.size();
	s << size;

	EntityRef::VIEW_ENTITIES::iterator iter = viewEntities_.begin();
	for(; iter != viewEntities_.end(); ++iter)
	{
		(*iter)->addToStream(s);
	}
	*/

	// 当前这么做能解决问题，但是在space多cell分割的情况下将会出现问题
	s << viewRadius_ << viewHysteresisArea_ << (uint16)0;	
	s << (uint32)0; // viewEntities_map_.size();
}

//-------------------------------------------------------------------------------------
void Witness::createFromStream(KBEngine::MemoryStream& s)
{
	clearVolatileDirtyQueue();
	fullScanRequired_ = true;
	s >> viewRadius_ >> viewHysteresisArea_ >> clientViewSize_;

	uint32 size;
	s >> size;
	
	for(uint32 i=0; i<size; ++i)
	{
		EntityRef* pEntityRef = EntityRef::createPoolObject(OBJECTPOOL_POINT);
		pEntityRef->createFromStream(s);
		initializeEntityRefLifecycle(pEntityRef);
		viewEntities_.push_back(pEntityRef);
		viewEntities_map_[pEntityRef->id()] = pEntityRef;
		pEntityRef->aliasID(i);
	}
	synchronizeViewEntityMetrics();

	setViewRadius(viewRadius_, viewHysteresisArea_);

	lastBasePos_.z = -FLT_MAX;
	lastBaseDir_.yaw(-FLT_MAX);
	Cellapp::getSingleton().addUpdatable(this);
	++g_witnessActiveCount;
}

//-------------------------------------------------------------------------------------
void Witness::attach(Entity* pEntity)
{
	//DEBUG_MSG(fmt::format("Witness::attach: {}({}).\n", 
	//	pEntity->scriptName(), pEntity->id()));

	pEntity_ = pEntity;
	clearVolatileDirtyQueue();
	fullScanRequired_ = true;
	++g_witnessActiveCount;

	lastBasePos_.z = -FLT_MAX;
	lastBaseDir_.yaw(-FLT_MAX);

	if(g_kbeSrvConfig.getCellApp().use_coordinate_system)
	{
		// 初始化默认View范围
		ENGINE_COMPONENT_INFO& ecinfo = ServerConfig::getSingleton().getCellApp();
		setViewRadius(ecinfo.defaultViewRadius, ecinfo.defaultViewHysteresisArea);
	}

	Cellapp::getSingleton().addUpdatable(this);

	onAttach(pEntity);
}

//-------------------------------------------------------------------------------------
void Witness::onAttach(Entity* pEntity)
{
	lastBasePos_.z = -FLT_MAX;
	lastBaseDir_.yaw(-FLT_MAX);

	// 通知客户端enterworld
	Network::Bundle* pSendBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
	NETWORK_ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pEntity_->id(), (*pSendBundle));
	
	ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pSendBundle, ClientInterface::onUpdatePropertys, updatePropertys);
	MemoryStream* s1 = MemoryStream::createPoolObject(OBJECTPOOL_POINT);
	(*pSendBundle) << pEntity_->id();
	pEntity_->addPositionAndDirectionToStream(*s1, true);
	(*pSendBundle).append(*s1);
	MemoryStream::reclaimPoolObject(s1);
	ENTITY_MESSAGE_FORWARD_CLIENT_END(pSendBundle, ClientInterface::onUpdatePropertys, updatePropertys);
	
	ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pSendBundle, ClientInterface::onEntityEnterWorld, entityEnterWorld);

	(*pSendBundle) << pEntity_->id();
	pEntity_->pScriptModule()->addSmartUTypeToBundle(pSendBundle);
	if(!pEntity_->isOnGround())
		(*pSendBundle) << pEntity_->isOnGround();

	ENTITY_MESSAGE_FORWARD_CLIENT_END(pSendBundle, ClientInterface::onEntityEnterWorld, entityEnterWorld);
	pEntity_->clientEntityCall()->sendCall(pSendBundle);
}

//-------------------------------------------------------------------------------------
void Witness::detach(Entity* pEntity)
{
	//DEBUG_MSG(fmt::format("Witness::detach: {}({}).\n", 
	//	pEntity->scriptName(), pEntity->id()));

	EntityCall* pClientMB = pEntity_->clientEntityCall();
	if(pClientMB)
	{
		Network::Channel* pChannel = pClientMB->getChannel();
		if(pChannel)
		{
			pChannel->send();

			// 通知客户端leaveworld
			Network::Bundle* pSendBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
			NETWORK_ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pEntity_->id(), (*pSendBundle));

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pSendBundle, ClientInterface::onEntityLeaveWorld, entityLeaveWorld);
			(*pSendBundle) << pEntity->id();
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pSendBundle, ClientInterface::onEntityLeaveWorld, entityLeaveWorld);
			pClientMB->sendCall(pSendBundle);
		}
	}

	clear(pEntity);
}

//-------------------------------------------------------------------------------------
void Witness::clear(Entity* pEntity)
{
	KBE_ASSERT(pEntity == pEntity_);
	uninstallViewTrigger();

	VIEW_ENTITIES::iterator iter = viewEntities_.begin();
	for(; iter != viewEntities_.end(); ++iter)
	{
		if((*iter)->pEntity())
		{
			(*iter)->pEntity()->delWitnessed(pEntity_);
		}
		
		EntityRef::reclaimPoolObject((*iter));
	}
	
	setVolatileUpdatesEnabled(true);
	pEntity_ = NULL;
	viewRadius_ = 0.0f;
	viewHysteresisArea_ = 5.0f;
	clientViewSize_ = 0;

	// 不需要销毁，后面还可以重用
	// 此处销毁可能会产生错误，因为enterView过程中可能导致实体销毁
	// 在pViewTrigger_流程没走完之前这里销毁了pViewTrigger_就crash
	//SAFE_RELEASE(pViewTrigger_);
	//SAFE_RELEASE(pViewHysteresisAreaTrigger_);

	viewEntities_.clear();
	viewEntities_map_.clear();
	synchronizeViewEntityMetrics();
	clearVolatileDirtyQueue();
	fullScanRequired_ = true;
	KBE_ASSERT(g_witnessActiveCount > 0);
	--g_witnessActiveCount;

	Cellapp::getSingleton().removeUpdatable(this);
}

//-------------------------------------------------------------------------------------
static ObjectPool<Witness> _g_objPool("Witness");
ObjectPool<Witness>& Witness::ObjPool()
{
	return _g_objPool;
}

//-------------------------------------------------------------------------------------
Witness* Witness::createPoolObject(const std::string& logPoint)
{
	return _g_objPool.createObject(logPoint);
}

//-------------------------------------------------------------------------------------
void Witness::reclaimPoolObject(Witness* obj)
{
	_g_objPool.reclaimObject(obj);
}

//-------------------------------------------------------------------------------------
void Witness::destroyObjPool()
{
	DEBUG_MSG(fmt::format("Witness::destroyObjPool(): size {}.\n",
		_g_objPool.size()));

	_g_objPool.destroy();
}

//-------------------------------------------------------------------------------------
Witness::SmartPoolObjectPtr Witness::createSmartPoolObj(const std::string& logPoint)
{
	return SmartPoolObjectPtr(new SmartPoolObject<Witness>(ObjPool().createObject(logPoint), _g_objPool));
}

//-------------------------------------------------------------------------------------
void Witness::onReclaimObject()
{
	setVolatileUpdatesEnabled(true);
	synchronizeViewEntityMetrics();
	clearVolatileDirtyQueue();
	fullScanRequired_ = true;
	nextEntityRefGeneration_ = 1;
}

//-------------------------------------------------------------------------------------
const Position3D& Witness::basePos()
{
	return pEntity()->position();
}

//-------------------------------------------------------------------------------------
const Direction3D& Witness::baseDir()
{
	return pEntity()->direction();
}

//-------------------------------------------------------------------------------------
void Witness::setViewRadius(float radius, float hyst)
{
	if(!g_kbeSrvConfig.getCellApp().use_coordinate_system)
		return;

	viewRadius_ = radius;
	viewHysteresisArea_ = hyst;

	// 由于位置同步使用了相对位置压缩传输，可用范围为-512~512之间，因此超过范围将出现同步错误
	// 这里做一个限制，如果需要过大的数值客户端应该调整坐标单位比例，将其放大使用。
	// 参考: MemoryStream::appendPackXZ
	if(viewRadius_ + viewHysteresisArea_ > 512)
	{
		if (g_kbeSrvConfig.getCellApp().entity_posdir_updates_type > 0)
		{
			viewRadius_ = 512 - 5.0f;
			viewHysteresisArea_ = 5.0f;

			ERROR_MSG(fmt::format("Witness::setViewRadius({}): viewRadius({}) cannot be greater than 512! Beyond 512, please set kbengine[_defaults].xml->entity_posdir_updates->type to 0.\n",
				(pEntity_ ? pEntity_->id() : 0), (viewRadius_ + viewHysteresisArea_)));

			// 不返回，继续生效
			// return;
		}
	}

	if (viewRadius_ > 0.f && pEntity_)
	{
		if (pViewTrigger_ == NULL)
		{
			pViewTrigger_ = new ViewTrigger((CoordinateNode*)pEntity_->pEntityCoordinateNode(), viewRadius_, viewRadius_);

			// 如果实体已经在场景中，那么需要安装
			if (((CoordinateNode*)pEntity_->pEntityCoordinateNode())->pCoordinateSystem())
				pViewTrigger_->install();
		}
		else
		{
			pViewTrigger_->update(viewRadius_, viewRadius_);

			// 如果实体已经在场景中，那么需要安装
			if (!pViewTrigger_->isInstalled() && ((CoordinateNode*)pEntity_->pEntityCoordinateNode())->pCoordinateSystem())
				pViewTrigger_->reinstall((CoordinateNode*)pEntity_->pEntityCoordinateNode());
		}

		if (viewHysteresisArea_ > 0.01f && pEntity_/*上面update流程可能导致销毁 */)
		{
			if (pViewHysteresisAreaTrigger_ == NULL)
			{
				pViewHysteresisAreaTrigger_ = new ViewTrigger((CoordinateNode*)pEntity_->pEntityCoordinateNode(),
					viewHysteresisArea_ + viewRadius_, viewHysteresisArea_ + viewRadius_);

				if (((CoordinateNode*)pEntity_->pEntityCoordinateNode())->pCoordinateSystem())
					pViewHysteresisAreaTrigger_->install();
			}
			else
			{
				pViewHysteresisAreaTrigger_->update(viewHysteresisArea_ + viewRadius_, viewHysteresisArea_ + viewRadius_);

				// 如果实体已经在场景中，那么需要安装
				if (!pViewHysteresisAreaTrigger_->isInstalled() && ((CoordinateNode*)pEntity_->pEntityCoordinateNode())->pCoordinateSystem())
					pViewHysteresisAreaTrigger_->reinstall((CoordinateNode*)pEntity_->pEntityCoordinateNode());
			}
		}
		else
		{
			// 注意：此处如果不销毁pViewHysteresisAreaTrigger_则必须是update
			// 因为离开View的判断如果pViewHysteresisAreaTrigger_存在，那么必须出了pViewHysteresisAreaTrigger_才算出View
			if (pViewHysteresisAreaTrigger_)
				pViewHysteresisAreaTrigger_->update(viewHysteresisArea_ + viewRadius_, viewHysteresisArea_ + viewRadius_);
		}
	}
	else
	{
		uninstallViewTrigger();
	}
}

//-------------------------------------------------------------------------------------
void Witness::onEnterView(ViewTrigger* pViewTrigger, Entity* pEntity)
{
	// 如果进入的是Hysteresis区域，那么不产生作用
	 if (pViewHysteresisAreaTrigger_ == pViewTrigger)
		return;

	// 先增加一个引用，避免实体在回调中被销毁造成后续判断出错
	Py_INCREF(pEntity);

	// 在onEnteredView和addWitnessed可能导致自己销毁然后
	// pEntity_将被设置为NULL，后面没有机会DECREF
	Entity* pSelfEntity = pEntity_;
	Py_INCREF(pSelfEntity);

	VIEW_ENTITIES_MAP::iterator iter = viewEntities_map_.find(pEntity->id());
	if (iter != viewEntities_map_.end())
	{
		EntityRef* pEntityRef = iter->second;
		if ((pEntityRef->flags() & ENTITYREF_FLAG_LEAVE_CLIENT_PENDING) > 0)
		{
			//DEBUG_MSG(fmt::format("Witness::onEnterView: {} entity={}\n", 
			//	pEntity_->id(), pEntity->id()));

			// Leave is still queued and has not reached the client. Coalesce the reversal instead of
			// emitting a synthetic Leave+Enter pair, which shifts every later alias and amplifies AOI
			// churn. Keep the current generation and queue ownership: the retained structural entry
			// becomes a state-skip, while queueEntityRefVolatile adds at most one missing volatile item.
			// Leave 仍在队列中且尚未到达客户端；直接合并反向状态，避免额外 Leave+Enter 使后续
			// 所有别名移位并放大 AOI 抖动。保留当前 generation 与队列所有权：旧结构项自然变为
			// state-skip，queueEntityRefVolatile 最多只补入一个缺失的 volatile 项。
			const bool wasVisibleToClient =
				(pEntityRef->flags() & ENTITYREF_FLAG_NORMAL) > 0;
			pEntityRef->flags(wasVisibleToClient ?
				ENTITYREF_FLAG_NORMAL : ENTITYREF_FLAG_ENTER_CLIENT_PENDING);
			pEntityRef->pEntity(pEntity);
			queueEntityRefVolatile(pEntityRef);
			g_witnessLoadMetrics.recordCancelledPendingLeave();
			pEntity->addWitnessed(pEntity_);
			pSelfEntity->onEnteredView(pEntity);
		}

		Py_DECREF(pEntity);
		Py_DECREF(pSelfEntity);
		return;
	}

	//DEBUG_MSG(fmt::format("Witness::onEnterView: {} entity={}\n", 
	//	pEntity_->id(), pEntity->id()));
	
	EntityRef* pEntityRef = EntityRef::createPoolObject(OBJECTPOOL_POINT);
	pEntityRef->pEntity(pEntity);
	initializeEntityRefLifecycle(pEntityRef);
	pEntityRef->flags(pEntityRef->flags() | ENTITYREF_FLAG_ENTER_CLIENT_PENDING);
	viewEntities_.push_back(pEntityRef);
	viewEntities_map_[pEntityRef->id()] = pEntityRef;
	synchronizeViewEntityMetrics();
	KBE_ASSERT(viewEntities_map_.size() <= static_cast<size_t>(std::numeric_limits<int>::max()));
	// pending-enter 实体尚未存在于客户端别名表，不能占用可见实体的连续编号。
	// A pending-enter entity is absent from the client alias table and must not reserve a visible alias.
	pEntityRef->aliasID(-1);
	queueEntityRefVolatile(pEntityRef);
	
	pEntity->addWitnessed(pEntity_);
	pSelfEntity->onEnteredView(pEntity);

	Py_DECREF(pEntity);
	Py_DECREF(pSelfEntity);
}

//-------------------------------------------------------------------------------------
void Witness::onLeaveView(ViewTrigger* pViewTrigger, Entity* pEntity)
{
	// 如果设置过Hysteresis区域，那么离开Hysteresis区域才算离开View
	if (pViewHysteresisAreaTrigger_ && pViewHysteresisAreaTrigger_ != pViewTrigger)
		return;

	VIEW_ENTITIES_MAP::iterator iter = viewEntities_map_.find(pEntity->id());
	if (iter == viewEntities_map_.end())
		return;

	_onLeaveView(iter->second);
}

//-------------------------------------------------------------------------------------
void Witness::_onLeaveView(EntityRef* pEntityRef)
{
	//DEBUG_MSG(fmt::format("Witness::onLeaveView: {} entity={}\n", 
	//	pEntity_->id(), pEntityRef->id()));

	// 这里不delete， 我们需要待update将此行为更新至客户端时再进行
	//EntityRef::reclaimPoolObject((*iter));
	//viewEntities_.erase(iter);
	//viewEntities_map_.erase(iter);

	pEntityRef->flags(((pEntityRef->flags() | ENTITYREF_FLAG_LEAVE_CLIENT_PENDING) & ~(ENTITYREF_FLAG_ENTER_CLIENT_PENDING)));

	if(pEntityRef->pEntity())
		pEntityRef->pEntity()->delWitnessed(pEntity_);

	pEntityRef->pEntity(NULL);
	queueEntityRefVolatile(pEntityRef);
}

//-------------------------------------------------------------------------------------
void Witness::resetViewEntities()
{
	requireFullScan();
	clientViewSize_ = 0;
	VIEW_ENTITIES::iterator iter = viewEntities_.begin();
	for(; iter != viewEntities_.end(); )
	{
		if(((*iter)->flags() & ENTITYREF_FLAG_LEAVE_CLIENT_PENDING) > 0)
		{
			viewEntities_map_.erase((*iter)->id());
			EntityRef::reclaimPoolObject((*iter));
			iter = viewEntities_.erase(iter);
			continue;
		}

		(*iter)->flags(ENTITYREF_FLAG_ENTER_CLIENT_PENDING);
		++iter;
	}
	synchronizeViewEntityMetrics();
	
	updateEntitiesAliasID();
}

//-------------------------------------------------------------------------------------
void Witness::onEnterSpace(Space* pSpace, bool recordMigrationStages)
{
	const uint64 networkNotifyStart = recordMigrationStages ? timestamp() : 0;
	Network::Bundle* pSendBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
	NETWORK_ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pEntity_->id(), (*pSendBundle));

	// 通知位置强制改变
	Position3D &pos = pEntity_->position();
	Direction3D &dir = pEntity_->direction();
	ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pSendBundle, ClientInterface::onSetEntityPosAndDir, setEntityPosAndDir);
	(*pSendBundle) << pEntity_->id();
	(*pSendBundle) << pos.x << pos.y << pos.z;
	(*pSendBundle) << dir.roll() << dir.pitch() << dir.yaw();
	ENTITY_MESSAGE_FORWARD_CLIENT_END(pSendBundle, ClientInterface::onSetEntityPosAndDir, setEntityPosAndDir);
	
	// 通知进入了新地图
	ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pSendBundle, ClientInterface::onEntityEnterSpace, entityEnterSpace);

	(*pSendBundle) << pEntity_->id();
	(*pSendBundle) << pSpace->id();
	if(!pEntity_->isOnGround())
		(*pSendBundle) << pEntity_->isOnGround();

	ENTITY_MESSAGE_FORWARD_CLIENT_END(pSendBundle, ClientInterface::onEntityEnterSpace, entityEnterSpace);

	// 发送消息并清理
	pEntity_->clientEntityCall()->sendCall(pSendBundle);
	if (recordMigrationStages)
	{
		scriptStageMetrics().record(SCRIPT_STAGE_MIGRATION_WITNESS_NETWORK_NOTIFY,
			scriptStageDurationNanos(networkNotifyStart), true, "reqTeleportToCellApp");
	}

	const uint64 viewTriggerInstallStart = recordMigrationStages ? timestamp() : 0;
	installViewTrigger();
	if (recordMigrationStages)
	{
		scriptStageMetrics().record(SCRIPT_STAGE_MIGRATION_WITNESS_VIEW_TRIGGER_INSTALL,
			scriptStageDurationNanos(viewTriggerInstallStart), true, "reqTeleportToCellApp");
	}
}

//-------------------------------------------------------------------------------------
void Witness::onLeaveSpace(Space* pSpace)
{
	uninstallViewTrigger();

	Network::Bundle* pSendBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
	NETWORK_ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pEntity_->id(), (*pSendBundle));

	ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pSendBundle, ClientInterface::onEntityLeaveSpace, entityLeaveSpace);
	(*pSendBundle) << pEntity_->id();
	ENTITY_MESSAGE_FORWARD_CLIENT_END(pSendBundle, ClientInterface::onEntityLeaveSpace, entityLeaveSpace);
	pEntity_->clientEntityCall()->sendCall(pSendBundle);

	lastBasePos_.z = -FLT_MAX;
	lastBaseDir_.yaw(-FLT_MAX);

	VIEW_ENTITIES::iterator iter = viewEntities_.begin();
	for(; iter != viewEntities_.end(); ++iter)
	{
		if((*iter)->pEntity())
		{
			(*iter)->pEntity()->delWitnessed(pEntity_);
		}

		EntityRef::reclaimPoolObject((*iter));
	}

	viewEntities_.clear();
	viewEntities_map_.clear();
	synchronizeViewEntityMetrics();
	clearVolatileDirtyQueue();
	fullScanRequired_ = true;

	clientViewSize_ = 0;
}

//-------------------------------------------------------------------------------------
void Witness::installViewTrigger()
{
	if (pViewTrigger_)
	{
		// 在设置View半径为0后掉线重登陆会出现这种情况
		if (viewRadius_ <= 0.f)
			return;

		// 必须先安装pViewHysteresisAreaTrigger_，否则一些极端情况会出现错误的结果
		// 例如：一个Avatar正好进入到世界此时正在安装View触发器，而安装过程中这个实体onWitnessed触发导致自身被销毁了
		// 由于View触发器并未完全安装完毕导致触发器的节点old_xx等都为-FLT_MAX，所以该实体在离开坐标管理器时Avatar的View触发器判断错误
		// 如果先安装pViewHysteresisAreaTrigger_则不会触发实体进入View事件，这样在安装pViewTrigger_时触发事件导致上面出现的问题时也能之前捕获离开事件了
		if (pViewHysteresisAreaTrigger_ && pEntity_/*上面流程可能导致销毁 */)
			pViewHysteresisAreaTrigger_->reinstall((CoordinateNode*)pEntity_->pEntityCoordinateNode());

		if (pEntity_/*上面流程可能导致销毁 */)
			pViewTrigger_->reinstall((CoordinateNode*)pEntity_->pEntityCoordinateNode());
	}
	else
	{
		KBE_ASSERT(pViewHysteresisAreaTrigger_ == NULL);
	}
}

//-------------------------------------------------------------------------------------
void Witness::uninstallViewTrigger()
{
	requireFullScan();
	if (pViewTrigger_)
		pViewTrigger_->uninstall();

	if (pViewHysteresisAreaTrigger_)
		pViewHysteresisAreaTrigger_->uninstall();

	// 通知所有实体离开View
	VIEW_ENTITIES::iterator iter = viewEntities_.begin();
	for (; iter != viewEntities_.end(); ++iter)
	{
		_onLeaveView((*iter));
	}
}

//-------------------------------------------------------------------------------------
bool Witness::pushBundle(Network::Bundle* pBundle)
{
	Network::Channel* pc = pChannel();
	if(!pc)
		return false;

	pc->send(pBundle);
	return true;
}

//-------------------------------------------------------------------------------------
Network::Channel* Witness::pChannel()
{
	if(pEntity_ == NULL)
		return NULL;

	EntityCall* clientMB = pEntity_->clientEntityCall();
	if(!clientMB)
		return NULL;

	Network::Channel* pChannel = clientMB->getChannel();
	if(!pChannel)
		return NULL;
	
	return pChannel;
}

//-------------------------------------------------------------------------------------
void Witness::_addViewEntityIDToBundle(Network::Bundle* pBundle, EntityRef* pEntityRef)
{
	if(!EntityDef::entityAliasID())
	{
		(*pBundle) << pEntityRef->id();
	}
	else
	{
		// 注意：不可在该模块外部使用，否则可能出现客户端表找不到entityID的情况
		// clientViewSize_需要实体真正同步到客户端时才会增加
		if(clientViewSize_ > 255)
		{
			(*pBundle) << pEntityRef->id();
		}
		else
		{
			if ((pEntityRef->flags() & (ENTITYREF_FLAG_NORMAL)) > 0)
			{
				KBE_ASSERT(pEntityRef->aliasID() <= 255);
				(*pBundle) << (uint8)pEntityRef->aliasID();
			}
			else
			{
				(*pBundle) << pEntityRef->id();
			}
		}
	}
}

//-------------------------------------------------------------------------------------
const Network::MessageHandler& Witness::getViewEntityMessageHandler(const Network::MessageHandler& normalMsgHandler,
	const Network::MessageHandler& optimizedMsgHandler, ENTITY_ID entityID, int& ialiasID)
{
	(void)optimizedMsgHandler;
	(void)entityID;
	ialiasID = -1;

	// Property updates and RPCs are produced independently from the deferred AOI structural queue.
	// A compacted one-byte view alias can therefore identify a different Entity during intense
	// Enter/Leave churn. Preserve aliases for high-frequency volatile movement, but carry the full
	// EID for semantic messages where misdelivery corrupts state or dispatches the wrong method.
	// 属性更新和 RPC 与延迟 AOI 结构队列独立产生；高频 Enter/Leave 中压紧的一字节别名可能指向另一实体。
	// 高频位姿仍使用别名，而会修改状态或调用方法的语义消息固定携带完整 EID。
	return normalMsgHandler;
}

//-------------------------------------------------------------------------------------
bool Witness::entityID2AliasID(ENTITY_ID id, uint8& aliasID)
{
	VIEW_ENTITIES_MAP::iterator iter = viewEntities_map_.find(id);
	if (iter == viewEntities_map_.end())
	{
		aliasID = 0;
		return false;
	}

	EntityRef* pEntityRef = iter->second;
	if ((pEntityRef->flags() & (ENTITYREF_FLAG_NORMAL)) <= 0)
	{
		aliasID = 0;
		return false;
	}

	// 溢出
	if (pEntityRef->aliasID() > 255)
	{
		aliasID = 0;
		return false;
	}
	
	aliasID = (uint8)pEntityRef->aliasID();
	return true;
}

//-------------------------------------------------------------------------------------
void Witness::updateEntitiesAliasID(int removedAliasID)
{
	VIEW_ENTITIES::iterator iter = viewEntities_.begin();
	for(; iter != viewEntities_.end(); ++iter)
	{
		EntityRef* pEntityRef = (*iter);
		if ((pEntityRef->flags() & ENTITYREF_FLAG_NORMAL) == 0)
		{
			pEntityRef->aliasID(-1);
			continue;
		}

		// 客户端以 EnterWorld 处理顺序尾插，并在 LeaveWorld 时 vector::erase；空间容器顺序不能重建该表。
		// The client appends in EnterWorld order and uses vector::erase on leave; spatial container order cannot rebuild it.
		if (removedAliasID >= 0 && pEntityRef->aliasID() > removedAliasID)
			pEntityRef->aliasID(pEntityRef->aliasID() - 1);
	}
}

//-------------------------------------------------------------------------------------
void Witness::requireFullScan()
{
	fullScanRequired_ = true;
}

//-------------------------------------------------------------------------------------
void Witness::prepareFullScanQueue()
{
	// 全量恢复只重建待发送顺序，不在一个 Tick 内直接序列化整个视野。
	// A full recovery rebuilds send order only; it must not serialize the entire view in one tick.
	clearVolatileDirtyQueue();
	const size_t scannedEntities = viewEntities_.size();
	for (VIEW_ENTITIES::iterator iter = viewEntities_.begin(); iter != viewEntities_.end(); ++iter)
	{
		(*iter)->volatileQueued(false);
		(*iter)->structuralQueued(false);
		queueEntityRefVolatile(*iter);
	}

	fullScanRequired_ = false;
	g_witnessLoadMetrics.recordFullScan(scannedEntities);
}

//-------------------------------------------------------------------------------------
void Witness::clearVolatileDirtyQueue()
{
	VIEW_ENTITIES::iterator iter = viewEntities_.begin();
	for (; iter != viewEntities_.end(); ++iter)
	{
		if ((*iter)->volatileQueued() && (*iter)->pEntity())
			(*iter)->pEntity()->onWitnessVolatileDequeued();
		(*iter)->volatileQueued(false);
		(*iter)->structuralQueued(false);
	}

	const size_t volatileQueuedCount = volatileDirtyQueue_.size();
	const size_t structuralQueuedCount = structuralDirtyQueue_.size();
	g_witnessLoadMetrics.recordDirtyDequeued(volatileQueuedCount, false);
	g_witnessLoadMetrics.recordDirtyDequeued(structuralQueuedCount, true);
	volatileDirtyQueue_.clear();
	structuralDirtyQueue_.clear();
}

//-------------------------------------------------------------------------------------
void Witness::synchronizeViewEntityMetrics()
{
	g_witnessLoadMetrics.synchronizeViewCount(trackedViewEntityCount_, viewEntities_.size());
}

//-------------------------------------------------------------------------------------
void Witness::initializeEntityRefLifecycle(EntityRef* pEntityRef)
{
	// generation 将队列条目绑定到一次可见生命周期，避免对象池地址复用或同 ID 重入让旧条目命中新引用。
	// The generation binds a queue entry to one visibility lifetime, preventing pooled-address reuse or same-ID re-entry from matching a stale entry.
	if (nextEntityRefGeneration_ == 0)
		++nextEntityRefGeneration_;

	pEntityRef->generation(nextEntityRefGeneration_++);
	pEntityRef->volatileQueued(false);
	pEntityRef->structuralQueued(false);
}

//-------------------------------------------------------------------------------------
void Witness::queueEntityRefVolatile(EntityRef* pEntityRef, bool requeue)
{
	// 结构消息必须绕过 volatile 背压，因此使用独立优先队列；generation + 双 queued 标记
	// 允许结构事件提升已经排队的 volatile 项，而不需要在线性容器中搜索和删除旧条目。
	// Structural messages must bypass volatile backpressure, so they use a dedicated priority queue.
	// Generation plus two queued flags promotes an already queued volatile item without searching a linear container.
	if (!pEntityRef || pEntityRef->flags() == ENTITYREF_FLAG_UNKONWN)
		return;

	if (isStructuralUpdate(pEntityRef))
	{
		const bool promoted = pEntityRef->volatileQueued();
		if (structuralDirtyQueue_.enqueue(
			pEntityRef->id(), pEntityRef->generation(), pEntityRef->structuralQueuedRef()))
		{
			g_witnessLoadMetrics.recordDirtyEnqueued(
				volatileDirtyQueue_.size() + structuralDirtyQueue_.size(), requeue, true, promoted);
		}
		else
		{
			g_witnessLoadMetrics.recordQueueDeduplicated();
		}
		return;
	}

	if (volatileDirtyQueue_.enqueue(
		pEntityRef->id(), pEntityRef->generation(), pEntityRef->volatileQueuedRef()))
	{
		g_witnessLoadMetrics.recordDirtyEnqueued(
			volatileDirtyQueue_.size() + structuralDirtyQueue_.size(), requeue, false, false);
	}
	else
	{
		g_witnessLoadMetrics.recordQueueDeduplicated();
	}
}

//-------------------------------------------------------------------------------------
void Witness::setVolatileUpdatesEnabled(bool enabled)
{
	if (volatileUpdatesEnabled_ == enabled)
		return;

	volatileUpdatesEnabled_ = enabled;
	g_witnessLoadMetrics.recordVolatileSuppression(!enabled);
}

//-------------------------------------------------------------------------------------
void Witness::markViewEntityVolatileDirty(ENTITY_ID entityID)
{
	VIEW_ENTITIES_MAP::iterator iter = viewEntities_map_.find(entityID);
	if (iter != viewEntities_map_.end())
		queueEntityRefVolatile(iter->second);
}

//-------------------------------------------------------------------------------------
bool Witness::needsVolatileUpdate(Entity* pEntity)
{
	return getEntityVolatileDataUpdateFlags(pEntity) != UPDATE_FLAG_NULL;
}

//-------------------------------------------------------------------------------------
bool Witness::isStructuralUpdate(const EntityRef* pEntityRef) const
{
	const uint32 structuralFlags = ENTITYREF_FLAG_ENTER_CLIENT_PENDING | ENTITYREF_FLAG_LEAVE_CLIENT_PENDING;
	return (pEntityRef->flags() & structuralFlags) != 0;
}

//-------------------------------------------------------------------------------------
bool Witness::processEntityRefUpdate(Network::Bundle* pSendBundle, EntityRef* pEntityRef)
{
	if ((pEntityRef->flags() & ENTITYREF_FLAG_ENTER_CLIENT_PENDING) > 0)
	{
		// 进入通知必须重新按 ID 查找实体；脚本回调可能已经销毁原对象，不能使用缓存指针。
		// Enter notification must resolve the entity by ID because script callbacks may have destroyed the cached object.
		Entity* pOtherEntity = Cellapp::getSingleton().findEntity(pEntityRef->id());
		if (pOtherEntity == NULL)
			return false;

		pEntityRef->removeflags(ENTITYREF_FLAG_ENTER_CLIENT_PENDING);

		MemoryStream* pStream = MemoryStream::createPoolObject(OBJECTPOOL_POINT);
		pOtherEntity->addPositionAndDirectionToStream(*pStream, true);
		pOtherEntity->addClientDataToStream(pStream, true);

		ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pSendBundle, ClientInterface::onUpdatePropertys, updatePropertys);
		(*pSendBundle) << pOtherEntity->id();
		(*pSendBundle).append(*pStream);
		MemoryStream::reclaimPoolObject(pStream);
		ENTITY_MESSAGE_FORWARD_CLIENT_END(pSendBundle, ClientInterface::onUpdatePropertys, updatePropertys);

		ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pSendBundle, ClientInterface::onEntityEnterWorld, entityEnterWorld);
		(*pSendBundle) << pOtherEntity->id();
		pOtherEntity->pScriptModule()->addSmartUTypeToBundle(pSendBundle);
		if (!pOtherEntity->isOnGround())
			(*pSendBundle) << pOtherEntity->isOnGround();
		ENTITY_MESSAGE_FORWARD_CLIENT_END(pSendBundle, ClientInterface::onEntityEnterWorld, entityEnterWorld);

		pEntityRef->flags(ENTITYREF_FLAG_NORMAL);
		// EnterWorld 在客户端按尾插建立别名；这里必须使用发送前的客户端可见数量。
		// EnterWorld appends to the client alias table, so use the visible count before incrementing it.
		pEntityRef->aliasID(static_cast<int>(clientViewSize_));
		KBE_ASSERT(clientViewSize_ != 65535);
		++clientViewSize_;

		if (needsVolatileUpdate(pOtherEntity))
			queueEntityRefVolatile(pEntityRef, true);

		return true;
	}

	if ((pEntityRef->flags() & ENTITYREF_FLAG_LEAVE_CLIENT_PENDING) > 0)
	{
		pEntityRef->removeflags(ENTITYREF_FLAG_LEAVE_CLIENT_PENDING);
		if ((pEntityRef->flags() & ENTITYREF_FLAG_NORMAL) > 0)
		{
			// Leave changes the client's alias table and therefore must identify the entity by its
			// stable ID. A stale one-byte alias can erase a different entity and corrupt every later
			// alias; the three-byte saving is not worth that structural risk under heavy AOI churn.
			// Leave 会改变客户端别名表，因此必须使用稳定的完整实体 ID。过期的一字节别名会
			// 删除错误实体并污染后续所有别名；在高频 AOI 抖动下不值得为节省三字节承担该风险。
			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pSendBundle, ClientInterface::onEntityLeaveWorld, leaveWorld);
			(*pSendBundle) << pEntityRef->id();
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pSendBundle, ClientInterface::onEntityLeaveWorld, leaveWorld);

			KBE_ASSERT(clientViewSize_ > 0);
			--clientViewSize_;
		}

		return false;
	}

	Entity* pOtherEntity = pEntityRef->pEntity();
	if (pOtherEntity == NULL)
	{
		KBE_ASSERT(clientViewSize_ > 0);
		--clientViewSize_;
		return false;
	}

	KBE_ASSERT(pEntityRef->flags() == ENTITYREF_FLAG_NORMAL);
	const uint32 flags = getEntityVolatileDataUpdateFlags(pOtherEntity);
	addUpdateToStream(pSendBundle, flags, pEntityRef);
	if (flags != UPDATE_FLAG_NULL)
		queueEntityRefVolatile(pEntityRef, true);

	return true;
}

//-------------------------------------------------------------------------------------
void Witness::removeViewEntityRef(EntityRef* pEntityRef)
{
	const int removedAliasID = pEntityRef->aliasID();
	viewEntities_map_.erase(pEntityRef->id());
	for (VIEW_ENTITIES::iterator iter = viewEntities_.begin(); iter != viewEntities_.end(); ++iter)
	{
		if (*iter == pEntityRef)
		{
			viewEntities_.erase(iter);
			break;
		}
	}

	EntityRef::reclaimPoolObject(pEntityRef);
	synchronizeViewEntityMetrics();
	updateEntitiesAliasID(removedAliasID);
}

//-------------------------------------------------------------------------------------
void Witness::processVolatileDirtyQueue(Network::Bundle* pSendBundle)
{
	if (!volatileUpdatesEnabled_ && structuralDirtyQueue_.size() == 0)
	{
		// 没有结构事件时保持 volatile 队列静止，避免拥塞期间扫描任何普通位姿项。
		// Keep the volatile queue stationary when no structural event exists, avoiding all normal-position scans while congested.
		g_witnessLoadMetrics.recordSuppressedUpdateSkip();
		return;
	}

	const EngineComponentInfo& config = g_kbeSrvConfig.getCellApp();
	WitnessVolatileBudget volatileBudget(config.witness_volatile_bytes_per_tick);
	WitnessVolatileBudget sendBudget(witnessEffectiveByteLimit(
		config.witness_total_bytes_per_tick,
		config.witness_global_bytes_per_tick,
		g_witnessActiveCount));
	bool volatileExhaustionRecorded = false;

	// Enter/Leave 独立排队并始终先于 volatile 项处理。结构队列只消费 Tick 开始时的快照，
	// 回调中新产生的结构事件留到下一 Tick，保持单 Tick 工作量有界。
	// Enter/Leave use a separate priority queue. Only the tick-start snapshot is consumed;
	// structural work created by callbacks remains for the next tick, keeping one tick bounded.
	const size_t structuralBatchSize = structuralDirtyQueue_.batchSize();
	for (size_t i = 0; i < structuralBatchSize; ++i)
	{
		if (!sendBudget.canSend(false))
		{
			g_witnessLoadMetrics.recordSendBudgetExhaustion();
			break;
		}

		WitnessDirtyQueue::Entry entry;
		if (!structuralDirtyQueue_.pop(entry))
			break;
		g_witnessLoadMetrics.recordDirtyDequeued(1, true);

		VIEW_ENTITIES_MAP::iterator iter = viewEntities_map_.find(entry.entityID);
		if (iter == viewEntities_map_.end() || iter->second->generation() != entry.generation)
		{
			g_witnessLoadMetrics.recordStaleDiscard();
			continue;
		}

		EntityRef* pEntityRef = iter->second;
		pEntityRef->structuralQueued(false);
		if (!isStructuralUpdate(pEntityRef))
		{
			g_witnessLoadMetrics.recordStateSkip();
			continue;
		}

		const size_t beforeBytes = static_cast<size_t>(pSendBundle->currMsgLength());
		const uint32 flagsBeforeUpdate = pEntityRef->flags();
		const bool isEnter = (flagsBeforeUpdate & ENTITYREF_FLAG_ENTER_CLIENT_PENDING) != 0;
		const bool sampleProcessing = isEnter ?
			g_witnessLoadMetrics.beginEnterProcessing() :
			g_witnessLoadMetrics.beginLeaveProcessing();
		const uint64 processingStarted = sampleProcessing ? timestamp() : 0;
		const bool retained = processEntityRefUpdate(pSendBundle, pEntityRef);
		if (sampleProcessing)
		{
			const uint64 durationNanos = static_cast<uint64>(
				static_cast<long double>(timestamp() - processingStarted) * 1000000000.0L /
				stampsPerSecondD());
			if (isEnter)
				g_witnessLoadMetrics.recordEnterProcessing(durationNanos);
			else
				g_witnessLoadMetrics.recordLeaveProcessing(durationNanos);
		}
		const size_t afterBytes = static_cast<size_t>(pSendBundle->currMsgLength());
		const uint64 encodedBytes = afterBytes > beforeBytes ? static_cast<uint64>(afterBytes - beforeBytes) : 0;
		if (isEnter)
			g_witnessLoadMetrics.recordEnter(encodedBytes);
		else
			g_witnessLoadMetrics.recordLeave(encodedBytes);

		sendBudget.recordBundleGrowth(beforeBytes, afterBytes);
		g_witnessLoadMetrics.recordStructuralProcessed();
		if (!volatileUpdatesEnabled_)
			g_witnessLoadMetrics.recordStructuralWhileSuppressed();
		g_witnessLoadMetrics.recordDirtyProcessed();
		if (!retained)
			removeViewEntityRef(pEntityRef);
	}

	if (!volatileUpdatesEnabled_)
	{
		g_witnessLoadMetrics.recordVolatileBytes(volatileBudget.bytesSent());
		g_witnessLoadMetrics.recordSendBytes(sendBudget.bytesSent());
		return;
	}

	const size_t batchSize = volatileDirtyQueue_.batchSize();
	for (size_t i = 0; i < batchSize; ++i)
	{
		// 总预算采用软上限：上一条完整消息达到上限后停止，绝不拆断协议消息。
		// The total budget is soft: stop after the previous complete message reaches it; never split protocol messages.
		if (!sendBudget.canSend(false))
		{
			g_witnessLoadMetrics.recordSendBudgetExhaustion();
			break;
		}

		WitnessDirtyQueue::Entry entry;
		if (!volatileDirtyQueue_.pop(entry))
			break;
		g_witnessLoadMetrics.recordDirtyDequeued(1, false);

		VIEW_ENTITIES_MAP::iterator iter = viewEntities_map_.find(entry.entityID);
		if (iter == viewEntities_map_.end() || iter->second->generation() != entry.generation)
		{
			g_witnessLoadMetrics.recordStaleDiscard();
			continue;
		}

		EntityRef* pEntityRef = iter->second;
		if (pEntityRef->volatileQueued() && pEntityRef->pEntity())
			pEntityRef->pEntity()->onWitnessVolatileDequeued();
		pEntityRef->volatileQueued(false);
		if (isStructuralUpdate(pEntityRef))
		{
			// 该项已被提升到结构队列。保留的旧 volatile 环形条目只需跳过一次，
			// 不再执行出队再入队的 O(queue) 放大路径。
			// This item was promoted to the structural queue. Its retained volatile ring entry
			// is skipped once instead of entering the dequeue/requeue amplification path.
			if (!pEntityRef->structuralQueued())
				queueEntityRefVolatile(pEntityRef, true);
			g_witnessLoadMetrics.recordPromotedVolatileSkip();
			continue;
		}
		if (!volatileBudget.canSend(false))
		{
			// 普通易变更新回队后在下 Tick 读取实体最新状态，不保留中间位置快照。
			// Deferred volatile entries read the entity's latest state next tick without retaining intermediate positions.
			queueEntityRefVolatile(pEntityRef, true);
			g_witnessLoadMetrics.recordVolatileBudgetDeferred();
			if (!volatileExhaustionRecorded)
			{
				g_witnessLoadMetrics.recordVolatileBudgetExhaustion();
				volatileExhaustionRecorded = true;
			}
			continue;
		}

		// 外层实体转发消息尚未完成时 packetsLength() 只反映分包边界，currMsgLength() 才是连续增长的真实编码字节数。
		// While the outer entity-forward message is open, packetsLength() reflects packet boundaries; currMsgLength() is the continuously growing encoded byte count.
		const size_t beforeBytes = static_cast<size_t>(pSendBundle->currMsgLength());
		const bool retained = processEntityRefUpdate(pSendBundle, pEntityRef);
		const size_t afterBytes = static_cast<size_t>(pSendBundle->currMsgLength());
		const uint64 encodedBytes = afterBytes > beforeBytes ? static_cast<uint64>(afterBytes - beforeBytes) : 0;
		g_witnessLoadMetrics.recordVolatileUpdate(encodedBytes);
		sendBudget.recordBundleGrowth(beforeBytes, afterBytes);
		volatileBudget.recordBundleGrowth(beforeBytes, afterBytes);
		g_witnessLoadMetrics.recordDirtyProcessed();
		if (!retained)
			removeViewEntityRef(pEntityRef);
	}

	g_witnessLoadMetrics.recordVolatileBytes(volatileBudget.bytesSent());
	g_witnessLoadMetrics.recordSendBytes(sendBudget.bytesSent());
}

//-------------------------------------------------------------------------------------
uint64 Witness::activeCount()
{
	return g_witnessActiveCount;
}

//-------------------------------------------------------------------------------------
uint64 Witness::dirtyQueuedCount()
{
	return g_witnessLoadMetrics.dirtyQueued();
}

//-------------------------------------------------------------------------------------
uint64 Witness::fullScanCount()
{
	return g_witnessLoadMetrics.fullScans();
}

//-------------------------------------------------------------------------------------
uint64 Witness::fullScanEntityCount()
{
	return g_witnessLoadMetrics.fullScanEntities();
}

//-------------------------------------------------------------------------------------
uint64 Witness::dirtyProcessedCount()
{
	return g_witnessLoadMetrics.dirtyProcessed();
}

//-------------------------------------------------------------------------------------
uint64 Witness::maxQueueDepth()
{
	return g_witnessLoadMetrics.maxQueueDepth();
}

//-------------------------------------------------------------------------------------
uint64 Witness::viewEntityCount()
{
	return g_witnessLoadMetrics.viewEntities();
}

//-------------------------------------------------------------------------------------
uint64 Witness::maxViewEntityCount()
{
	return g_witnessLoadMetrics.maxViewEntities();
}

//-------------------------------------------------------------------------------------
uint64 Witness::dirtyEnqueuedCount()
{
	return g_witnessLoadMetrics.dirtyEnqueued();
}

//-------------------------------------------------------------------------------------
uint64 Witness::dirtyRequeueCount()
{
	return g_witnessLoadMetrics.dirtyRequeues();
}

//-------------------------------------------------------------------------------------
uint64 Witness::staleDiscardCount()
{
	return g_witnessLoadMetrics.staleDiscards();
}

//-------------------------------------------------------------------------------------
uint64 Witness::stateSkipCount()
{
	return g_witnessLoadMetrics.stateSkips();
}

//-------------------------------------------------------------------------------------
uint64 Witness::volatileBytesSentCount()
{
	return g_witnessLoadMetrics.volatileBytesSent();
}

//-------------------------------------------------------------------------------------
uint64 Witness::volatileBudgetDeferredCount()
{
	return g_witnessLoadMetrics.volatileBudgetDeferred();
}

//-------------------------------------------------------------------------------------
uint64 Witness::volatileBudgetExhaustionCount()
{
	return g_witnessLoadMetrics.volatileBudgetExhaustions();
}

//-------------------------------------------------------------------------------------
uint64 Witness::sendBytesCount()
{
	return g_witnessLoadMetrics.sendBytes();
}

//-------------------------------------------------------------------------------------
uint64 Witness::sendBudgetExhaustionCount()
{
	return g_witnessLoadMetrics.sendBudgetExhaustions();
}

//-------------------------------------------------------------------------------------
uint64 Witness::structuralProcessedCount()
{
	return g_witnessLoadMetrics.structuralProcessed();
}

//-------------------------------------------------------------------------------------
uint64 Witness::structuralQueuedCount() { return g_witnessLoadMetrics.structuralQueued(); }
uint64 Witness::volatileQueuedCount() { return g_witnessLoadMetrics.volatileQueued(); }
uint64 Witness::structuralEnqueuedCount() { return g_witnessLoadMetrics.structuralEnqueued(); }
uint64 Witness::volatileEnqueuedCount() { return g_witnessLoadMetrics.volatileEnqueued(); }
uint64 Witness::queueDeduplicatedCount() { return g_witnessLoadMetrics.queueDeduplicated(); }
uint64 Witness::producerCoalescedCount() { return g_witnessLoadMetrics.producerCoalesced(); }
void Witness::recordProducerCoalesced() { g_witnessLoadMetrics.recordProducerCoalesced(); }
uint64 Witness::structuralPromotionCount() { return g_witnessLoadMetrics.structuralPromotions(); }
uint64 Witness::promotedVolatileSkipCount() { return g_witnessLoadMetrics.promotedVolatileSkips(); }
uint64 Witness::cancelledPendingLeaveCount() { return g_witnessLoadMetrics.cancelledPendingLeaves(); }

//-------------------------------------------------------------------------------------
void Witness::beginUpdateTick()
{
	g_witnessUpdateScheduler.beginTick(
		g_witnessActiveCount, g_kbeSrvConfig.getCellApp().witness_global_updates_per_tick);
}

//-------------------------------------------------------------------------------------
uint64 Witness::globalAdmittedCount() { return g_witnessLoadMetrics.globalAdmitted(); }
uint64 Witness::globalDeferredCount() { return g_witnessLoadMetrics.globalDeferred(); }
uint64 Witness::globalUpdateLimit() { return g_kbeSrvConfig.getCellApp().witness_global_updates_per_tick; }
uint64 Witness::enterUpdateCount() { return g_witnessLoadMetrics.enterUpdates(); }
uint64 Witness::enterBytesCount() { return g_witnessLoadMetrics.enterBytes(); }
uint64 Witness::leaveUpdateCount() { return g_witnessLoadMetrics.leaveUpdates(); }
uint64 Witness::leaveBytesCount() { return g_witnessLoadMetrics.leaveBytes(); }
uint64 Witness::enterProcessingSampleRate() { return g_witnessLoadMetrics.enterProcessing().sampleRate(); }
uint64 Witness::enterProcessingSamples() { return g_witnessLoadMetrics.enterProcessing().sampledCalls(); }
uint64 Witness::enterProcessingTotalNanos() { return g_witnessLoadMetrics.enterProcessing().sampledTotalNanos(); }
uint64 Witness::enterProcessingAverageNanos() { return g_witnessLoadMetrics.enterProcessing().sampledAverageNanos(); }
uint64 Witness::enterProcessingMaxNanos() { return g_witnessLoadMetrics.enterProcessing().sampledMaxNanos(); }
uint64 Witness::enterProcessingSlowSamplesOver1ms() { return g_witnessLoadMetrics.enterProcessing().slowSamplesOver1ms(); }
uint64 Witness::leaveProcessingSampleRate() { return g_witnessLoadMetrics.leaveProcessing().sampleRate(); }
uint64 Witness::leaveProcessingSamples() { return g_witnessLoadMetrics.leaveProcessing().sampledCalls(); }
uint64 Witness::leaveProcessingTotalNanos() { return g_witnessLoadMetrics.leaveProcessing().sampledTotalNanos(); }
uint64 Witness::leaveProcessingAverageNanos() { return g_witnessLoadMetrics.leaveProcessing().sampledAverageNanos(); }
uint64 Witness::leaveProcessingMaxNanos() { return g_witnessLoadMetrics.leaveProcessing().sampledMaxNanos(); }
uint64 Witness::leaveProcessingSlowSamplesOver1ms() { return g_witnessLoadMetrics.leaveProcessing().slowSamplesOver1ms(); }
uint64 Witness::volatileUpdateCount() { return g_witnessLoadMetrics.volatileUpdates(); }
uint64 Witness::volatileUpdateBytesCount() { return g_witnessLoadMetrics.volatileUpdateBytes(); }

//-------------------------------------------------------------------------------------
uint64 Witness::activeSuppressedCount()
{
	return g_witnessLoadMetrics.activeSuppressed();
}

//-------------------------------------------------------------------------------------
uint64 Witness::suppressionTransitionCount()
{
	return g_witnessLoadMetrics.suppressionTransitions();
}

//-------------------------------------------------------------------------------------
uint64 Witness::resumeTransitionCount()
{
	return g_witnessLoadMetrics.resumeTransitions();
}

//-------------------------------------------------------------------------------------
uint64 Witness::suppressedUpdateSkipCount()
{
	return g_witnessLoadMetrics.suppressedUpdateSkips();
}

//-------------------------------------------------------------------------------------
uint64 Witness::structuralWhileSuppressedCount()
{
	return g_witnessLoadMetrics.structuralWhileSuppressed();
}

//-------------------------------------------------------------------------------------
uint64 Witness::bundlesSentCount()
{
	return g_witnessLoadMetrics.bundlesSent();
}

//-------------------------------------------------------------------------------------
uint64 Witness::maxBundleBytes()
{
	return g_witnessLoadMetrics.maxBundleBytes();
}

//-------------------------------------------------------------------------------------
bool Witness::update()
{
	SCOPED_PROFILE(CLIENT_UPDATE_PROFILE);

	const bool globallyAdmitted = g_witnessUpdateScheduler.admit();
	g_witnessLoadMetrics.recordGlobalAdmission(globallyAdmitted);
	if (!globallyAdmitted)
		return true;

	if(pEntity_ == NULL || !pEntity_->clientEntityCall())
		return true;

	Network::Channel* pChannel = pEntity_->clientEntityCall()->getChannel();
	if(!pChannel)
		return true;

	Py_INCREF(pEntity_);

	static bool notificationScriptBegin = PyObject_HasAttrString(pEntity_, "onUpdateBegin") > 0;
	if (notificationScriptBegin)
	{
		PyObject* pyResult = PyObject_CallMethod(pEntity_,
			const_cast<char*>("onUpdateBegin"),
			const_cast<char*>(""));

		if (pyResult != NULL)
		{
			AsyncioHelper::submitCoroutine(pyResult);
			Py_DECREF(pyResult);
		}
		else
		{
			SCRIPT_ERROR_CHECK();
		}
	}

	if (viewEntities_map_.size() > 0 || pEntity_->isControlledNotSelfClient())
	{
		if (fullScanRequired_)
			prepareFullScanQueue();

		Network::Bundle* pSendBundle = pChannel->createSendBundle();
		
		// 得到当前pSendBundle中是否有数据，如果有数据表示该bundle是重用的缓存的数据包
		bool isBufferedSendBundleMessageLength = pSendBundle->packets().size() > 0 ? true : 
			(pSendBundle->pCurrPacket() && pSendBundle->pCurrPacket()->length() > 0);
		
		NETWORK_ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pEntity_->id(), (*pSendBundle));
		if (volatileUpdatesEnabled_)
			addBaseDataToStream(pSendBundle);

		processVolatileDirtyQueue(pSendBundle);

		size_t pSendBundleMessageLength = pSendBundle->currMsgLength();
		if (pSendBundleMessageLength > 8/*NETWORK_ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN产生的基础包大小*/)
		{
			// 超过单包大小会由网络层正常分包；热路径只累计 Watcher 指标，避免日志 IO 放大拥塞。
			// The network layer fragments oversized bundles normally; retain metrics without hot-path log amplification.
			g_witnessLoadMetrics.recordBundle(pSendBundleMessageLength);
			AUTO_SCOPED_PROFILE("sendToClient");
			pChannel->send(pSendBundle);
		}
		else
		{
			// 如果bundle是channel缓存的包
			// 取出来重复利用的如果想丢弃本次消息发送
			// 此时应该将NETWORK_ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN从其中抹除掉
			if(isBufferedSendBundleMessageLength)
			{
				KBE_ASSERT(pSendBundleMessageLength == 8);
				pSendBundle->revokeMessage(8);
				pChannel->pushBundle(pSendBundle);
			}
			else
			{
				Network::Bundle::reclaimPoolObject(pSendBundle);
			}
		}
	}

	static bool notificationScriptEnd = PyObject_HasAttrString(pEntity_, "onUpdateEnd") > 0;
	if (notificationScriptEnd)
	{
		PyObject* pyResult = PyObject_CallMethod(pEntity_,
			const_cast<char*>("onUpdateEnd"),
			const_cast<char*>(""));

		if (pyResult != NULL)
		{
			AsyncioHelper::submitCoroutine(pyResult);
			Py_DECREF(pyResult);
		}
		else
		{
			SCRIPT_ERROR_CHECK();
		}
	}

	Py_DECREF(pEntity_);
	return true;
}

//-------------------------------------------------------------------------------------
void Witness::addBaseDataToStream(Network::Bundle* pSendBundle)
{
	if (pEntity_->isControlledNotSelfClient())
	{
		const Direction3D& bdir = baseDir();
		Vector3 changeDir = bdir.dir - lastBaseDir_.dir;

		if (KBEVec3Length(&changeDir) > 0.0004f)
		{
			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pSendBundle, ClientInterface::onUpdateBaseDir, onUpdateBaseDir);
			(*pSendBundle) << bdir.yaw() << bdir.pitch() << bdir.roll();
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pSendBundle, ClientInterface::onUpdateBaseDir, onUpdateBaseDir);
			lastBaseDir_ = bdir;
		}
	}

	const Position3D& bpos = basePos();
	Vector3 movement = bpos - lastBasePos_;

	if(KBEVec3Length(&movement) < 0.0004f)
		return;

	if (fabs(lastBasePos_.y - bpos.y) > 0.0004f)
	{
		ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pSendBundle, ClientInterface::onUpdateBasePos, basePos);
		pSendBundle->appendPackAnyXYZ(bpos.x, bpos.y, bpos.z, 0.f);
		ENTITY_MESSAGE_FORWARD_CLIENT_END(pSendBundle, ClientInterface::onUpdateBasePos, basePos);
	}
	else
	{
		ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pSendBundle, ClientInterface::onUpdateBasePosXZ, basePos);
		pSendBundle->appendPackAnyXZ(bpos.x, bpos.z, 0.f);
		ENTITY_MESSAGE_FORWARD_CLIENT_END(pSendBundle, ClientInterface::onUpdateBasePosXZ, basePos);
	}

	lastBasePos_ = bpos;
}

//-------------------------------------------------------------------------------------
void Witness::addUpdateToStream(Network::Bundle* pForwardBundle, uint32 flags, EntityRef* pEntityRef)
{
	Entity* otherEntity = pEntityRef->pEntity();

	static uint16 type = g_kbeSrvConfig.getCellApp().entity_posdir_updates_type;
	static uint16 threshold = g_kbeSrvConfig.getCellApp().entity_posdir_updates_smart_threshold;
	
	bool isOptimized = true;
	if ((type == 2 && clientViewSize_ <= threshold) || type == 0)
	{
		isOptimized = false;
	} 
	
	if (isOptimized)
	{
		switch (flags)
		{
		case UPDATE_FLAG_NULL:
		{
			// (*pForwardBundle).newMessage(ClientInterface::onUpdateData);
		}
		break;
		case UPDATE_FLAG_XZ:
		{
			Position3D relativePos = otherEntity->position() - this->pEntity()->position();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xz_optimized, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			pForwardBundle->appendPackXZ(relativePos.x, relativePos.z);
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xz_optimized, update);
		}
		break;
		case UPDATE_FLAG_XYZ:
		{
			Position3D relativePos = otherEntity->position() - this->pEntity()->position();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xyz_optimized, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			pForwardBundle->appendPackXZ(relativePos.x, relativePos.z);
			pForwardBundle->appendPackY(relativePos.y);
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xyz_optimized, update);
		}
		break;
		case UPDATE_FLAG_YAW:
		{
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_y_optimized, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << angle2int8(dir.yaw());
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_y_optimized, update);
		}
		break;
		case UPDATE_FLAG_ROLL:
		{
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_r_optimized, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << angle2int8(dir.roll());
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_r_optimized, update);
		}
		break;
		case UPDATE_FLAG_PITCH:
		{
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_p_optimized, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << angle2int8(dir.pitch());
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_p_optimized, update);
		}
		break;
		case UPDATE_FLAG_YAW_PITCH_ROLL:
		{
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_ypr_optimized, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << angle2int8(dir.yaw());
			(*pForwardBundle) << angle2int8(dir.pitch());
			(*pForwardBundle) << angle2int8(dir.roll());
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_ypr_optimized, update);
		}
		break;
		case UPDATE_FLAG_YAW_PITCH:
		{
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_yp_optimized, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << angle2int8(dir.yaw());
			(*pForwardBundle) << angle2int8(dir.pitch());
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_yp_optimized, update);
		}
		break;
		case UPDATE_FLAG_YAW_ROLL:
		{
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_yr_optimized, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << angle2int8(dir.yaw());
			(*pForwardBundle) << angle2int8(dir.roll());
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_yr_optimized, update);
		}
		break;
		case UPDATE_FLAG_PITCH_ROLL:
		{
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_pr_optimized, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << angle2int8(dir.pitch());
			(*pForwardBundle) << angle2int8(dir.roll());
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_pr_optimized, update);
		}
		break;
		case (UPDATE_FLAG_XZ | UPDATE_FLAG_YAW):
		{
			Position3D relativePos = otherEntity->position() - this->pEntity()->position();
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xz_y_optimized, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			pForwardBundle->appendPackXZ(relativePos.x, relativePos.z);
			(*pForwardBundle) << angle2int8(dir.yaw());
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xz_y_optimized, update);
		}
		break;
		case (UPDATE_FLAG_XZ | UPDATE_FLAG_PITCH):
		{
			Position3D relativePos = otherEntity->position() - this->pEntity()->position();
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xz_p_optimized, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			pForwardBundle->appendPackXZ(relativePos.x, relativePos.z);
			(*pForwardBundle) << angle2int8(dir.pitch());
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xz_p_optimized, update);
		}
		break;
		case (UPDATE_FLAG_XZ | UPDATE_FLAG_ROLL):
		{
			Position3D relativePos = otherEntity->position() - this->pEntity()->position();
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xz_r_optimized, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			pForwardBundle->appendPackXZ(relativePos.x, relativePos.z);
			(*pForwardBundle) << angle2int8(dir.roll());
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xz_r_optimized, update);
		}
		break;
		case (UPDATE_FLAG_XZ | UPDATE_FLAG_YAW_ROLL):
		{
			Position3D relativePos = otherEntity->position() - this->pEntity()->position();
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xz_yr_optimized, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			pForwardBundle->appendPackXZ(relativePos.x, relativePos.z);
			(*pForwardBundle) << angle2int8(dir.yaw());
			(*pForwardBundle) << angle2int8(dir.roll());
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xz_yr_optimized, update);
		}
		break;
		case (UPDATE_FLAG_XZ | UPDATE_FLAG_YAW_PITCH):
		{
			Position3D relativePos = otherEntity->position() - this->pEntity()->position();
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xz_yp_optimized, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			pForwardBundle->appendPackXZ(relativePos.x, relativePos.z);
			(*pForwardBundle) << angle2int8(dir.yaw());
			(*pForwardBundle) << angle2int8(dir.pitch());
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xz_yp_optimized, update);
		}
		break;
		case (UPDATE_FLAG_XZ | UPDATE_FLAG_PITCH_ROLL):
		{
			Position3D relativePos = otherEntity->position() - this->pEntity()->position();
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xz_pr_optimized, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			pForwardBundle->appendPackXZ(relativePos.x, relativePos.z);
			(*pForwardBundle) << angle2int8(dir.pitch());
			(*pForwardBundle) << angle2int8(dir.roll());
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xz_pr_optimized, update);
		}
		break;
		case (UPDATE_FLAG_XZ | UPDATE_FLAG_YAW_PITCH_ROLL):
		{
			Position3D relativePos = otherEntity->position() - this->pEntity()->position();
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xz_ypr_optimized, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			pForwardBundle->appendPackXZ(relativePos.x, relativePos.z);
			(*pForwardBundle) << angle2int8(dir.yaw());
			(*pForwardBundle) << angle2int8(dir.pitch());
			(*pForwardBundle) << angle2int8(dir.roll());
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xz_ypr_optimized, update);
		}
		break;
		case (UPDATE_FLAG_XYZ | UPDATE_FLAG_YAW):
		{
			Position3D relativePos = otherEntity->position() - this->pEntity()->position();
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xyz_y_optimized, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			pForwardBundle->appendPackXZ(relativePos.x, relativePos.z);
			pForwardBundle->appendPackY(relativePos.y);
			(*pForwardBundle) << angle2int8(dir.yaw());
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xyz_y_optimized, update);
		}
		break;
		case (UPDATE_FLAG_XYZ | UPDATE_FLAG_PITCH):
		{
			Position3D relativePos = otherEntity->position() - this->pEntity()->position();
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xyz_p_optimized, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			pForwardBundle->appendPackXZ(relativePos.x, relativePos.z);
			pForwardBundle->appendPackY(relativePos.y);
			(*pForwardBundle) << angle2int8(dir.pitch());
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xyz_p_optimized, update);
		}
		break;
		case (UPDATE_FLAG_XYZ | UPDATE_FLAG_ROLL):
		{
			Position3D relativePos = otherEntity->position() - this->pEntity()->position();
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xyz_r_optimized, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			pForwardBundle->appendPackXZ(relativePos.x, relativePos.z);
			pForwardBundle->appendPackY(relativePos.y);
			(*pForwardBundle) << angle2int8(dir.roll());
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xyz_r_optimized, update);
		}
		break;
		case (UPDATE_FLAG_XYZ | UPDATE_FLAG_YAW_ROLL):
		{
			Position3D relativePos = otherEntity->position() - this->pEntity()->position();
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xyz_yr_optimized, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			pForwardBundle->appendPackXZ(relativePos.x, relativePos.z);
			pForwardBundle->appendPackY(relativePos.y);
			(*pForwardBundle) << angle2int8(dir.yaw());
			(*pForwardBundle) << angle2int8(dir.roll());
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xyz_yr_optimized, update);
		}
		break;
		case (UPDATE_FLAG_XYZ | UPDATE_FLAG_YAW_PITCH):
		{
			Position3D relativePos = otherEntity->position() - this->pEntity()->position();
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xyz_yp_optimized, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			pForwardBundle->appendPackXZ(relativePos.x, relativePos.z);
			pForwardBundle->appendPackY(relativePos.y);
			(*pForwardBundle) << angle2int8(dir.yaw());
			(*pForwardBundle) << angle2int8(dir.pitch());
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xyz_yp_optimized, update);
		}
		break;
		case (UPDATE_FLAG_XYZ | UPDATE_FLAG_PITCH_ROLL):
		{
			Position3D relativePos = otherEntity->position() - this->pEntity()->position();
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xyz_pr_optimized, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			pForwardBundle->appendPackXZ(relativePos.x, relativePos.z);
			pForwardBundle->appendPackY(relativePos.y);
			(*pForwardBundle) << angle2int8(dir.pitch());
			(*pForwardBundle) << angle2int8(dir.roll());
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xyz_pr_optimized, update);
		}
		break;
		case (UPDATE_FLAG_XYZ | UPDATE_FLAG_YAW_PITCH_ROLL):
		{
			Position3D relativePos = otherEntity->position() - this->pEntity()->position();
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xyz_ypr_optimized, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			pForwardBundle->appendPackXZ(relativePos.x, relativePos.z);
			pForwardBundle->appendPackY(relativePos.y);
			(*pForwardBundle) << angle2int8(dir.yaw());
			(*pForwardBundle) << angle2int8(dir.pitch());
			(*pForwardBundle) << angle2int8(dir.roll());
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xyz_ypr_optimized, update);
		}
		break;
		default:
			KBE_ASSERT(false);
			break;
		};
	}
	else
	{
		switch (flags)
		{
		case UPDATE_FLAG_NULL:
		{
			// (*pForwardBundle).newMessage(ClientInterface::onUpdateData);
		}
		break;
		case UPDATE_FLAG_XZ:
		{
			const Position3D& pos = otherEntity->position();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xz, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << pos.x;
			(*pForwardBundle) << pos.z;
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xz, update);
		}
		break;
		case UPDATE_FLAG_XYZ:
		{
			const Position3D& pos = otherEntity->position();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xyz, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << pos.x;
			(*pForwardBundle) << pos.y;
			(*pForwardBundle) << pos.z;
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xyz, update);
		}
		break;
		case UPDATE_FLAG_YAW:
		{
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_y, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << dir.yaw();
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_y, update);
		}
		break;
		case UPDATE_FLAG_ROLL:
		{
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_r, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << dir.roll();
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_r, update);
		}
		break;
		case UPDATE_FLAG_PITCH:
		{
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_p, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << dir.pitch();
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_p, update);
		}
		break;
		case UPDATE_FLAG_YAW_PITCH_ROLL:
		{
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_ypr, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << dir.yaw();
			(*pForwardBundle) << dir.pitch();
			(*pForwardBundle) << dir.roll();
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_ypr, update);
		}
		break;
		case UPDATE_FLAG_YAW_PITCH:
		{
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_yp, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << dir.yaw();
			(*pForwardBundle) << dir.pitch();
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_yp, update);
		}
		break;
		case UPDATE_FLAG_YAW_ROLL:
		{
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_yr, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << dir.yaw();
			(*pForwardBundle) << dir.roll();
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_yr, update);
		}
		break;
		case UPDATE_FLAG_PITCH_ROLL:
		{
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_pr, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << dir.pitch();
			(*pForwardBundle) << dir.roll();
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_pr, update);
		}
		break;
		case (UPDATE_FLAG_XZ | UPDATE_FLAG_YAW):
		{
			const Position3D& pos = otherEntity->position();
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xz_y, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << pos.x;
			(*pForwardBundle) << pos.z;
			(*pForwardBundle) << dir.yaw();
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xz_y, update);
		}
		break;
		case (UPDATE_FLAG_XZ | UPDATE_FLAG_PITCH):
		{
			const Position3D& pos = otherEntity->position();
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xz_p, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << pos.x;
			(*pForwardBundle) << pos.z;
			(*pForwardBundle) << dir.pitch();
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xz_p, update);
		}
		break;
		case (UPDATE_FLAG_XZ | UPDATE_FLAG_ROLL):
		{
			const Position3D& pos = otherEntity->position();
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xz_r, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << pos.x;
			(*pForwardBundle) << pos.z;
			(*pForwardBundle) << dir.roll();
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xz_r, update);
		}
		break;
		case (UPDATE_FLAG_XZ | UPDATE_FLAG_YAW_ROLL):
		{
			const Position3D& pos = otherEntity->position();
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xz_yr, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << pos.x;
			(*pForwardBundle) << pos.z;
			(*pForwardBundle) << dir.yaw();
			(*pForwardBundle) << dir.roll();
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xz_yr, update);
		}
		break;
		case (UPDATE_FLAG_XZ | UPDATE_FLAG_YAW_PITCH):
		{
			const Position3D& pos = otherEntity->position();
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xz_yp, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << pos.x;
			(*pForwardBundle) << pos.z;
			(*pForwardBundle) << dir.yaw();
			(*pForwardBundle) << dir.pitch();
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xz_yp, update);
		}
		break;
		case (UPDATE_FLAG_XZ | UPDATE_FLAG_PITCH_ROLL):
		{
			const Position3D& pos = otherEntity->position();
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xz_pr, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << pos.x;
			(*pForwardBundle) << pos.z;
			(*pForwardBundle) << dir.pitch();
			(*pForwardBundle) << dir.roll();
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xz_pr, update);
		}
		break;
		case (UPDATE_FLAG_XZ | UPDATE_FLAG_YAW_PITCH_ROLL):
		{
			const Position3D& pos = otherEntity->position();
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xz_ypr, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << pos.x;
			(*pForwardBundle) << pos.z;
			(*pForwardBundle) << dir.yaw();
			(*pForwardBundle) << dir.pitch();
			(*pForwardBundle) << dir.roll();
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xz_ypr, update);
		}
		break;
		case (UPDATE_FLAG_XYZ | UPDATE_FLAG_YAW):
		{
			const Position3D& pos = otherEntity->position();
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xyz_y, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << pos.x;
			(*pForwardBundle) << pos.y;
			(*pForwardBundle) << pos.z;
			(*pForwardBundle) << dir.yaw();
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xyz_y, update);
		}
		break;
		case (UPDATE_FLAG_XYZ | UPDATE_FLAG_PITCH):
		{
			const Position3D& pos = otherEntity->position();
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xyz_p, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << pos.x;
			(*pForwardBundle) << pos.y;
			(*pForwardBundle) << pos.z;
			(*pForwardBundle) << dir.pitch();
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xyz, update);
		}
		break;
		case (UPDATE_FLAG_XYZ | UPDATE_FLAG_ROLL):
		{
			const Position3D& pos = otherEntity->position();
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xyz_r, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << pos.x;
			(*pForwardBundle) << pos.y;
			(*pForwardBundle) << pos.z;
			(*pForwardBundle) << dir.roll();
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xyz_r, update);
		}
		break;
		case (UPDATE_FLAG_XYZ | UPDATE_FLAG_YAW_ROLL):
		{
			const Position3D& pos = otherEntity->position();
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xyz_yr, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << pos.x;
			(*pForwardBundle) << pos.y;
			(*pForwardBundle) << pos.z;
			(*pForwardBundle) << dir.yaw();
			(*pForwardBundle) << dir.roll();
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xyz_yr, update);
		}
		break;
		case (UPDATE_FLAG_XYZ | UPDATE_FLAG_YAW_PITCH):
		{
			const Position3D& pos = otherEntity->position();
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xyz_yp, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << pos.x;
			(*pForwardBundle) << pos.y;
			(*pForwardBundle) << pos.z;
			(*pForwardBundle) << dir.yaw();
			(*pForwardBundle) << dir.pitch();
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xyz_yp, update);
		}
		break;
		case (UPDATE_FLAG_XYZ | UPDATE_FLAG_PITCH_ROLL):
		{
			const Position3D& pos = otherEntity->position();
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xyz_pr, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << pos.x;
			(*pForwardBundle) << pos.y;
			(*pForwardBundle) << pos.z;
			(*pForwardBundle) << dir.pitch();
			(*pForwardBundle) << dir.roll();
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xyz_pr, update);
		}
		break;
		case (UPDATE_FLAG_XYZ | UPDATE_FLAG_YAW_PITCH_ROLL):
		{
			const Position3D& pos = otherEntity->position();
			const Direction3D& dir = otherEntity->direction();

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pForwardBundle, ClientInterface::onUpdateData_xyz_ypr, update);
			_addViewEntityIDToBundle(pForwardBundle, pEntityRef);
			(*pForwardBundle) << pos.x;
			(*pForwardBundle) << pos.y;
			(*pForwardBundle) << pos.z;
			(*pForwardBundle) << dir.yaw();
			(*pForwardBundle) << dir.pitch();
			(*pForwardBundle) << dir.roll();
			ENTITY_MESSAGE_FORWARD_CLIENT_END(pForwardBundle, ClientInterface::onUpdateData_xyz_ypr, update);
		}
		break;
		default:
			KBE_ASSERT(false);
			break;
		};
	}
}

//-------------------------------------------------------------------------------------
uint32 Witness::getEntityVolatileDataUpdateFlags(Entity* otherEntity)
{
	uint32 flags = UPDATE_FLAG_NULL;

	/* 如果目标被我控制了，则目标的位置不通知我的客户端。
	   注意：当这个被我控制的entity在服务器中使用moveToPoint()等接口移动时，
	         也会由于这个判定导致坐标不会同步到控制者的客户端中
	*/
	if (otherEntity->controlledBy() && pEntity_->id() == otherEntity->controlledBy()->id())
		return flags;

	const VolatileInfo* pVolatileInfo = otherEntity->pCustomVolatileinfo();
	if (!pVolatileInfo)
		pVolatileInfo = otherEntity->pScriptModule()->getPVolatileInfo();

	static uint16 entity_posdir_additional_updates = g_kbeSrvConfig.getCellApp().entity_posdir_additional_updates;
	
	if ((pVolatileInfo->position() > 0.f) && (entity_posdir_additional_updates == 0 || g_kbetime - otherEntity->posChangedTime() < entity_posdir_additional_updates))
	{
		if (!otherEntity->isOnGround() || !pVolatileInfo->optimized())
		{
			flags |= UPDATE_FLAG_XYZ; 
		}
		else
		{
			flags |= UPDATE_FLAG_XZ; 
		}
	}

	if((entity_posdir_additional_updates == 0) || (g_kbetime - otherEntity->dirChangedTime() < entity_posdir_additional_updates))
	{
		if (pVolatileInfo->yaw() > 0.f)
		{
			if (pVolatileInfo->roll() > 0.f)
			{
				if (pVolatileInfo->pitch() > 0.f)
				{
					flags |= UPDATE_FLAG_YAW_PITCH_ROLL;
				}
				else
				{
					flags |= UPDATE_FLAG_YAW_ROLL;
				}
			}
			else if (pVolatileInfo->pitch() > 0.f)
			{
				flags |= UPDATE_FLAG_YAW_PITCH;
			}
			else
			{
				flags |= UPDATE_FLAG_YAW;
			}
		}
		else if (pVolatileInfo->roll() > 0.f)
		{
			if (pVolatileInfo->pitch() > 0.f)
			{
				flags |= UPDATE_FLAG_PITCH_ROLL;
			}
			else
			{
				flags |= UPDATE_FLAG_ROLL;
			}
		}
		else if (pVolatileInfo->pitch() > 0.f)
		{
			flags |= UPDATE_FLAG_PITCH; 
		}
	}

	return flags;
}

//-------------------------------------------------------------------------------------
bool Witness::sendToClient(const Network::MessageHandler& msgHandler, Network::Bundle* pBundle)
{
	if(pushBundle(pBundle))
		return true;

	ERROR_MSG(fmt::format("Witness::sendToClient: {} pBundles is NULL, not found channel.\n", pEntity_->id()));
	Network::Bundle::reclaimPoolObject(pBundle);
	return false;
}

//-------------------------------------------------------------------------------------
}
