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

#ifndef KBE_BASEAPPMGR_BASEAPP_H
#define KBE_BASEAPPMGR_BASEAPP_H

#include "common/common.h"
#include "helper/debug_helper.h"
#include "helper/watcher.h"

#include <deque>

namespace KBEngine{ 

class Baseapp
{
public:
	Baseapp();
	virtual ~Baseapp();
	
	ENTITY_ID numEntitys() const { return numEntitys_; }
	void numEntitys(ENTITY_ID num) { numEntitys_ = num; }
	
	ENTITY_ID numProxices() const { return numProxices_; }
	void numProxices(ENTITY_ID num) { numProxices_ = num; }

	ENTITY_ID numClients() const { return numClients_; }
	void updateConfirmedClients(ENTITY_ID num)
	{
		if (num > numClients_)
		{
			std::size_t confirmed = static_cast<std::size_t>(num - numClients_);
			while (confirmed > 0 && !pendingLoginTimes_.empty())
			{
				pendingLoginTimes_.pop_front();
				--confirmed;
			}
		}
		numClients_ = num;
	}

	std::size_t pendingLogins() const { return pendingLoginTimes_.size(); }
	void reservePendingLogin(uint64 now) { pendingLoginTimes_.push_back(now); }
	void releasePendingLogin()
	{
		if (!pendingLoginTimes_.empty())
			pendingLoginTimes_.pop_front();
	}
	void expirePendingLogins(uint64 now, uint64 maximumAge)
	{
		while (!pendingLoginTimes_.empty() && now - pendingLoginTimes_.front() >= maximumAge)
			pendingLoginTimes_.pop_front();
	}

	float load() const { return load_; }
	void load(float v) { load_ = v; }
	
	void destroy(){ isDestroyed_ = true; }
	bool isDestroyed() const { return isDestroyed_; }

	float initProgress() const{ return initProgress_; }
	void initProgress(float v){ initProgress_ = v; }

	ENTITY_ID numEntities() const { return numEntitys_ + numProxices_; }
	std::size_t assignedClients() const
	{
		return static_cast<std::size_t>(numClients_) + pendingLoginTimes_.size();
	}
	void incNumEntities() { ++numEntitys_; }

	uint32 flags() const { return flags_; }
	void flags(uint32 v) { flags_ = v; }
	
protected:
	ENTITY_ID numEntitys_;
	ENTITY_ID numProxices_;
	ENTITY_ID numClients_;
	std::deque<uint64> pendingLoginTimes_;
	float load_;

	bool isDestroyed_;

	Watchers watchers_;

	float initProgress_;
	
	uint32 flags_;
};

}

#endif // KBE_BASEAPPMGR_BASEAPP_H
