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
bool NavigateHandler::updateDetour()
{
	if (isDestroyed_)
	{
		delete this;
		return false;
	}

	if (!pController_ || !pController_->pEntity())
	{
		requestMoveFailure();
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
				delete this;
				return false;
			}

			Py_DECREF(pEntity);
			return true;
		}
	}

	while (straightPathIndex_ < straightPath_.size() &&
		(straightPath_[straightPathIndex_] - currentPosition).squaredLength() <= 0.0025f)
	{
		++straightPathIndex_;
	}

	if (straightPathIndex_ >= straightPath_.size())
	{
		invalidateDetourPath();
		Py_DECREF(pEntity);
		return true;
	}

	Vector3 movement = straightPath_[straightPathIndex_] - currentPosition;
	float movementLength = movement.length();
	if (movementLength <= 0.00001f || velocity_ <= 0.f)
	{
		if (++retryCount_ > 5)
		{
			requestMoveFailure();
			Py_DECREF(pEntity);
			delete this;
			return false;
		}

		invalidateDetourPath();
		Py_DECREF(pEntity);
		return true;
	}

	KBEVec3Normalize(&movement, &movement);
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
			delete this;
			return false;
		}

		invalidateDetourPath();
		Py_DECREF(pEntity);
		return true;
	}

	float groundHeight = nextPosition.y;
	if (!pNavMesh->getPolyHeight(layer_, currentPolygon_, nextPosition, groundHeight) ||
		(nextPosition - currentPosition).squaredLength() <= 0.00000001f)
	{
		if (++retryCount_ > 5)
		{
			requestMoveFailure();
			Py_DECREF(pEntity);
			delete this;
			return false;
		}

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
		return updateDetour();

	return MoveToPointHandler::update();
}

//-------------------------------------------------------------------------------------
}

