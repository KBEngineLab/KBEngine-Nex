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

#ifndef KBE_UPDATABLES_H
#define KBE_UPDATABLES_H

// common include
#include "helper/debug_helper.h"
#include "common/common.h"
#include "updatable.h"	
#include <vector>
// #define NDEBUG
// windows include	
#if KBE_PLATFORM == PLATFORM_WIN32	
#else
// linux include
#endif

namespace KBEngine{

class Updatables
{
public:
	Updatables();
	~Updatables();

	void clear();

	bool add(Updatable* updatable);
	bool remove(Updatable* updatable);

	void update();

private:
	// Updatable 以单调 removeIdx 保持优先级内顺序；连续槽位避免每 Tick 遍历红黑树节点。
	// Monotonic removeIdx preserves per-priority order while contiguous slots avoid tree-node pointer chasing each tick.
	std::vector< std::vector<Updatable*> > objects_;
};

}
#endif
