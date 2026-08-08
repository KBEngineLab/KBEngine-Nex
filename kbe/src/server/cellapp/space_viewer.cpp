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
#include "entity.h"
#include "space_viewer.h"
#include "network/network_interface.h"
#include "network/event_dispatcher.h"
#include "network/address.h"
#include "network/network_stats.h"
#include "network/bundle.h"
#include "network/message_handler.h"
#include "common/memorystream.h"
#include "helper/console_helper.h"
#include "helper/profile.h"
#include "navigation/navigation_mesh_handle.h"
#include "server/serverconfig.h"

#include <ctime>

namespace KBEngine { 

//-------------------------------------------------------------------------------------
SpaceViewers::SpaceViewers():
reportLimitTimerHandle_(),
spaceViews_()
{
}

//-------------------------------------------------------------------------------------
SpaceViewers::~SpaceViewers()
{
	finalise();
}

//-------------------------------------------------------------------------------------
bool SpaceViewers::addTimer()
{
	if (!reportLimitTimerHandle_.isSet())
	{
		reportLimitTimerHandle_ = Cellapp::getSingleton().networkInterface().dispatcher().addTimer(
			1000000 / 10, this);

		return true;
	}

	return false;
}

//-------------------------------------------------------------------------------------
void SpaceViewers::finalise()
{
	clear();
	reportLimitTimerHandle_.cancel();
}

//-------------------------------------------------------------------------------------
void SpaceViewers::updateSpaceViewer(const Network::Address& addr, SPACE_ID spaceID, CELL_ID cellID, bool del,
	bool isV2, uint16 sampleIntervalMs)
{
	if (del)
	{
		spaceViews_.erase(addr);
		return;
	}

	SpaceViewer& viewer = spaceViews_[addr];
	viewer.updateViewer(addr, spaceID, cellID, isV2, sampleIntervalMs);

	addTimer();
}

//-------------------------------------------------------------------------------------
void SpaceViewers::handleTimeout(TimerHandle handle, void * arg)
{
	if (spaceViews_.size() == 0)
	{
		reportLimitTimerHandle_.cancel();
		return;
	}

	std::map< Network::Address, SpaceViewer>::iterator iter = spaceViews_.begin();
	for (; iter != spaceViews_.end(); )
	{
		// 如果该viewer地址找不到了则将其擦除
		Network::Channel* pChannel = Cellapp::getSingleton().networkInterface().findChannel(iter->second.addr());
		if (pChannel == NULL)
		{
			spaceViews_.erase(iter++);
		}
		else
		{
			iter->second.timeout();
			++iter;
		}
	}
}

//-------------------------------------------------------------------------------------
SpaceViewer::SpaceViewer():
addr_(),
spaceID_(0),
cellID_(0),
viewedEntities(),
updateType_(0),
lastUpdateVersion_(0),
isV2_(false),
sampleIntervalMs_(100),
elapsedMs_(0),
snapshotId_(0),
sequence_(0),
lastSampleDurationStamps_(0),
lastUpdateCount_(0),
lastPayloadBytes_(0),
lastPendingCount_(0),
budgetLimitedCount_(0)
{
}

//-------------------------------------------------------------------------------------
SpaceViewer::~SpaceViewer()
{
}

//-------------------------------------------------------------------------------------
void SpaceViewer::resetViewer()
{
	viewedEntities.clear();
	lastUpdateVersion_ = 0;
	sequence_ = 0;
	++snapshotId_;
}

//-------------------------------------------------------------------------------------
void SpaceViewer::updateViewer(const Network::Address& addr, SPACE_ID spaceID, CELL_ID cellID,
	bool isV2, uint16 sampleIntervalMs)
{
	addr_ = addr;
	isV2_ = isV2;
	sampleIntervalMs_ = sampleIntervalMs;
	elapsedMs_ = 0;

	bool chagnedSpace = spaceID_ != spaceID;

	if (chagnedSpace)
	{
		onChangedSpaceOrCell();
		spaceID_ = spaceID;
	}

	if (cellID_ != cellID)
	{
		if (!chagnedSpace)
			onChangedSpaceOrCell();

		cellID_ = cellID;
	}
}

//-------------------------------------------------------------------------------------
void SpaceViewer::onChangedSpaceOrCell()
{
	resetViewer();
}

//-------------------------------------------------------------------------------------
void SpaceViewer::timeout()
{
	if (isV2_)
	{
		elapsedMs_ = uint16(elapsedMs_ + 100);
		if (elapsedMs_ < sampleIntervalMs_)
			return;
		elapsedMs_ = 0;
	}

	switch (updateType_)
	{
	case 0: // 初始化
		initClient();
		break;
	default: // 更新实体
		updateClient();
	};
}

//-------------------------------------------------------------------------------------
void SpaceViewer::sendStream(MemoryStream* s, int type)
{
	Network::Channel* pChannel = Cellapp::getSingleton().networkInterface().findChannel(addr_);
	if(pChannel == NULL)
	{
		WARNING_MSG(fmt::format("SpaceViewer::sendStream: not found addr({})\n",
			addr_.c_str()));

		return;
	}

	Network::Bundle* pBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);

