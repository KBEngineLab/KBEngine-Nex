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

#ifndef KBE_SPACEMANAGER_H
#define KBE_SPACEMANAGER_H

#include "helper/debug_helper.h"
#include "common/common.h"
#include "common/singleton.h"
#include "updatable.h"
#include "space.h"
#include "space_load_snapshot.h"

namespace KBEngine{

class Spaces
{
public:
	Spaces();
	~Spaces();
	
	static void finalise();

	typedef std::map<SPACE_ID, KBEShared_ptr<Space> > SPACES;

	/** 
		创建一个新的space 
	*/
	static Space* createNewSpace(SPACE_ID spaceID, const std::string& scriptModuleName);
	
	/**
		销毁一个space
	*/
	static bool destroySpace(SPACE_ID spaceID, ENTITY_ID entityID);

	/** 
		寻找一个指定space 
	 */
	static Space* findSpace(SPACE_ID spaceID);

	// 返回当前 CellApp 的空间拥有者实体集合，兼容普通 Entity 创建空间的场景。
	// Returns space-owner entities in this CellApp, including spaces created by regular Entity types.
	static PyObject* __py_Spaces(PyObject* self, PyObject* args);

	// 返回指定空间中的实体集合，键为实体 ID。
	// Returns entities in a space, keyed by entity ID.
	static PyObject* __py_EntitiesForSpace(PyObject* self, PyObject* args);
	
	/** 
		更新所有的space 
	*/
	static void update();

	static size_t size(){ return spaces().size(); }
	static uint64 snapshotSpaceCount();
	static uint64 snapshotTotalEntities();
	static uint64 snapshotTotalWitnesses();
	static uint64 snapshotTotalPendingWitnesses();
	static uint64 snapshotTotalAoiRelations();
	static uint64 snapshotMaxEntities();
	static uint64 snapshotMaxWitnesses();
	static uint64 snapshotMaxPendingWitnesses();
	static uint64 snapshotMaxAoiRelations();
	static uint64 snapshotMaxEntitiesSpaceID();
	static uint64 snapshotMaxWitnessesSpaceID();
	static uint64 snapshotMaxPendingWitnessesSpaceID();
	static uint64 snapshotMaxAoiRelationsSpaceID();

protected:
	static SPACES& spaces();
	static const SpaceLoadSnapshot& loadSnapshot();
};

}
#endif
