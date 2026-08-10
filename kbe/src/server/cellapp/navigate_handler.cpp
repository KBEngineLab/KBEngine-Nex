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
polyRef_(NavMeshHandle::INVALID_NAVMESH_POLYREF),
straightPath_(),
currentPathIndex_(0),
pathValid_(false),
lookAheadDistance_(2.0f),
retryCount_(0)
{
	destPos_ = (*paths_)[destPosIdx_++];
	
	updatableName = "NavigateHandler";
}

//-------------------------------------------------------------------------------------
NavigateHandler::NavigateHandler(KBEShared_ptr<Controller>& pController, const Position3D& destPos,
	float velocity, float distance, bool faceMovement, float maxMoveDistance,
	int8 layer, VECTOR_POS3D_PTR paths_ptr, PyObject* userarg):
MoveToPointHandler(pController, layer, destPos, velocity, distance, faceMovement, false, userarg),
destPosIdx_(0),
paths_(),
maxMoveDistance_(maxMoveDistance),
useDetour_(true),
navHandle_(),
polyRef_(NavMeshHandle::INVALID_NAVMESH_POLYREF),
straightPath_(),
currentPathIndex_(0),
pathValid_(false),
lookAheadDistance_(2.0f),
retryCount_(0)
{
	// Entity::navigate() already performs the synchronous reachability query. Reuse that
	// path so the first Detour tick only initializes its corridor polygon instead of searching twice.
	// Entity::navigate() 已同步完成可达性查询，首次 Tick 直接复用路径，只初始化 corridor 多边形。
	if (paths_ptr && !paths_ptr->empty() && pController->pEntity())
	{
		Space* pSpace = Spaces::findSpace(pController->pEntity()->spaceID());
		if (pSpace && pSpace->isGood())
		{
			navHandle_ = pSpace->pNavHandle();
			if (navHandle_ && navHandle_->type() == NavigationHandle::NAV_MESH)
			{
				straightPath_ = *paths_ptr;
				NavMeshHandle* pNavMesh = static_cast<NavMeshHandle*>(navHandle_.get());
				polyRef_ = pNavMesh->findNearestPoly(layer_, pController->pEntity()->position(), NULL);
				pathValid_ = polyRef_ != NavMeshHandle::INVALID_NAVMESH_POLYREF;
			}
		}
	}

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
polyRef_(NavMeshHandle::INVALID_NAVMESH_POLYREF),
straightPath_(),
currentPathIndex_(0),
pathValid_(false),
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
polyRef_(NavMeshHandle::INVALID_NAVMESH_POLYREF),
straightPath_(),
currentPathIndex_(0),
pathValid_(false),
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
	if (isDestroyed_ || !useDetour_ || !useDetour || !paths_ptr || paths_ptr->empty() || !pController_)
		return false;

	Entity* pEntity = pController_->pEntity();
	if (!pEntity)
		return false;

	Space* pSpace = Spaces::findSpace(pEntity->spaceID());
	if (!pSpace || !pSpace->isGood() || pSpace->isGeometryLoading())
		return false;

	NavigationHandlePtr currentNavHandle = pSpace->pNavHandle();
	if (!currentNavHandle || currentNavHandle->type() != NavigationHandle::NAV_MESH)
		return false;

	destPos_ = destPos;
	velocity_ = velocity;
	distance_ = distance;
	faceMovement_ = faceMovement;
	maxMoveDistance_ = maxMoveDistance;
	layer_ = layer;

	if (pyuserarg_ != userarg)
	{
		Py_INCREF(userarg);
		Py_DECREF(pyuserarg_);
		pyuserarg_ = userarg;
	}

	navHandle_ = currentNavHandle;
	straightPath_ = *paths_ptr;
	currentPathIndex_ = 0;
	const Position3D currentPosition = pEntity->position();
	while (currentPathIndex_ < static_cast<int>(straightPath_.size()) &&
		(straightPath_[currentPathIndex_] - currentPosition).squaredLength() <= 0.0025f)
	{
		++currentPathIndex_;
	}

	polyRef_ = static_cast<NavMeshHandle*>(navHandle_.get())->findNearestPoly(layer_, currentPosition, NULL);
	pathValid_ = polyRef_ != NavMeshHandle::INVALID_NAVMESH_POLYREF &&
		currentPathIndex_ < static_cast<int>(straightPath_.size());
	retryCount_ = 0;
	return pathValid_;
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
	pathValid_ = false;
	polyRef_ = NavMeshHandle::INVALID_NAVMESH_POLYREF;
	straightPath_.clear();
	currentPathIndex_ = 0;
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

	polyRef_ = pNavMesh->findNearestPoly(layer_, currentPosition, NULL);
	if (polyRef_ == NavMeshHandle::INVALID_NAVMESH_POLYREF)
		return false;

	currentPathIndex_ = 0;
	while (currentPathIndex_ < (int)straightPath_.size() &&
		(straightPath_[currentPathIndex_] - currentPosition).squaredLength() <= 0.0025f)
	{
		++currentPathIndex_;
	}
	pathValid_ = true;

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
	pEntity->isOnNavigate(true);
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

	NavigationHandlePtr currentNavHandle = pSpace->pNavHandle();
	if (navHandle_.get() != currentNavHandle.get())
	{
		// Space 重载或切换 NavMesh 后，旧 polyRef 不能继续用于 Detour corridor。
		// After a Space reload or NavMesh switch, stale polyRef must not remain in the corridor.
		navHandle_.clear();
		invalidateDetourPath();
		retryCount_ = 0;
	}

	if (!pathValid_)
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

		retryCount_ = 0;
	}

	if (straightPath_.empty())
	{
		requestMoveOver(oldPosition);
		Py_DECREF(pEntity);
		if (deleteOnFinish)
			delete this;
		return false;
	}

	Position3D moveTarget = currentPosition;
	float remainingLookAhead = lookAheadDistance_;
	const int cornerPathIndex = currentPathIndex_;
	int pathIndex = currentPathIndex_;
	while (pathIndex < (int)straightPath_.size() && remainingLookAhead > 0.f)
	{
		Vector3 segment = straightPath_[pathIndex] - moveTarget;
		float segmentLength = segment.length();
		if (segmentLength > remainingLookAhead)
		{
			segment *= 1.f / segmentLength;
			moveTarget += segment * remainingLookAhead;
			break;
		}

		moveTarget = straightPath_[pathIndex];
		remainingLookAhead -= segmentLength;
		++pathIndex;
	}

	currentPathIndex_ = pathIndex;
	Vector3 movement = moveTarget - currentPosition;
	float movementLength = movement.length();
	if (movementLength <= 0.05f || velocity_ <= 0.f)
	{
		++currentPathIndex_;
		if (currentPathIndex_ >= (int)straightPath_.size())
		{
			requestMoveOver(currentPosition);
			Py_DECREF(pEntity);
			if (deleteOnFinish)
				delete this;
			return false;
		}

		Py_DECREF(pEntity);
		return true;
	}

	movement *= 1.f / movementLength;
	float stepDistance = std::min(velocity_, movementLength);
	if (maxMoveDistance_ > 0.f)
		stepDistance = std::min(stepDistance, maxMoveDistance_);
	movement *= stepDistance;

	NavMeshHandle* pNavMesh = static_cast<NavMeshHandle*>(navHandle_.get());
	if (polyRef_ == NavMeshHandle::INVALID_NAVMESH_POLYREF)
	{
		invalidateDetourPath();
		Py_DECREF(pEntity);
		return true;
	}

	Position3D nextPosition;
	if (!pNavMesh->moveAlongSurface(layer_, polyRef_, currentPosition,
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
		cornerPathIndex < (int)straightPath_.size())
	{
		// 前视目标可能越过拐角并撞到障碍边界；先尝试当前拐点，
		// 避免每 tick 重复向同一条被阻挡的前视线投影。
		// Look-ahead can cross a corner and project to the obstacle boundary;
		// try the current corner first so subsequent ticks can leave the corner.
		Vector3 cornerMovement = straightPath_[cornerPathIndex] - currentPosition;
		const float cornerLength = cornerMovement.length();
		if (cornerLength > 0.05f)
		{
			cornerMovement *= 1.f / cornerLength;
			float cornerStepDistance = std::min(velocity_, cornerLength);
			if (maxMoveDistance_ > 0.f)
				cornerStepDistance = std::min(cornerStepDistance, maxMoveDistance_);
			cornerMovement *= cornerStepDistance;

			dtPolyRef cornerPolyRef = polyRef_;
			Position3D cornerPosition;
			if (pNavMesh->moveAlongSurface(layer_, cornerPolyRef, currentPosition,
				currentPosition + cornerMovement, cornerPosition) &&
				(cornerPosition - currentPosition).squaredLength() > 0.00000001f)
			{
				polyRef_ = cornerPolyRef;
				currentPathIndex_ = cornerPathIndex;
				nextPosition = cornerPosition;
				movement = cornerMovement;
			}
		}
	}

	float groundHeight = nextPosition.y;
	// 多边形边界上的高度查询可能瞬时失败，但 moveAlongSurface 已经给出了合法的表面位置。
	// 老版 Nex 会保留 Detour 返回的 Y 并继续提交本 Tick 位移；若在这里重建路径，
	// 尖锐转角会反复回到同一个 funnel 起点，表现为没有 onMoveFailure 的原地卡顿。
	// Height lookup can transiently fail on a polygon boundary even though moveAlongSurface
	// returned a valid surface position. Preserve that Y value so the entity can leave the corner.
	if (pNavMesh->getPolyHeight(layer_, polyRef_, nextPosition, groundHeight))
		nextPosition.y = groundHeight;

	if ((nextPosition - currentPosition).squaredLength() <= 0.00000001f)
	{
		// Detour may report success without progress exactly at a corner. Keep the
		// current path context and advance one waypoint before rebuilding.
		if (currentPathIndex_ + 1 < (int)straightPath_.size())
			++currentPathIndex_;
		else
			invalidateDetourPath();

		Py_DECREF(pEntity);
		return true;
	}

	retryCount_ = 0;
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
			requestMoveOver(nextPosition);

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
}

