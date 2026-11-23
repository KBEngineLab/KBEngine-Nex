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
#include "entity.h"
#include "witnessed_timeout_handler.h"
#include "server/serverconfig.h"

namespace KBEngine{	

static const uint8 TICKSECS = 5;

//-------------------------------------------------------------------------------------
WitnessedTimeoutHandler::WitnessedTimeoutHandler() : 
witnessedEntityIDs_(), pTimerHandle_(NULL)
{
}

//-------------------------------------------------------------------------------------
WitnessedTimeoutHandler::~WitnessedTimeoutHandler()
{
	cancel();
}

//-------------------------------------------------------------------------------------
void WitnessedTimeoutHandler::cancel()
{
	if(pTimerHandle_)
	{
		pTimerHandle_->cancel();
		delete pTimerHandle_;
		pTimerHandle_ = NULL;
	}
}

//-------------------------------------------------------------------------------------
void WitnessedTimeoutHandler::handleTimeout(TimerHandle, void * arg)
{
	std::map<ENTITY_ID, uint16>::iterator iter = witnessedEntityIDs_.begin();
	for(; iter != witnessedEntityIDs_.end();)
	{
		if(iter->second > TICKSECS)
		{
			iter->second -= TICKSECS;
			iter++;
		}
		else
		{
			const ENTITY_ID id = iter->first;
			Entity* pEntity = Cellapp::getSingleton().findEntity(id);
			witnessedEntityIDs_.erase(iter);

			if(pEntity)
				pEntity->onDelWitnessed();

			// 脚本可能销毁其他实体，不能继续使用回调前保存的迭代器。
			iter = witnessedEntityIDs_.upper_bound(id);
		}
	}

	if(witnessedEntityIDs_.size() == 0)
	{
		cancel();
		DEBUG_MSG("WitnessedTimeoutHandler::handleTimeout: witnesseds is empty, timer is canceled!\n");
	}
}

//-------------------------------------------------------------------------------------
void WitnessedTimeoutHandler::addWitnessed(Entity* pEntity)
{
	if(pEntity == NULL)
		return;

	const uint16 witness_timeout_dec = ServerConfig::getSingleton().getCellApp().witness_timeout;
	if(witness_timeout_dec == 0)
	{
		if (Cellapp::getSingleton().spacePartitioningEnabled())
			pendingExits_.insert(pEntity->id());
		else
			pEntity->onDelWitnessed();
		return;
	}

	if(witnessedEntityIDs_.find(pEntity->id()) != witnessedEntityIDs_.end())
		witnessedEntityIDs_[pEntity->id()] = witness_timeout_dec;
	else
		witnessedEntityIDs_.insert(std::map<ENTITY_ID, uint16>::value_type(pEntity->id(), witness_timeout_dec));
	
	if(pTimerHandle_ == NULL)
	{
		pTimerHandle_ = new TimerHandle();
		(*pTimerHandle_) = Cellapp::getSingleton().dispatcher().addTimer(TICKSECS * 1000000, this,
								NULL);
	}
}

//-------------------------------------------------------------------------------------
bool WitnessedTimeoutHandler::delWitnessed(Entity* pEntity)
{
	const ENTITY_ID id = pEntity->id();
	const auto pending = pendingExits_.erase(id);
	const auto dispatching = dispatchingExits_.erase(id);
	return witnessedEntityIDs_.erase(id) || pending || dispatching;
}

//-------------------------------------------------------------------------------------
void WitnessedTimeoutHandler::dispatchPendingExits()
{
	// 脚本可能让其他实体重新被观察，也可能产生新的离开通知。新通知留到下一轮。
	dispatchingExits_.swap(pendingExits_);
	while (!dispatchingExits_.empty())
	{
		const auto entry = dispatchingExits_.begin();
		const ENTITY_ID id = *entry;
		dispatchingExits_.erase(entry);
		if (auto* entity = Cellapp::getSingleton().findEntity(id))
			entity->onDelWitnessed();
	}
}

//-------------------------------------------------------------------------------------

}