	ConsoleInterface::ConsoleQuerySpacesHandler msgHandler;
	(*pBundle).newMessage(msgHandler);

	(*pBundle) << g_componentType;
	(*pBundle) << g_componentID;
	(*pBundle) << type;
	(*pBundle).append(s->data() + s->rpos(), static_cast<int>(s->length()));
	pChannel->send(pBundle);
}

//-------------------------------------------------------------------------------------
void SpaceViewer::initClient()
{
	MemoryStream s;
	Space* space = Spaces::findSpace(spaceID_);

	if (isV2_)
	{
		if (space == NULL || !space->isGood())
			return;

		const uint32 viewerV2Magic = 0x3253584E;
		const uint16 viewerV2Version = 3;
		const uint8 metadataMessage = 2;
		float minimumX = -50.f;
		float minimumZ = -50.f;
		float maximumX = 50.f;
		float maximumZ = 50.f;
		Space::VIEWER_BOUNDS_SOURCE boundsSource = Space::VIEWER_BOUNDS_DEFAULT;
		space->getViewerBounds(minimumX, minimumZ, maximumX, maximumZ, boundsSource);

		s << viewerV2Magic << viewerV2Version << metadataMessage;
		s << (uint64)g_componentID << (uint32)space->id();
		s << space->getGeometryPath() << space->getScriptModuleName();
		s << (uint32)space->entities().size();
		s << minimumX << minimumZ << maximumX << maximumZ << (uint8)boundsSource;
		s << (int64)(std::time(NULL) * 1000LL);
	}

	// 先下发脚本ID对应脚本模块的名称，便于降低后面实体同步量，实体只同步id过去
	const EntityDef::SCRIPT_MODULES& scriptModules = EntityDef::getScriptModules();
	s << (uint32)scriptModules.size();

	EntityDef::SCRIPT_MODULES::const_iterator moduleIter = scriptModules.begin();
	for (; moduleIter != scriptModules.end(); ++moduleIter)
	{
		s << moduleIter->get()->getUType();
		s << moduleIter->get()->getName();
	}

	if (isV2_)
	{
		const uint32 maxViewerSegments = 20000;
		NavigationHandlePtr pNavHandle = space->pNavHandle();
		if (pNavHandle && pNavHandle->type() == NavigationHandle::NAV_MESH)
		{
			static_cast<NavMeshHandle*>(pNavHandle.get())->writeViewerSegments(s, maxViewerSegments);
		}
		else
		{
			s << (uint32)0;
		}
	}

	sendStream(&s, updateType_);

	// 改变为更新实体
	updateType_ = 1;

	lastUpdateVersion_ = 0;
}

