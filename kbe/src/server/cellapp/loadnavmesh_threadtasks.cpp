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
	Navigation::getSingleton().loadNavigation(resPath_, params_);
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
	else
	{
		NavigationHandlePtr pNavigationHandle = Navigation::getSingleton().findNavigation(resPath_);

		// 几何映射允许没有服务端导航数据；空句柄仍须完成映射并通知脚本。
		// 导航文件存在但损坏时，具体加载器负责记录错误，不能把可选数据缺失误报为加载失败。
		pSpace->onLoadedSpaceGeometryMapping(resPath_, loadGeneration_, pNavigationHandle);
	}
	
	return thread::TPTask::TPTASK_STATE_COMPLETED; 
}

//-------------------------------------------------------------------------------------
}
