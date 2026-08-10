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

#include "cellapp.h"
#include "entity.h"
#include "navigate_handler.h"	
#include "navigation/navigation.h"
#include "space.h"
#include "spaces.h"

namespace KBEngine{	


//-------------------------------------------------------------------------------------
NavigateHandler::NavigateHandler(KBEShared_ptr<Controller>& pController, const Position3D& destPos, 
											 float velocity, float distance, bool faceMovement, 
											 float maxMoveDistance, VECTOR_POS3D_PTR paths_ptr,
											PyObject* userarg):
MoveToPointHandler(pController, pController->pEntity()->layer(), pController->pEntity()->position(), velocity, distance, faceMovement, false, userarg),
destPosIdx_(0),
paths_(paths_ptr),
maxMoveDistance_(maxMoveDistance),
useDetour_(false),
navHandle_(),
currentPolygon_(NavMeshHandle::INVALID_NAVMESH_POLYREF),
straightPath_(),
straightPathIndex_(0),
lookAheadDistance_(2.0f),
retryCount_(0)
{
	destPos_ = (*paths_)[destPosIdx_++];
	
	updatableName = "NavigateHandler";
}

//-------------------------------------------------------------------------------------
NavigateHandler::NavigateHandler(KBEShared_ptr<Controller>& pController, const Position3D& destPos,
	float velocity, float distance, bool faceMovement, float maxMoveDistance,
	int8 layer, PyObject* userarg):
MoveToPointHandler(pController, layer, destPos, velocity, distance, faceMovement, false, userarg),
destPosIdx_(0),
paths_(),
maxMoveDistance_(maxMoveDistance),
useDetour_(true),
navHandle_(),
currentPolygon_(NavMeshHandle::INVALID_NAVMESH_POLYREF),
straightPath_(),
straightPathIndex_(0),
lookAheadDistance_(2.0f),
retryCount_(0)
{
	updatableName = "NavigateHandler";
}

//-------------------------------------------------------------------------------------
NavigateHandler::NavigateHandler():
MoveToPointHandler(),
destPosIdx_(0),
paths_(),
maxMoveDistance_(0.f),
useDetour_(false),
navHandle_(),
currentPolygon_(NavMeshHandle::INVALID_NAVMESH_POLYREF),
straightPath_(),
straightPathIndex_(0),
lookAheadDistance_(2.0f),
retryCount_(0)
{
	updatableName = "NavigateHandler";
}

//-------------------------------------------------------------------------------------
NavigateHandler::NavigateHandler(bool useDetour):
MoveToPointHandler(),
destPosIdx_(0),
paths_(),
maxMoveDistance_(0.f),
useDetour_(useDetour),
navHandle_(),
currentPolygon_(NavMeshHandle::INVALID_NAVMESH_POLYREF),
straightPath_(),
straightPathIndex_(0),
lookAheadDistance_(2.0f),
retryCount_(0)
{
	updatableName = "NavigateHandler";
}

//-------------------------------------------------------------------------------------
NavigateHandler::~NavigateHandler()
{
}

//-------------------------------------------------------------------------------------
void NavigateHandler::addToStream(KBEngine::MemoryStream& s)
{
	MoveToPointHandler::addToStream(s);
	s << maxMoveDistance_;
}

//-------------------------------------------------------------------------------------
void NavigateHandler::createFromStream(KBEngine::MemoryStream& s)
{
	MoveToPointHandler::createFromStream(s);
	s >> maxMoveDistance_;
}

//-------------------------------------------------------------------------------------
bool NavigateHandler::resetNavigate(const Position3D& destPos, float velocity, float distance, bool faceMovement,
	float maxMoveDistance, VECTOR_POS3D_PTR paths_ptr, int8 layer, PyObject* userarg, bool useDetour)
{
	if (isDestroyed_ || useDetour_ != useDetour)
		return false;

	if (!useDetour_ && (!paths_ptr || paths_ptr->empty()))
		return false;

	destPos_ = destPos;
	velocity_ = velocity;
	distance_ = distance;
	faceMovement_ = faceMovement;
	maxMoveDistance_ = maxMoveDistance;
	layer_ = layer;
	retryCount_ = 0;

	if (pyuserarg_ != userarg)
	{
		Py_INCREF(userarg);
		Py_DECREF(pyuserarg_);
		pyuserarg_ = userarg;
	}

	if (useDetour_)
	{
		// 追敌重定向只丢弃路径缓存，不销毁控制器，避免频繁 navigate 造成一帧移动空窗。
		// Retargeting drops only cached path state, avoiding the one-frame gap caused by destroy-and-create.
		navHandle_.clear();
		invalidateDetourPath();
	}
	else
	{
		paths_ = paths_ptr;
		destPosIdx_ = 0;
		destPos_ = (*paths_)[destPosIdx_++];
	}

	return true;
}

//-------------------------------------------------------------------------------------
bool NavigateHandler::requestMoveOver(const Position3D& oldPos)
{
	if (useDetour_)
		return MoveToPointHandler::requestMoveOver(oldPos);

	if(destPosIdx_ == ((int)paths_->size()))
		return MoveToPointHandler::requestMoveOver(oldPos);
	else
		destPos_ = (*paths_)[destPosIdx_++];

	return false;
}

//-------------------------------------------------------------------------------------
void NavigateHandler::invalidateDetourPath()
{
	currentPolygon_ = NavMeshHandle::INVALID_NAVMESH_POLYREF;
	straightPath_.clear();
	straightPathIndex_ = 0;
}

//-------------------------------------------------------------------------------------
bool NavigateHandler::buildDetourPath(const Position3D& currentPosition)
{
	if (!pController_ || !pController_->pEntity())
		return false;

	Space* pSpace = Spaces::findSpace(pController_->pEntity()->spaceID());
	if (pSpace == NULL || !pSpace->isGood())
		return false;

	// 每次重建路径都从 Space 重新取得句柄，使几何重载后的控制器不会继续使用旧网格。
	// Reacquire the handle for every rebuild so a controller never keeps using stale geometry after a space reload.
	navHandle_ = pSpace->pNavHandle();
	if (!navHandle_ || navHandle_->type() != NavigationHandle::NAV_MESH)
		return false;

	NavMeshHandle* pNavMesh = static_cast<NavMeshHandle*>(navHandle_.get());
	straightPath_.clear();
	if (pNavMesh->findStraightPath(layer_, currentPosition, destPos_, straightPath_) <= 0 || straightPath_.empty())
		return false;

	currentPolygon_ = pNavMesh->findNearestPoly(layer_, currentPosition, NULL);
	if (currentPolygon_ == NavMeshHandle::INVALID_NAVMESH_POLYREF)
		return false;

	straightPathIndex_ = 0;
	while (straightPathIndex_ < straightPath_.size() &&
		(straightPath_[straightPathIndex_] - currentPosition).squaredLength() <= 0.0025f)
	{
		++straightPathIndex_;
	}

	return true;
}

//-------------------------------------------------------------------------------------
bool NavigateHandler::requestMoveFailure()
{
	if (pController_)
	{
		if (pController_->pEntity())
			pController_->pEntity()->onMoveFailure(pController_->id(), pyuserarg_);

		pController_->destroy();
	}

	return true;
}

//-------------------------------------------------------------------------------------
bool NavigateHandler::updateDetour(bool deleteOnFinish)
{
	if (isDestroyed_)
	{
		if (deleteOnFinish)
			delete this;
		return false;
	}

	if (!pController_ || !pController_->pEntity())
	{
		requestMoveFailure();
		if (deleteOnFinish)
			delete this;
		return false;
	}

	Entity* pEntity = pController_->pEntity();
	Py_INCREF(pEntity);
	Position3D currentPosition = pEntity->position();
	Position3D oldPosition = currentPosition;
	const float arrivalDistance = std::max(distance_, 0.05f);

	Space* pSpace = Spaces::findSpace(pEntity->spaceID());
	if (pSpace == NULL || !pSpace->isGood())
	{
		requestMoveFailure();
		Py_DECREF(pEntity);
		if (deleteOnFinish)
			delete this;
		return false;
	}

	NavigationHandlePtr currentNavHandle = pSpace->pNavHandle();
	if (navHandle_.get() != currentNavHandle.get())
	{
		// Space 更换句柄后必须丢弃旧 polygon 与直线路径，不能在旧网格上继续推进实体。
		// A Space handle change must discard the old polygon and straight path so the entity cannot continue on the old mesh.
		navHandle_.clear();
		invalidateDetourPath();
		retryCount_ = 0;
	}

	if (pSpace->isGeometryLoading())
	{
		Py_DECREF(pEntity);
		return true;
	}

	if ((destPos_ - currentPosition).squaredLength() <= arrivalDistance * arrivalDistance)
	{
		requestMoveOver(oldPosition);
		Py_DECREF(pEntity);
		if (deleteOnFinish)
			delete this;
		return false;
	}

	if (straightPath_.empty() || straightPathIndex_ >= straightPath_.size())
	{
		if (!buildDetourPath(currentPosition))
		{
			if (++retryCount_ > 5)
			{
				requestMoveFailure();
				Py_DECREF(pEntity);
				if (deleteOnFinish)
					delete this;
				return false;
			}

			Py_DECREF(pEntity);
			return true;
		}
	}

	// 与旧版 Detour 行为保持一致：不是每 tick 只追最近拐点，而是向前看一小段。
	// 这样在拐点很近、实体贴边或 Detour 投影点略有抖动时，不会连续产生“几乎不动”的假失败。
	// Keep the legacy Detour semantics: look a little ahead instead of chasing only the nearest corner.
	Position3D moveTarget = currentPosition;
	float remainingLookAhead = lookAheadDistance_;
	size_t cornerPathIndex = straightPathIndex_;
	size_t nextPathIndex = straightPathIndex_;
	while (nextPathIndex < straightPath_.size() && remainingLookAhead > 0.f)
	{
		Vector3 segment = straightPath_[nextPathIndex] - moveTarget;
		float segmentLength = segment.length();
		if (segmentLength > remainingLookAhead)
		{
			segment *= 1.f / segmentLength;
			moveTarget += segment * remainingLookAhead;
			break;
		}

		moveTarget = straightPath_[nextPathIndex];
		remainingLookAhead -= segmentLength;
		++nextPathIndex;
	}

	straightPathIndex_ = nextPathIndex;
	Vector3 movement = moveTarget - currentPosition;
	float movementLength = movement.length();
	if (movementLength <= 0.05f || velocity_ <= 0.f)
	{
		++straightPathIndex_;
		if (straightPathIndex_ >= straightPath_.size())
		{
			requestMoveOver(oldPosition);
			Py_DECREF(pEntity);
			if (deleteOnFinish)
				delete this;
			return false;
		}

		Py_DECREF(pEntity);
		return true;
	}

	// movementLength 已计算，直接复用倒数，避免再次计算平方根。
	// Reuse movementLength instead of normalizing with a second square root.
	movement *= 1.f / movementLength;
	float stepDistance = std::min(velocity_, movementLength);
	if (maxMoveDistance_ > 0.f)
		stepDistance = std::min(stepDistance, maxMoveDistance_);
	movement *= stepDistance;

	NavMeshHandle* pNavMesh = static_cast<NavMeshHandle*>(navHandle_.get());
	Position3D nextPosition;
	if (!pNavMesh->moveAlongSurface(layer_, currentPolygon_, currentPosition,
		currentPosition + movement, nextPosition))
	{
		if (++retryCount_ > 5)
		{
			requestMoveFailure();
			Py_DECREF(pEntity);
			if (deleteOnFinish)
				delete this;
			return false;
		}

		invalidateDetourPath();
		Py_DECREF(pEntity);
		return true;
	}

	if ((nextPosition - currentPosition).squaredLength() <= 0.00000001f &&
		cornerPathIndex < straightPath_.size())
	{
		Vector3 cornerMovement = straightPath_[cornerPathIndex] - currentPosition;
		float cornerMovementLength = cornerMovement.length();
		if (cornerMovementLength > 0.05f)
		{
			// 前视点可能已经越过拐角，直线推进会切到障碍边界上。此时先朝当前拐点移动，
			// 让实体真正完成转弯，再恢复前视移动，避免在栅栏/墙角处持续投影不动。
			// The look-ahead target can be past the corner and the straight steering vector may cut into an obstacle.
			// Fall back to the current corner first so the entity can complete the turn.
			cornerMovement *= 1.f / cornerMovementLength;
			float cornerStepDistance = std::min(velocity_, cornerMovementLength);
			if (maxMoveDistance_ > 0.f)
				cornerStepDistance = std::min(cornerStepDistance, maxMoveDistance_);
			cornerMovement *= cornerStepDistance;

			dtPolyRef cornerPolygon = currentPolygon_;
			Position3D cornerPosition;
			if (pNavMesh->moveAlongSurface(layer_, cornerPolygon, currentPosition,
				currentPosition + cornerMovement, cornerPosition) &&
				(cornerPosition - currentPosition).squaredLength() > 0.00000001f)
			{
				currentPolygon_ = cornerPolygon;
				straightPathIndex_ = cornerPathIndex;
				nextPosition = cornerPosition;
				movement = cornerMovement;
			}
		}
	}

	float groundHeight = nextPosition.y;
	if (!pNavMesh->getPolyHeight(layer_, currentPolygon_, nextPosition, groundHeight))
	{
		// 高度查询失败通常是 polyRef 在边界上暂时失效，旧版只重建路径不触发 onMoveFailure。
		// Height lookup failures are usually transient boundary polyRef misses, so rebuild instead of failing.
		invalidateDetourPath();
		Py_DECREF(pEntity);
		return true;
	}

	if ((nextPosition - currentPosition).squaredLength() <= 0.00000001f)
	{
		// 拐角处 Detour 可能返回成功但本 tick 几乎没有推进。此时如果立即重建路径，
		// straightPathIndex_ 会回到起点，下一 tick 可能再次选择同一段方向并卡在角上。
		// 先尝试推进到下一个拐点，保留当前路径上下文，让后续 tick 有机会绕过角。
		// At corners Detour may succeed without moving. Rebuilding immediately resets the path index
		// and can choose the same blocked segment again, so first keep the corridor and skip a corner.
		if (straightPathIndex_ + 1 < straightPath_.size())
			++straightPathIndex_;
		else
			invalidateDetourPath();

		Py_DECREF(pEntity);
		return true;
	}

	retryCount_ = 0;
	nextPosition.y = groundHeight;
	Direction3D direction = pEntity->direction();
	if (faceMovement_ && (movement.x != 0.f || movement.z != 0.f))
		direction.yaw(movement.yaw());

	if (!isDestroyed_)
		pEntity->setPositionAndDirection(nextPosition, direction);
	if (!isDestroyed_)
		pEntity->isOnGround(true);
	if (!isDestroyed_)
		pEntity->onMove(pController_->id(), layer_, oldPosition, pyuserarg_);

	if (isDestroyed_ || (destPos_ - nextPosition).squaredLength() <= arrivalDistance * arrivalDistance)
	{
		if (!isDestroyed_)
			requestMoveOver(oldPosition);

		Py_DECREF(pEntity);
		if (deleteOnFinish)
			delete this;
		return false;
	}

	Py_DECREF(pEntity);
	return true;
}

//-------------------------------------------------------------------------------------
bool NavigateHandler::update()
{
	if (useDetour_)
		return updateDetour(true);

	return MoveToPointHandler::update();
}

//-------------------------------------------------------------------------------------
bool NavigateHandler::stepMoveOnceWithoutDelete()
{
	if (!useDetour_)
		return MoveToPointHandler::update();

	return updateDetour(false);
}

//-------------------------------------------------------------------------------------
}

