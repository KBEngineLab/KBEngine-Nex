// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

#include "spacememorys.h"
#include "cellapp.h"

namespace KBEngine{
SpaceMemorys::SPACEMEMORYS SpaceMemorys::spaces_;

//-------------------------------------------------------------------------------------
SpaceMemorys::SpaceMemorys()
{
}

//-------------------------------------------------------------------------------------
SpaceMemorys::~SpaceMemorys()
{
}

//-------------------------------------------------------------------------------------
void SpaceMemorys::finalise()
{
	SpaceMemorys::SPACEMEMORYS spaces = spaces_;
	while (spaces.size() > 0)
	{
		SPACEMEMORYS::iterator iter = spaces.begin();
		KBEShared_ptr<SpaceMemory> pSpace = iter->second;
		spaces.erase(iter++);
		pSpace->destroy(0, false);
	}

	spaces_.clear();
}

//-------------------------------------------------------------------------------------
SpaceMemory* SpaceMemorys::createNewSpace(SPACE_ID spaceID, const std::string& scriptModuleName)
{
	SPACEMEMORYS::iterator iter = spaces_.find(spaceID);
	if(iter != spaces_.end())
	{
		ERROR_MSG(fmt::format("Spaces::createNewSpace: space {} is exist! scriptModuleName={}\n", spaceID, scriptModuleName));
		return NULL;
	}
	
	SpaceMemory* space = new SpaceMemory(spaceID, scriptModuleName);
	spaces_[spaceID].reset(space);
	
	DEBUG_MSG(fmt::format("Spaces::createNewSpace: new space({}) {}.\n", scriptModuleName, spaceID));
	return space;
}

//-------------------------------------------------------------------------------------
bool SpaceMemorys::destroySpace(SPACE_ID spaceID, ENTITY_ID entityID)
{
	INFO_MSG(fmt::format("Spaces::destroySpace: {}.\n", spaceID));

	SpaceMemory* pSpace = SpaceMemorys::findSpace(spaceID);
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
SpaceMemory* SpaceMemorys::findSpace(SPACE_ID spaceID)
{
	SPACEMEMORYS::iterator iter = spaces_.find(spaceID);
	if(iter != spaces_.end())
		return iter->second.get();
	
	return NULL;
}

//-------------------------------------------------------------------------------------
PyObject* SpaceMemorys::__py_Spaces(PyObject* self, PyObject* args)
{
	if(PyTuple_Size(args) != 0)
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::spaces: args error! argsSize != 0.");
		PyErr_PrintEx(0);
		return NULL;
	}

	PyObject* pySpaces = PyDict_New();
	Entities<Entity>* pEntities = Cellapp::getSingleton().pEntities();

	Entities<Entity>::ENTITYS_MAP& entities = pEntities->getEntities();
	Entities<Entity>::ENTITYS_MAP::iterator iter = entities.begin();
	for(; iter != entities.end(); ++iter)
	{
		Entity* pEntity = static_cast<Entity*>(iter->second.get());
		if(pEntity == NULL || pEntity->isDestroyed())
			continue;

		if(!pEntity->isSpace())
			continue;

		PyObject* pySpaceID = PyLong_FromUnsignedLong(pEntity->spaceID());
		PyDict_SetItem(pySpaces, pySpaceID, pEntity);
		Py_DECREF(pySpaceID);
	}

	return pySpaces;
}

//-------------------------------------------------------------------------------------
PyObject* SpaceMemorys::__py_EntitiesForSpace(PyObject* self, PyObject* args)
{
	SPACE_ID spaceID = 0;

	if(PyTuple_Size(args) != 1)
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::entitiesForSpace: args error! argsSize != 1.");
		PyErr_PrintEx(0);
		return NULL;
	}

	if(!PyArg_ParseTuple(args, "I", &spaceID))
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::entitiesForSpace: args error!");
		PyErr_PrintEx(0);
		return NULL;
	}

	if(SpaceMemorys::findSpace(spaceID) == NULL)
	{
		PyErr_Format(PyExc_AssertionError, "KBEngine::entitiesForSpace: spaceID %u not found.", spaceID);
		PyErr_PrintEx(0);
		return NULL;
	}

	PyObject* pyEntities = PyDict_New();
	Entities<Entity>* pEntities = Cellapp::getSingleton().pEntities();

	Entities<Entity>::ENTITYS_MAP& entities = pEntities->getEntities();
	Entities<Entity>::ENTITYS_MAP::iterator iter = entities.begin();
	for(; iter != entities.end(); ++iter)
	{
		Entity* pEntity = static_cast<Entity*>(iter->second.get());
		if(pEntity == NULL || pEntity->isDestroyed())
			continue;

		if(pEntity->spaceID() != spaceID)
			continue;

		PyObject* pyEntityID = PyLong_FromLong(pEntity->id());
		PyDict_SetItem(pyEntities, pyEntityID, pEntity);
		Py_DECREF(pyEntityID);
	}

	return pyEntities;
}

//-------------------------------------------------------------------------------------
void SpaceMemorys::update()
{
	SPACEMEMORYS::iterator iter = spaces_.begin();

	for(; iter != spaces_.end(); )
	{
		if(!iter->second->update())
		{
			spaces_.erase(iter++);
		}
		else
		{
			++iter;
		}
	}
}

//-------------------------------------------------------------------------------------
}
