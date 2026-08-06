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

#include "spaces.h"
#include "profile.h"
#include "cellapp.h"
#include "entity.h"
#include "witness.h"
#include "common/performance_probes.h"
namespace KBEngine{	

Spaces::SPACES& Spaces::spaces()
{
	// The registry is explicitly cleared by finalise while runtime dependencies are valid.
	// Keeping its storage process-lived prevents CRT static destruction from releasing delayed Space owners after Cellapp, Components, or network pools are gone.
	// 注册表会在运行期依赖仍有效时由 finalise 显式清理。
	// 让存储与进程同寿命，可以避免 CRT 静态析构在 Cellapp、Components 或网络对象池销毁后才释放延迟的 Space 所有权。
	static SPACES* pSpaces = new SPACES();
	return *pSpaces;
}

//-------------------------------------------------------------------------------------
Spaces::Spaces()
{
}

//-------------------------------------------------------------------------------------
Spaces::~Spaces()
{
}

//-------------------------------------------------------------------------------------
void Spaces::finalise()
{
	SPACES& registeredSpaces = Spaces::spaces();
	Spaces::SPACES spaces = registeredSpaces;
	while (spaces.size() > 0)
	{
		SPACES::iterator iter = spaces.begin();
		KBEShared_ptr<Space> pSpace = iter->second;
		spaces.erase(iter++);
		pSpace->destroy(0, false);
		pSpace->finalise();
	}

	registeredSpaces.clear();
}

//-------------------------------------------------------------------------------------
Space* Spaces::createNewSpace(SPACE_ID spaceID, const std::string& scriptModuleName)
{
	SPACES& registeredSpaces = Spaces::spaces();
	SPACES::iterator iter = registeredSpaces.find(spaceID);
	if(iter != registeredSpaces.end())
	{
		ERROR_MSG(fmt::format("Spaces::createNewSpace: space {} is exist! scriptModuleName={}\n", spaceID, scriptModuleName));
		return NULL;
	}
	
	Space* space = new Space(spaceID, scriptModuleName);
	registeredSpaces[spaceID].reset(space);
	
	DEBUG_MSG(fmt::format("Spaces::createNewSpace: new space({}) {}.\n", scriptModuleName, spaceID));
	return space;
}

//-------------------------------------------------------------------------------------
bool Spaces::destroySpace(SPACE_ID spaceID, ENTITY_ID entityID)
{
	INFO_MSG(fmt::format("Spaces::destroySpace: {}.\n", spaceID));

	Space* pSpace = Spaces::findSpace(spaceID);
	if(!pSpace)
		return true;
	
	if(pSpace->isDestroyed())
		return true;

	if(!pSpace->destroy(entityID))
	{
		//WARNING_MSG("Spaces::destroySpace: destroying!\n");
		return false;
	}

	// 延时一段时间再销毁
	//spaces_.erase(spaceID);
	return true;
}

//-------------------------------------------------------------------------------------
Space* Spaces::findSpace(SPACE_ID spaceID)
{
	SPACES& registeredSpaces = Spaces::spaces();
	SPACES::iterator iter = registeredSpaces.find(spaceID);
	if(iter != registeredSpaces.end())
		return iter->second.get();
	
	return NULL;
}

//-------------------------------------------------------------------------------------
PyObject* Spaces::__py_Spaces(PyObject* /*self*/, PyObject* args)
{
	if(PyTuple_Size(args) != 0)
	{
		PyErr_SetString(PyExc_TypeError, "KBEngine.spaces: expected no arguments.");
		return NULL;
	}

	PyObject* result = PyDict_New();
	Entities<Entity>::ENTITYS_MAP& entities = Cellapp::getSingleton().pEntities()->getEntities();
	for(Entities<Entity>::ENTITYS_MAP::iterator iter = entities.begin(); iter != entities.end(); ++iter)
	{
		Entity* entity = static_cast<Entity*>(iter->second.get());
		if(entity == NULL || entity->isDestroyed() || !entity->isSpace())
			continue;

		PyObject* key = PyLong_FromUnsignedLong(entity->spaceID());
		PyDict_SetItem(result, key, entity);
		Py_DECREF(key);
	}

	return result;
}

//-------------------------------------------------------------------------------------
PyObject* Spaces::__py_EntitiesForSpace(PyObject* /*self*/, PyObject* args)
{
	SPACE_ID spaceID = 0;
	if(PyTuple_Size(args) != 1)
	{
		PyErr_SetString(PyExc_TypeError, "KBEngine.entitiesForSpace: expected one space ID.");
		return NULL;
	}

	if(!PyArg_ParseTuple(args, "I", &spaceID))
		return NULL;

	if(Spaces::findSpace(spaceID) == NULL)
	{
		PyErr_Format(PyExc_AssertionError, "KBEngine.entitiesForSpace: spaceID %u not found.", spaceID);
		return NULL;
	}

	PyObject* result = PyDict_New();
	Entities<Entity>::ENTITYS_MAP& entities = Cellapp::getSingleton().pEntities()->getEntities();
	for(Entities<Entity>::ENTITYS_MAP::iterator iter = entities.begin(); iter != entities.end(); ++iter)
	{
		Entity* entity = static_cast<Entity*>(iter->second.get());
		if(entity == NULL || entity->isDestroyed() || entity->spaceID() != spaceID)
			continue;

		PyObject* key = PyLong_FromLong(entity->id());
		PyDict_SetItem(result, key, entity);
		Py_DECREF(key);
	}

	return result;
}

//-------------------------------------------------------------------------------------
void Spaces::update()
{
	SCOPED_PROFILE(SPACES_UPDATE_PROFILE);
	SPACES& registeredSpaces = Spaces::spaces();
	SPACES::iterator iter = registeredSpaces.begin();

	for(; iter != registeredSpaces.end(); )
	{
		if(!iter->second->update())
		{
			// Notify CellAppMgr before releasing the map owner; a delayed destructor may run after global services are gone.
			// 在释放 map 所有权前通知 CellAppMgr；延迟析构可能发生在全局服务已经销毁之后。
			iter->second->finalise(true);
			registeredSpaces.erase(iter++);
		}
		else
		{
			++iter;
		}
	}
}

//-------------------------------------------------------------------------------------
const SpaceLoadSnapshot& Spaces::loadSnapshot()
{
	static const SpaceLoadSnapshot emptySnapshot;
	static SpaceLoadSnapshot snapshot;
	static GAME_TIME sampledTick = 0;
	static bool hasSnapshot = false;

	// Watcher sampling is intentionally pull-based and disabled with the other
	// performance probes. All spaces are scanned at most once per game Tick.
	// Watcher 采样按需触发，并随性能探针一同关闭；同一游戏 Tick 最多扫描一次全部 Space。
	if (!g_performanceProbesEnabled)
		return emptySnapshot;

	if (hasSnapshot && sampledTick == g_kbetime)
		return snapshot;

	snapshot = SpaceLoadSnapshot();
	snapshot.sampledTick = g_kbetime;

	const SPACES& registeredSpaces = spaces();
	for (SPACES::const_iterator spaceIter = registeredSpaces.begin();
		spaceIter != registeredSpaces.end(); ++spaceIter)
	{
		const Space* pSpace = spaceIter->second.get();
		if (pSpace == NULL)
			continue;

		const SPACE_ENTITIES& spaceEntities = pSpace->entities();
		uint64 witnesses = 0;
		uint64 pendingWitnesses = 0;
		uint64 aoiRelations = 0;

		for (SPACE_ENTITIES::const_iterator entityIter = spaceEntities.begin();
			entityIter != spaceEntities.end(); ++entityIter)
		{
			Entity* pEntity = entityIter->get();
			if (pEntity == NULL || pEntity->isDestroyed() || !pEntity->hasWitness())
				continue;

			Witness* pWitness = pEntity->pWitness();
			if (pWitness == NULL)
				continue;

			++witnesses;
			if (pWitness->schedulerPending())
				++pendingWitnesses;
			aoiRelations += static_cast<uint64>(pWitness->viewEntities().size());
		}

		snapshot.observe(pSpace->id(), static_cast<uint64>(spaceEntities.size()),
			witnesses, pendingWitnesses, aoiRelations);
	}

	sampledTick = g_kbetime;
	hasSnapshot = true;
	return snapshot;
}

//-------------------------------------------------------------------------------------
uint64 Spaces::snapshotSpaceCount() { return loadSnapshot().spaceCount; }
uint64 Spaces::snapshotTotalEntities() { return loadSnapshot().totalEntities; }
uint64 Spaces::snapshotTotalWitnesses() { return loadSnapshot().totalWitnesses; }
uint64 Spaces::snapshotTotalPendingWitnesses() { return loadSnapshot().totalPendingWitnesses; }
uint64 Spaces::snapshotTotalAoiRelations() { return loadSnapshot().totalAoiRelations; }
uint64 Spaces::snapshotMaxEntities() { return loadSnapshot().maxEntities; }
uint64 Spaces::snapshotMaxWitnesses() { return loadSnapshot().maxWitnesses; }
uint64 Spaces::snapshotMaxPendingWitnesses() { return loadSnapshot().maxPendingWitnesses; }
uint64 Spaces::snapshotMaxAoiRelations() { return loadSnapshot().maxAoiRelations; }
uint64 Spaces::snapshotMaxEntitiesSpaceID() { return loadSnapshot().maxEntitiesSpaceID; }
uint64 Spaces::snapshotMaxWitnessesSpaceID() { return loadSnapshot().maxWitnessesSpaceID; }
uint64 Spaces::snapshotMaxPendingWitnessesSpaceID() { return loadSnapshot().maxPendingWitnessesSpaceID; }
uint64 Spaces::snapshotMaxAoiRelationsSpaceID() { return loadSnapshot().maxAoiRelationsSpaceID; }

//-------------------------------------------------------------------------------------
}
