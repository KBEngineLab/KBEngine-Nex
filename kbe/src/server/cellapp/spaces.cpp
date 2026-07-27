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
#include "cellapp.h"
#include "entity.h"
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
}
