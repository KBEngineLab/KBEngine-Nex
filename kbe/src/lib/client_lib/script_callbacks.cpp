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

#include "config.h"
#include "script_callbacks.h"
#include "server/serverapp.h"
#include "server/serverconfig.h"
#include "pyscript/script.h"
#include "pyscript/pyobject_pointer.h"
#include "common/smartpointer.h"

namespace KBEngine
{


//-------------------------------------------------------------------------------------
ScriptCallbacks::ScriptCallbacks(Timers& timers):
timers_(timers)
{
}

//-------------------------------------------------------------------------------------
ScriptCallbacks::~ScriptCallbacks()
{
	// DEBUG_MSG("ScriptCallbacks::~ScriptCallbacks: timers_size(%d).\n", map_.size());
	cancelAll();
}

//-------------------------------------------------------------------------------------
ScriptID ScriptCallbacks::addCallback( float initialOffset, float repeatOffset, TimerHandler * pHandler )
{
	if (initialOffset < 0.f)
	{
		WARNING_MSG(fmt::format("ScriptCallbacks::addTimer: Negative timer offset ({})\n",
				initialOffset));

		initialOffset = 0.f;
	}

	int hertz = 0;
	
	if(g_componentType == BOTS_TYPE)
		hertz = g_kbeSrvConfig.gameUpdateHertz();
	else
		hertz = Config::getSingleton().gameUpdateHertz();

	int initialTicks = GameTime( g_kbetime +
			initialOffset * hertz );

	int repeatTicks = 0;

	if (repeatOffset > 0.f)
	{
		repeatTicks = GameTime( repeatOffset * hertz );
		if (repeatTicks < 1)
		{
			repeatTicks = 1;
		}
	}

	TimerHandle timerHandle = timers_.add(
			initialTicks, repeatTicks,
			pHandler, NULL );

	if (timerHandle.isSet())
	{
		int id = this->getNewID();

		map_[ id ] = timerHandle;

		return id;
	}

	return 0;
}

//-------------------------------------------------------------------------------------
ScriptID ScriptCallbacks::getNewID()
{
	ScriptID id = 1;

	while (map_.find( id ) != map_.end())
	{
		++id;
	}

	return id;
}

//-------------------------------------------------------------------------------------
bool ScriptCallbacks::delCallback(ScriptID timerID)
{
	Map::iterator iter = map_.find( timerID );

	if (iter != map_.end())
	{
		TimerHandle handle = iter->second;
		handle.cancel();
		return true;
	}

	return false;
}

//-------------------------------------------------------------------------------------
void ScriptCallbacks::releaseCallback( TimerHandle handle )
{
	int numErased = 0;

	Map::iterator iter = map_.begin();

	while (iter != map_.end())
	{
		KBE_ASSERT( iter->second.isSet() );

		if (handle == iter->second)
		{
			map_.erase( iter++ );
			++numErased;
		}
		else
		{
			iter++;
		}
	}

	KBE_ASSERT( numErased == 1 );
}

//-------------------------------------------------------------------------------------
void ScriptCallbacks::cancelAll()
{
	while (!map_.empty())
	{
		// Releasing a bound Entity method may synchronously destroy that Entity and cancel another
		// callback (for example its movement controller). Require progress instead of assuming one
		// erased map entry per iteration.
		// 释放 Entity 绑定方法可能同步析构 Entity，并取消另一个回调（例如移动控制器）；
		// 因此只要求每轮有进展，不能假定每次恰好删除一个条目。
		Map::size_type sizeBeforeCancel = map_.size();
		TimerHandle handle = map_.begin()->second;
		handle.cancel();
		KBE_ASSERT(map_.size() < sizeBeforeCancel);
	}
}

//-------------------------------------------------------------------------------------
ScriptID ScriptCallbacks::getIDForHandle(TimerHandle handle) const
{
	Map::const_iterator iter = this->findCallback( handle );

	return (iter != map_.end()) ? iter->first : 0;
}

//-------------------------------------------------------------------------------------
ScriptCallbacks::Map::const_iterator ScriptCallbacks::findCallback(TimerHandle handle) const
{
	Map::const_iterator iter = map_.begin();

	while (iter != map_.end())
	{
		if (iter->second == handle)
		{
			return iter;
		}

		++iter;
	}

	return iter;
}

//-------------------------------------------------------------------------------------
void ScriptCallbackHandler::handleTimeout( TimerHandle handle, void * pUser )
{
	AUTO_SCOPED_PROFILE("callCallbacks");

	//int id = scriptCallbacks_.getIDForHandle(handle);

	PyObject * pObject = pObject_;

	Py_INCREF( pObject );
	
	PyObject * pResult =
		PyObject_CallFunction( pObject, const_cast<char*>(""));

	Py_XDECREF( pResult );
	Py_DECREF( pObject );
	SCRIPT_ERROR_CHECK();
}

//-------------------------------------------------------------------------------------
void ScriptCallbackHandler::onRelease( TimerHandle handle, void * /*pUser*/ )
{
	scriptCallbacks_.releaseCallback(handle);
	delete this;
}

//-------------------------------------------------------------------------------------


}



