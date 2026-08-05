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

#include "entity.h"
#include "config.h"
#include "moveto_point_handler.h"	

namespace KBEngine{	
namespace client
{

uint64 g_moveControllerStarts = 0;
uint64 g_moveControllerReplacements = 0;
uint64 g_moveControllerCompletions = 0;
uint64 g_moveUpdateCalls = 0;
uint64 g_moveDelayedUpdates = 0;
uint64 g_moveSkippedTicks = 0;
uint64 g_moveCatchupClamps = 0;
uint64 g_moveMaxElapsedMicros = 0;

//-------------------------------------------------------------------------------------
MoveToPointHandler::MoveToPointHandler(ScriptCallbacks& scriptCallbacks, client::Entity* pEntity, 
											int layer, const Position3D& destPos, 
											 float velocity, float distance, bool faceMovement, 
											bool moveVertically, PyObject* userarg):
ScriptCallbackHandler(scriptCallbacks, NULL),
destPos_(destPos),
velocity_(velocity),
faceMovement_(faceMovement),
moveVertically_(moveVertically),
pyuserarg_(userarg),
distance_(distance),
layer_(layer),
pEntity_(pEntity),
lastUpdateTimestamp_(timestamp())
{
}

//-------------------------------------------------------------------------------------
MoveToPointHandler::~MoveToPointHandler()
{
	if(pyuserarg_ != NULL)
	{
		Py_DECREF(pyuserarg_);
	}

	// DEBUG_MSG(fmt::format("MoveToPointHandler::~MoveToPointHandler(): {:p}\n"), (void*)this));
}

//-------------------------------------------------------------------------------------
void MoveToPointHandler::handleTimeout( TimerHandle handle, void * pUser )
{
	update(handle);
}

//-------------------------------------------------------------------------------------
void MoveToPointHandler::onRelease( TimerHandle handle, void * /*pUser*/ )
{
	scriptCallbacks_.releaseCallback(handle);
	delete this;
}

//-------------------------------------------------------------------------------------
bool MoveToPointHandler::requestMoveOver(TimerHandle& handle, const Position3D& oldPos)
{
	++g_moveControllerCompletions;
	pEntity_->onMoveOver(scriptCallbacks_.getIDForHandle(handle), layer_, oldPos, pyuserarg_);
	handle.cancel();
	return true;
}

//-------------------------------------------------------------------------------------
bool MoveToPointHandler::update(TimerHandle& handle)
{
	if(pEntity_ == NULL)
	{
		handle.cancel();
		return false;
	}
	
	Entity* pEntity = pEntity_;
	const Position3D& dstPos = destPos();
	Position3D currpos = pEntity->position();
	Position3D currpos_backup = currpos;
	Direction3D direction = pEntity->direction();

	// Periodic timers deliberately skip stale intervals after a slow event-loop turn. Movement
	// must still integrate bounded elapsed game time; otherwise one delayed turn permanently
	// loses distance and appears to players as an Avatar standing still under load.
	// 周期 Timer 在事件循环变慢后会主动跳过过期周期。移动仍需按经过的游戏 Tick 有界积分，
	// 否则一次迟到会永久丢失位移，并在负载下表现为 Avatar 长时间站立。
	const uint64 now = timestamp();
	const uint64 elapsedStamps = now >= lastUpdateTimestamp_ ? now - lastUpdateTimestamp_ : 0;
	lastUpdateTimestamp_ = now;
	const uint64 elapsedMicros = static_cast<uint64>(
		static_cast<double>(elapsedStamps) * 1000000.0 / stampsPerSecondD());
	const int configuredHertz = static_cast<int>(g_kbeSrvConfig.gameUpdateHertz());
	const uint64 hertz = static_cast<uint64>(configuredHertz > 0 ? configuredHertz : 1);
	const double elapsedTickScale = static_cast<double>(elapsedStamps) * static_cast<double>(hertz) /
		stampsPerSecondD();
	uint64 elapsedTicks = static_cast<uint64>(elapsedTickScale);
	if (elapsedTicks == 0)
		elapsedTicks = 1;

	++g_moveUpdateCalls;
	g_moveMaxElapsedMicros = KBE_MAX(g_moveMaxElapsedMicros, elapsedMicros);
	if (elapsedTicks > 1)
	{
		++g_moveDelayedUpdates;
		g_moveSkippedTicks += static_cast<uint64>(elapsedTicks - 1);
	}

	const uint64 maxCatchupTicks = hertz;
	if (elapsedTicks > maxCatchupTicks)
	{
		elapsedTicks = maxCatchupTicks;
		++g_moveCatchupClamps;
	}

	// Preserve the historical immediate first step, then use real elapsed time so a 150 ms turn
	// advances 1.5 normal steps instead of permanently losing half a step.
	// 保留历史上的首次立即步进；后续按真实时间推进，使 150ms 的一轮移动 1.5 个正常步长，
	// 而不是永久丢掉半个步长。
	double movementScale = elapsedTickScale > 1.0 ? elapsedTickScale : 1.0;
	if (movementScale > static_cast<double>(maxCatchupTicks))
		movementScale = static_cast<double>(maxCatchupTicks);
	const float movementDistance = velocity_ * static_cast<float>(movementScale);
	Vector3 movement = dstPos - currpos;
	if (!moveVertically_) movement.y = 0.f;
	
	bool ret = true;

	if(KBEVec3Length(&movement) < movementDistance + distance_)
	{
		float y = currpos.y;
		currpos = dstPos;

		if(distance_ > 0.0f)
		{
			// 单位化向量
			KBEVec3Normalize(&movement, &movement); 
			movement *= distance_;
			currpos -= movement;
		}

		if (!moveVertically_)
			currpos.y = y;

		ret = false;
	}
	else
	{
		// 单位化向量
		KBEVec3Normalize(&movement, &movement); 

		// 移动位置
		movement *= movementDistance;
		currpos += movement;
	}
	
	// 是否需要改变面向
	if (faceMovement_ && (movement.x != 0.f || movement.z != 0.f))
		direction.yaw(movement.yaw());
	
	// 设置entity的新位置和面向
	pEntity_->clientPos(currpos);
	pEntity_->clientDir(direction);

	// 非navigate都不能确定其在地面上
	pEntity_->isOnGround(false);

	// 通知脚本
	pEntity->onMove(scriptCallbacks_.getIDForHandle(handle), layer_, currpos_backup, pyuserarg_);

	// 如果达到目的地则返回true
	if(!ret)
	{
		return !requestMoveOver(handle, currpos_backup);
	}

	return true;
}

//-------------------------------------------------------------------------------------
}
}
