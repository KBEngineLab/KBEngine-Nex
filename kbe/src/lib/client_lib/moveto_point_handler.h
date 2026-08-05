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

#ifndef KBE_CLIENT_MOVETOPOINTHANDLER_H
#define KBE_CLIENT_MOVETOPOINTHANDLER_H

#include "pyscript/scriptobject.h"	
#include "math/math.h"
#include "script_callbacks.h"

namespace KBEngine{
namespace client
{

// These counters are updated by the client event-loop thread and read by Bots Watchers
// on the same thread. Keeping them process-wide avoids per-Entity allocations in the
// movement hot path while still exposing whether scheduler lateness affects movement.
// 这些计数由客户端事件循环线程更新，并由同线程的 Bots Watcher 读取。使用进程级
// 计数可避免在移动热路径给每个 Entity 增加分配，同时仍能观测调度迟到对移动的影响。
extern uint64 g_moveControllerStarts;
extern uint64 g_moveControllerReplacements;
extern uint64 g_moveControllerCompletions;
extern uint64 g_moveUpdateCalls;
extern uint64 g_moveDelayedUpdates;
extern uint64 g_moveSkippedTicks;
extern uint64 g_moveCatchupClamps;
extern uint64 g_moveMaxElapsedMicros;

class Entity;
class MoveToPointHandler : public ScriptCallbackHandler
{
public:
	enum MoveType
	{
		MOVE_TYPE_POINT = 0,		// 常规类型
		MOVE_TYPE_ENTITY = 1,		// 范围触发器类型
		MOVE_TYPE_NAV = 2,			// 移动控制器类型
	};

	MoveToPointHandler(ScriptCallbacks& scriptCallbacks, client::Entity* pEntity, int layer, 
		const Position3D& destPos, float velocity, float distance, bool faceMovement, 
		bool moveVertically, PyObject* userarg);

	MoveToPointHandler();
	virtual ~MoveToPointHandler();
	
	virtual bool update(TimerHandle& handle);

	virtual const Position3D& destPos(){ return destPos_; }
	virtual bool requestMoveOver(TimerHandle& handle, const Position3D& oldPos);

	virtual bool isOnGround(){ return false; }

	virtual MoveType type() const{ return MOVE_TYPE_POINT; }

protected:
	virtual void handleTimeout( TimerHandle handle, void * pUser );
	virtual void onRelease( TimerHandle handle, void * /*pUser*/ );

protected:
	Position3D destPos_;
	float velocity_;			// 速度
	bool faceMovement_;			// 是否不改变面向移动
	bool moveVertically_;		// true则可以飞起来移动否则贴地
	PyObject* pyuserarg_;
	float distance_;
	int layer_;
	client::Entity* pEntity_;
	uint64 lastUpdateTimestamp_;
};
 
}
}
#endif // KBE_CLIENT_MOVETOPOINTHANDLER_H

