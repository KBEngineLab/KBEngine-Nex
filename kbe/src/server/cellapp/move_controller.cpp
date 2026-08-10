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
#include "move_controller.h"	
#include "moveto_point_handler.h"	
#include "moveto_entity_handler.h"	
#include "navigate_handler.h"	

namespace KBEngine{	


//-------------------------------------------------------------------------------------
MoveController::MoveController(Entity* pEntity, MoveToPointHandler* pMoveToPointHandler, uint32 id):
Controller(Controller::CONTROLLER_TYPE_MOVE, pEntity, 0, id),
pMoveToPointHandler_(pMoveToPointHandler)
{
}

//-------------------------------------------------------------------------------------
MoveController::~MoveController()
{
	// DEBUG_MSG(fmt::format("MoveController::~MoveController(): {:p}\n", (void*)this);
	if (pMoveToPointHandler_)
	{
		pMoveToPointHandler_->destroy();
		pMoveToPointHandler_ = NULL;
	}
}

//-------------------------------------------------------------------------------------
void MoveController::addToStream(KBEngine::MemoryStream& s)
{
	Controller::addToStream(s);

	uint8 utype = pMoveToPointHandler_->type();
	s << utype;

	pMoveToPointHandler_->addToStream(s);
}

//-------------------------------------------------------------------------------------
void MoveController::createFromStream(KBEngine::MemoryStream& s)
{
	Controller::createFromStream(s);
	KBE_ASSERT(pMoveToPointHandler_ == NULL);

	uint8 utype;
	s >> utype;

	if(utype == MoveToPointHandler::MOVE_TYPE_NAV_DETOUR)
		pMoveToPointHandler_ = new NavigateHandler(true);
	else if(utype == MoveToPointHandler::MOVE_TYPE_NAV)
		pMoveToPointHandler_ = new NavigateHandler();
	else if(utype == MoveToPointHandler::MOVE_TYPE_ENTITY)
		pMoveToPointHandler_ = new MoveToEntityHandler();
	else if(utype == MoveToPointHandler::MOVE_TYPE_POINT)
		pMoveToPointHandler_ = new MoveToPointHandler();
	else
		KBE_ASSERT(false);

	pMoveToPointHandler_->createFromStream(s);
}

//-------------------------------------------------------------------------------------
bool MoveController::resetNavigate(const Position3D& destPos, float velocity, float distance, bool faceMovement,
	float maxMoveDistance, VECTOR_POS3D_PTR paths_ptr, int8 layer, PyObject* userarg, bool useDetour)
{
	if (pMoveToPointHandler_ == NULL)
		return false;

	const MoveToPointHandler::MoveType expectedType = useDetour ?
		MoveToPointHandler::MOVE_TYPE_NAV_DETOUR : MoveToPointHandler::MOVE_TYPE_NAV;
	if (pMoveToPointHandler_->type() != expectedType)
		return false;

	NavigateHandler* pNavigateHandler = static_cast<NavigateHandler*>(pMoveToPointHandler_);
	return pNavigateHandler->resetNavigate(destPos, velocity, distance, faceMovement,
		maxMoveDistance, paths_ptr, layer, userarg, useDetour);
}

//-------------------------------------------------------------------------------------
void MoveController::destroy()
{
	Controller::destroy();

	// 既然自己要销毁了，那么与自己相联的updatable也应该停止了
	if (pMoveToPointHandler_)
	{
		pMoveToPointHandler_->destroy();
		pMoveToPointHandler_ = NULL;
	}
}

//-------------------------------------------------------------------------------------
}