//-------------------------------------------------------------------------------------
void SpaceViewer::updateClient()
{
	const uint64 sampleStartedAt = timestamp();
	if (spaceID_ == 0)
		return;

	Space* space = Spaces::findSpace(spaceID_);
	if (space == NULL || !space->isGood())
	{
		return;
	}

	// 单轮预算限制 Tick 与网络峰值；完整快照可跨多轮发送，并由完成标记显式收口。
	// The per-round budget bounds Tick and network spikes; completion is explicit when a snapshot spans rounds.
	const int MAX_UPDATE_COUNT = 100;
	const uint32 MAX_UPDATE_BYTES = 64 * 1024;
	int updateCount = 0;

	// 获取本次与上次结果的差值，将差值放入stream中更新到客户端
	// 差值包括新增的实体，以及已经有的实体的位置变化
	MemoryStream deltas;
	std::map<ENTITY_ID, Entity*> currentEntities;
	const SPACE_ENTITIES& spaceEntities = space->entities();
	for (SPACE_ENTITIES::const_iterator entityIter = spaceEntities.begin();
		entityIter != spaceEntities.end(); ++entityIter)
	{
		Entity* entity = (*entityIter).get();
		if (entity != NULL)
			currentEntities[entity->id()] = entity;
	}

	// 先检查已经监视的实体，对于版本号较低的优先更新
	if (updateCount < MAX_UPDATE_COUNT)
	{
		std::map< ENTITY_ID, ViewEntity >::iterator viewerIter = viewedEntities.begin();
		for (; viewerIter != viewedEntities.end(); )
		{
			if (updateCount >= MAX_UPDATE_COUNT || deltas.length() >= MAX_UPDATE_BYTES)
				break;

			ViewEntity& viewEntity = viewerIter->second;
			if (viewEntity.updateVersion > lastUpdateVersion_)
			{
				++viewerIter;
				continue;
			}

			std::map<ENTITY_ID, Entity*>::iterator iter = currentEntities.find(viewerIter->first);

			// 找不到实体， 说明已经销毁或者跑到其他进程了
			// 如果在其他进程， 其他进程会将其更新到客户端
			if (iter == currentEntities.end())
			{
				deltas << viewerIter->first;
				deltas << false; // true为更新， false为销毁

				// 将其从viewedEntities删除
				viewedEntities.erase(viewerIter++);
				++updateCount;
			}
			else
			{
				Entity* pEntity = iter->second;

				// 有新增的实体或者已经观察到的实体，检查位置变化
				// 如果没有变化则pass
				if ((viewEntity.position - pEntity->position()).length() <= 0.0004f &&
					(viewEntity.direction.dir - pEntity->direction().dir).length() <= 0.0004f)
				{
					++viewerIter;
					continue;
				}

				viewEntity.entityID = pEntity->id();
				viewEntity.position = pEntity->position();
				viewEntity.direction = pEntity->direction();
				++viewEntity.updateVersion;

				deltas << viewEntity.entityID;
				deltas << true; // true为更新， false为销毁
				deltas << pEntity->pScriptModule()->getUType();
				deltas << viewEntity.position.x << viewEntity.position.y << viewEntity.position.z;
				deltas << viewEntity.direction.roll() << viewEntity.direction.pitch() << viewEntity.direction.yaw();

				++updateCount;
				++viewerIter;
			}
		}
	}

	// 再检查是否有新增的实体
	if (updateCount < MAX_UPDATE_COUNT)
	{
		for (SPACE_ENTITIES::const_iterator iter = spaceEntities.begin(); iter != spaceEntities.end(); ++iter)
		{
			if (updateCount >= MAX_UPDATE_COUNT || deltas.length() >= MAX_UPDATE_BYTES)
				break;

			Entity* pEntity = (*iter).get();
			if (pEntity == NULL)
				continue;

			std::map< ENTITY_ID, ViewEntity >::iterator findIter = viewedEntities.find(pEntity->id());
			ViewEntity& viewEntity = viewedEntities[pEntity->id()];

			if (findIter != viewedEntities.end())
				continue;

			viewEntity.entityID = pEntity->id();
			viewEntity.position = pEntity->position();
			viewEntity.direction = pEntity->direction();
			viewEntity.updateVersion = lastUpdateVersion_ + 1;

			++updateCount;

			deltas << viewEntity.entityID;
			deltas << true; // true为更新， false为销毁
			deltas << pEntity->pScriptModule()->getUType();
			deltas << viewEntity.position.x << viewEntity.position.y << viewEntity.position.z;
			deltas << viewEntity.direction.roll() << viewEntity.direction.pitch() << viewEntity.direction.yaw();
		}
	}

	MemoryStream output;
	const bool snapshotComplete = updateCount < MAX_UPDATE_COUNT && deltas.length() < MAX_UPDATE_BYTES;
	if (isV2_)
	{
		const uint32 viewerV2Magic = 0x3253584E;
		const uint16 viewerV2Version = 3;
		const uint8 snapshotMessage = 3;
		uint8 flags = 0;
		if (lastUpdateVersion_ == 0)
			flags |= 0x01;
		if (snapshotComplete)
			flags |= 0x02;

		output << viewerV2Magic << viewerV2Version << snapshotMessage;
		output << snapshotId_ << ++sequence_ << flags;
		output << (int64)(std::time(NULL) * 1000LL);
		output << (uint32)spaceID_ << (uint32)spaceEntities.size() << (uint32)updateCount;
		output.append(deltas.data() + deltas.rpos(), deltas.length());
	}
	else
	{
		output.append(deltas.data() + deltas.rpos(), deltas.length());
	}

	lastUpdateCount_ = (uint32)updateCount;
	lastPayloadBytes_ = (uint32)output.length();
	lastPendingCount_ = (uint32)(spaceEntities.size() > viewedEntities.size()
		? spaceEntities.size() - viewedEntities.size() : 0);
	if (updateCount >= MAX_UPDATE_COUNT || deltas.length() >= MAX_UPDATE_BYTES)
		++budgetLimitedCount_;
	lastSampleDurationStamps_ = timestamp() - sampleStartedAt;

	sendStream(&output, updateType_);

	// 如果全部更新完毕，更换版本号
	if (snapshotComplete)
		++lastUpdateVersion_;
}

//-------------------------------------------------------------------------------------

}
