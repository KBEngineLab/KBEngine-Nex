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
Updatables::Updatables():
updating_(false)
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
	freeIndices_.clear();
	pendingFreeIndices_.clear();
}

//-------------------------------------------------------------------------------------
bool Updatables::add(Updatable* updatable)
{
	// 由于没有大量优先级需求，因此这里固定优先级数组
	if (objects_.size() == 0)
	{
		objects_.push_back(std::vector<Updatable*>());
		objects_.push_back(std::vector<Updatable*>());
		freeIndices_.push_back(std::vector<int>());
		freeIndices_.push_back(std::vector<int>());
		pendingFreeIndices_.push_back(std::vector<int>());
		pendingFreeIndices_.push_back(std::vector<int>());
	}

	KBE_ASSERT(updatable->updatePriority() < objects_.size());

	std::vector<Updatable*>& pools = objects_[updatable->updatePriority()];
	std::vector<int>& freeSlots = freeIndices_[updatable->updatePriority()];

	if (!freeSlots.empty())
	{
		const int index = freeSlots.back();
		freeSlots.pop_back();
		KBE_ASSERT(index >= 0 && static_cast<size_t>(index) < pools.size() && pools[index] == NULL);
		pools[index] = updatable;
		updatable->removeIdx = index;
	}
	else
	{
		pools.push_back(updatable);
		updatable->removeIdx = static_cast<int>(pools.size() - 1);
	}

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
	if (updating_)
		pendingFreeIndices_[updatable->updatePriority()].push_back(static_cast<int>(index));
	else
		freeIndices_[updatable->updatePriority()].push_back(static_cast<int>(index));
	updatable->removeIdx = -1;
	return true;
}

//-------------------------------------------------------------------------------------
void Updatables::update()
{
	SCOPED_PROFILE(UPDATABLES_PROFILE);
	updating_ = true;

	std::vector< std::vector<Updatable*> >::iterator fpIter = objects_.begin();
	for (; fpIter != objects_.end(); ++fpIter)
	{
		std::vector<Updatable*>& pools = (*fpIter);
		for (size_t index = 0; index < pools.size(); ++index)
		{
			Updatable* updatable = pools[index];
			// update() 可能在处理器内部 delete this；返回后只能比较槽位并清空，不能再次访问对象。
			// update() may delete this inside the handler; after return only inspect the slot and never dereference the object.
			if (updatable != NULL && !updatable->update() && pools[index] == updatable)
			{
				pools[index] = NULL;
				pendingFreeIndices_[static_cast<size_t>(fpIter - objects_.begin())].push_back(static_cast<int>(index));
			}
		}
	}

	updating_ = false;
	for (size_t priority = 0; priority < pendingFreeIndices_.size(); ++priority)
	{
		freeIndices_[priority].insert(freeIndices_[priority].end(),
			pendingFreeIndices_[priority].begin(), pendingFreeIndices_[priority].end());
		pendingFreeIndices_[priority].clear();
	}
}

//-------------------------------------------------------------------------------------
}
