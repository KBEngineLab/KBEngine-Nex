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

#ifndef KBE_WITNESSED_TIMEOUT_HANDLER_H
#define KBE_WITNESSED_TIMEOUT_HANDLER_H

// common include
#include "helper/debug_helper.h"
#include "common/common.h"
#include <set>
// #define NDEBUG
// windows include	
#if KBE_PLATFORM == PLATFORM_WIN32
#else
// linux include
#endif

namespace KBEngine{

class WitnessedTimeoutHandler : public TimerHandler
{	
public:	
	WitnessedTimeoutHandler();

	~WitnessedTimeoutHandler();

	void addWitnessed(Entity* pEntity);
	bool delWitnessed(Entity* pEntity);
	bool hasPendingExit(ENTITY_ID id) const
	{
		return witnessedEntityIDs_.count(id) || pendingExits_.count(id) || dispatchingExits_.count(id);
	}
	void dispatchPendingExits();

private:
	virtual void handleTimeout(TimerHandle handle, void * pUser);

	virtual void onRelease( TimerHandle handle, void * /*pUser*/ ){};

	void cancel();

	std::map<ENTITY_ID, uint16>		witnessedEntityIDs_;
	std::set<ENTITY_ID> pendingExits_;
	std::set<ENTITY_ID> dispatchingExits_;
	TimerHandle* pTimerHandle_;
};	


}

#endif // KBE_WITNESSED_TIMEOUT_HANDLER_H
