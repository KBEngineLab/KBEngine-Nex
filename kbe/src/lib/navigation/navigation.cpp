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

#include "navigation.h"
#include "resmgr/resmgr.h"
#include "thread/threadguard.h"

#include "navigation_tile_handle.h"
#include "navigation_mesh_handle.h"

namespace KBEngine{

KBE_SINGLETON_INIT(Navigation);

//-------------------------------------------------------------------------------------
Navigation::Navigation():
navhandles_(),
mutex_()
{
}

//-------------------------------------------------------------------------------------
Navigation::~Navigation()
{
	finalise();
}

//-------------------------------------------------------------------------------------
void Navigation::finalise()
{
	KBEngine::thread::ThreadGuard tg(&mutex_);
	navhandles_.clear();
}

//-------------------------------------------------------------------------------------
bool Navigation::removeNavigation(std::string resPath)
{
	KBEngine::thread::ThreadGuard tg(&mutex_); 
	bool removed = false;
	for (auto iter = navhandles_.begin(); iter != navhandles_.end(); )
	{
		if (iter->first.first == resPath)
		{
			// 删除缓存引用不会影响仍由其他 Space 持有的导航句柄。
			iter = navhandles_.erase(iter);
			removed = true;
		}
		else
			++iter;
	}
	return removed;
}

//-------------------------------------------------------------------------------------
NavigationHandlePtr Navigation::findNavigation(std::string resPath,
	const std::map<int, std::string>& params)
{
	KBEngine::thread::ThreadGuard tg(&mutex_); 
	NavigationHandles::iterator iter = navhandles_.find(NavigationKey(resPath, params));
	if(iter != navhandles_.end())
	{
		if(iter->second == NULL)
			return NULL;

		if(iter->second->type() == NavigationHandle::NAV_MESH)
		{
			return iter->second;
		}
		else if (iter->second->type() == NavigationHandle::NAV_TILE)
		{
			// 由于tile需要做碰撞， 每一个space都需要一份新的数据， 我们这里采用拷贝的方式来增加构造速度
			NavTileHandle* pNavTileHandle = new NavTileHandle(*(KBEngine::NavTileHandle*)iter->second.get());
			DEBUG_MSG(fmt::format("Navigation::findNavigation: copy NavTileHandle({:p})!\n", (void*)pNavTileHandle));
			return NavigationHandlePtr(pNavTileHandle);
		}

		return iter->second;
	}

	return NULL;
}

//-------------------------------------------------------------------------------------
bool Navigation::hasNavigation(std::string resPath)
{
	KBEngine::thread::ThreadGuard tg(&mutex_); 
	auto found = navhandles_.lower_bound(NavigationKey(resPath, {}));
	return found != navhandles_.end() && found->first.first == resPath;
}

//-------------------------------------------------------------------------------------
NavigationHandlePtr Navigation::loadNavigation(std::string resPath, const std::map< int, std::string >& params)
{
	KBEngine::thread::ThreadGuard tg(&mutex_); 
	if(resPath == "")
		return NULL;
	
	const NavigationKey key(resPath, params);
	NavigationHandles::iterator iter = navhandles_.find(key);
	if(iter != navhandles_.end())
	{
		return iter->second;
	}

	NavigationHandle* pNavigationHandle_ = NULL;

	std::string path = resPath;
	path = Resmgr::getSingleton().matchPath(path);
	if(path.size() == 0)
		return NULL;
		
	wchar_t* wpath = strutil::char2wchar(path.c_str());
	std::wstring wspath = wpath;
	free(wpath);

	std::vector<std::wstring> results;
	Resmgr::getSingleton().listPathRes(wspath, L"tmx", results);
	
	if(results.size() > 0)
	{
		pNavigationHandle_ = NavTileHandle::create(resPath, params);
	}
	else 	
	{
		results.clear();
		// Nex 导出的 Detour 数据使用 .bin 扩展名，旧资源仍可能使用 .navmesh。
		// Nex exports Detour data with the .bin extension, while legacy resources may still use .navmesh.
		Resmgr::getSingleton().listPathRes(wspath, L"bin", results);

		if(results.size() == 0)
		{
			Resmgr::getSingleton().listPathRes(wspath, L"navmesh", results);
		}

		if(results.size() == 0)
		{
			return NULL;
		}

		pNavigationHandle_ = NavMeshHandle::create(resPath, params);
	}


	// 创建失败的句柄不能进入缓存，否则修复资源后同一路径也无法重新加载。
	// A failed handle must not enter the cache or the same path cannot be retried after the resource is fixed.
	if(pNavigationHandle_ == NULL)
		return NULL;

	NavigationHandlePtr pNavigationHandle(pNavigationHandle_);
	navhandles_[key] = pNavigationHandle;
	return pNavigationHandle;
}

//-------------------------------------------------------------------------------------		
}
