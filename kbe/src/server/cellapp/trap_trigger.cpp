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

#include "trap_trigger.h"
#include "entity.h"
#include "entity_coordinate_node.h"
#include "proximity_controller.h"	

#ifndef CODE_INLINE
#include "trap_trigger.inl"
#endif

namespace KBEngine{	


//-------------------------------------------------------------------------------------
TrapTrigger::TrapTrigger(CoordinateNode* origin, ProximityController* pProximityController, float xz, float y):
RangeTrigger(origin, xz, y),
pProximityController_(pProximityController)
{
}

//-------------------------------------------------------------------------------------
TrapTrigger::~TrapTrigger()
{
}

//-------------------------------------------------------------------------------------
void TrapTrigger::onEnter(CoordinateNode * pNode)
{
	if((pNode->flags() & COORDINATE_NODE_FLAG_ENTITY) <= 0)
		return;

	Entity* pEntity = static_cast<EntityCoordinateNode*>(pNode)->pEntity();
	if (pEntity == NULL || pEntity->isDestroyed() || !pEntity->isReal())
	{
		// One trap callback may synchronously migrate the entering Entity. The
		// coordinate traversal can still visit other overlapping triggers in the
		// same stack frame, but those callbacks must not execute against the Source
		// Ghost because only the authoritative real Entity owns gameplay effects.
		// 一个 Trap 回调可能同步迁移进入者；同一坐标遍历仍可能继续访问其他重叠
		// Trigger，但后续回调不得作用于 Source Ghost，游戏逻辑只能由权威 Real Entity 执行。
		return;
	}

	pProximityController_->onEnter(pEntity, range_xz_, range_y_);
}

//-------------------------------------------------------------------------------------
void TrapTrigger::onLeave(CoordinateNode * pNode)
{
	if((pNode->flags() & COORDINATE_NODE_FLAG_ENTITY) <= 0)
		return;

	Entity* pEntity = static_cast<EntityCoordinateNode*>(pNode)->pEntity();
	if (pEntity == NULL || pEntity->isDestroyed() || !pEntity->isReal())
	{
		// Ghost nodes may be removed after their real Entity has already completed
		// the authoritative leave path. Suppress duplicate script callbacks during
		// that cleanup without weakening the coordinate-system teardown itself.
		// Ghost 节点可能在 Real Entity 已完成权威离开流程后才被移除；此处只抑制
		// 清理阶段的重复脚本回调，不改变坐标系统自身的拆除过程。
		return;
	}

	pProximityController_->onLeave(pEntity, range_xz_, range_y_);
}

//-------------------------------------------------------------------------------------
}
