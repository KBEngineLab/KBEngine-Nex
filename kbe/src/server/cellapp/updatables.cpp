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

#include "updatables.h"	
#include "helper/profile.h"	
#include "profile.h"

namespace KBEngine{	


//-------------------------------------------------------------------------------------
Updatables::Updatables()
{
}

//-------------------------------------------------------------------------------------
Updatables::~Updatables()
{
	clear();
}

//-------------------------------------------------------------------------------------
void Updatables::clear()
{
	objects_.clear();
}

//-------------------------------------------------------------------------------------
bool Updatables::add(Updatable* updatable)
{
	// 由于没有大量优先级需求，因此这里固定优先级数组
	if (objects_.size() == 0)
	{
		objects_.push_back(std::vector<Updatable*>());
		objects_.push_back(std::vector<Updatable*>());
	}

	KBE_ASSERT(updatable->updatePriority() < objects_.size());

	std::vector<Updatable*>& pools = objects_[updatable->updatePriority()];

	// 删除只留下空槽；新对象始终追加，保持旧 map 的注册顺序，避免同 Tick 重排更新顺序。
	// Removal leaves a hole; new objects always append so registration order and same-tick ordering stay stable.
	pools.push_back(updatable);
	updatable->removeIdx = static_cast<int>(pools.size() - 1);

	return true;
}

//-------------------------------------------------------------------------------------
bool Updatables::remove(Updatable* updatable)
{
	if (updatable->removeIdx < 0 || updatable->updatePriority() >= objects_.size())
		return false;

	std::vector<Updatable*>& pools = objects_[updatable->updatePriority()];
	const size_t index = static_cast<size_t>(updatable->removeIdx);
	if (index >= pools.size() || pools[index] != updatable)
		return false;

	pools[index] = NULL;
	updatable->removeIdx = -1;
	return true;
}

//-------------------------------------------------------------------------------------
void Updatables::update()
{
	SCOPED_PROFILE(UPDATABLES_PROFILE);

	std::vector< std::vector<Updatable*> >::iterator fpIter = objects_.begin();
	for (; fpIter != objects_.end(); ++fpIter)
	{
		std::vector<Updatable*>& pools = (*fpIter);
		for (size_t index = 0; index < pools.size(); ++index)
		{
			Updatable* updatable = pools[index];
			if (updatable != NULL && !updatable->update() && pools[index] == updatable)
				remove(updatable);
		}
	}
}

//-------------------------------------------------------------------------------------
}
