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


#ifndef KBE_MEMORY_HELPER_H
#define KBE_MEMORY_HELPER_H

#include "common/common.h"
#include "common/kbemalloc.h"
#include "helper/debug_helper.h"

namespace KBEngine{
	// VLD从未在当前运行时启用；保留空钩子以维持现有组件启动调用的API兼容性。
	// VLD was never enabled by the current runtime; retain the no-op hook to preserve the component startup API.
	inline void startLeakDetection(COMPONENT_TYPE type, COMPONENT_ID id)
	{
	}
}

namespace KBEngine{



}

#endif // KBE_MEMORY_HELPER_H
