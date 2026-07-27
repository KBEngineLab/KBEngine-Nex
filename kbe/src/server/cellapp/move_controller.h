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

#ifndef KBE_MOVETOPOINTCONTROLLERBASE_H
#define KBE_MOVETOPOINTCONTROLLERBASE_H

#include "controller.h"
#include "updatable.h"
#include "moveto_point_handler.h"
#include "pyscript/scriptobject.h"	

namespace KBEngine{

class MoveController : public Controller
{
public:
	MoveController(Entity* pEntity, MoveToPointHandler* pMoveToPointHandler = NULL, uint32 id = 0);
	virtual ~MoveController();
	
	void pMoveToPointHandler(MoveToPointHandler* pMoveToPointHandler)
		{ pMoveToPointHandler_ = pMoveToPointHandler; }

	virtual void destroy();
	virtual void addToStream(KBEngine::MemoryStream& s);
	virtual void createFromStream(KBEngine::MemoryStream& s);

	float velocity() const {
		return pMoveToPointHandler_->velocity();
	}

	void velocity(float v) {
		pMoveToPointHandler_->velocity(v);
	}

	// 流恢复完成后统一绑定共享控制器，避免处理器持有无所有权保证的裸指针。
	// Bind the shared controller after stream restoration so the handler never relies on an unowned raw pointer.
	void bindHandlerController(const KBEShared_ptr<Controller>& controller) {
		pMoveToPointHandler_->pController(controller);
	}

protected:
	MoveToPointHandler* pMoveToPointHandler_;
};
 
}
#endif // KBE_MOVETOPOINTCONTROLLERBASE_H

