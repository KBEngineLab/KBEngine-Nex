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

#include "space.h"	
#include "spaces.h"	
#include "loadnavmesh_threadtasks.h"
#include "server/serverconfig.h"
#include "common/deadline.h"
#include "navigation/navigation.h"

namespace KBEngine{

//-------------------------------------------------------------------------------------
bool LoadNavmeshTask::process()
{
	loadSucceeded_ = Navigation::getSingleton().loadNavigation(resPath_, params_) != NULL;
	return false;
}

//-------------------------------------------------------------------------------------
thread::TPTask::TPTaskState LoadNavmeshTask::presentMainThread()
{
	Space* pSpace = Spaces::findSpace(spaceID_);
	if(pSpace == NULL || !pSpace->isGood())
	{
		// Space 可在异步加载期间销毁，晚到任务应静默结束而不是回调失效对象。
		// A space may be destroyed during asynchronous loading; a late task must finish without calling an invalid object.
		WARNING_MSG(fmt::format("LoadNavmeshTask::presentMainThread(): space({}) was removed while loading navigation resource({})\n",
			spaceID_, resPath_));
	}
	else if(!pSpace->isGeometryLoadCurrent(resPath_, loadGeneration_))
	{
		// 路径切换或卸载后，旧任务可以完成缓存填充，但不能覆盖 Space 的新几何状态。
		// After a path switch or unload, an old task may populate the cache but must not overwrite the Space's new geometry state.
		WARNING_MSG(fmt::format("LoadNavmeshTask::presentMainThread(): ignored stale navigation resource({}) generation({}) for space({})\n",
			resPath_, loadGeneration_, spaceID_));
	}
	else if(!loadSucceeded_)
	{
		ERROR_MSG(fmt::format("LoadNavmeshTask::presentMainThread(): failed to load navigation resource({}) for space({})\n",
			resPath_, spaceID_));

		// 保留既有几何加载完成回调，避免改变脚本生命周期，同时明确传递空句柄。
		// Preserve the existing geometry completion callback to avoid changing script lifecycle while explicitly passing a null handle.
		pSpace->onLoadedSpaceGeometryMapping(resPath_, loadGeneration_, NULL);
	}
	else
	{
		NavigationHandlePtr pNavigationHandle = Navigation::getSingleton().findNavigation(resPath_);
		if(pNavigationHandle == NULL)
		{
			ERROR_MSG(fmt::format("LoadNavmeshTask::presentMainThread(): navigation resource({}) disappeared from cache for space({})\n",
				resPath_, spaceID_));
		}

		pSpace->onLoadedSpaceGeometryMapping(resPath_, loadGeneration_, pNavigationHandle);
	}
	
	return thread::TPTask::TPTASK_STATE_COMPLETED; 
}

//-------------------------------------------------------------------------------------
}
