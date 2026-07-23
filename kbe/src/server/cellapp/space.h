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

#ifndef KBE_SPACE_H
#define KBE_SPACE_H

#include "coordinate_system.h"
#include "cell.h"
#include "helper/debug_helper.h"
#include "common/common.h"
#include "common/smartpointer.h"
#include "pyscript/scriptobject.h"
#include "navigation/navigation_handle.h"

namespace KBEngine{

class Entity;
typedef SmartPointer<Entity> EntityPtr;
typedef std::vector<EntityPtr> SPACE_ENTITIES;

class Space
{
public:
	Space(SPACE_ID spaceID, const std::string& scriptModuleName);
	~Space();

	void unLoadSpaceGeometry();
	void loadSpaceGeometry(const std::map< int, std::string >& params);

	/** 
		����space�е����� 
	*/
	bool update();

	void addEntity(Entity* pEntity);
	void addEntityToNode(Entity* pEntity);

	void addEntityAndEnterWorld(Entity* pEntity, bool isRestore = false);
	void removeEntity(Entity* pEntity);

	/**
		һ��entity��������Ϸ����
	*/
	void onEnterWorld(Entity* pEntity);
	void _onEnterWorld(Entity* pEntity);
	void onLeaveWorld(Entity* pEntity);

	void onEntityAttachWitness(Entity* pEntity);

	SPACE_ID id() const{ return id_; }

	const SPACE_ENTITIES& entities() const{ return entities_; }
	Entity* findEntity(ENTITY_ID entityID);

	/**
		����
	*/
	bool destroy(ENTITY_ID entityID, bool ignoreGhost = true);

	/**
		���space��cell
	*/
	Cell * pCell() const	{ return pCell_; }
	void pCell( Cell * pCell );

	/**
		����space�ļ���ӳ��
	*/
	static PyObject* __py_AddSpaceGeometryMapping(PyObject* self, PyObject* args);
	bool addSpaceGeometryMapping(std::string respath, bool shouldLoadOnServer, const std::map< int, std::string >& params);
	static PyObject* __py_GetSpaceGeometryMapping(PyObject* self, PyObject* args);
	const std::string& getGeometryPath();
	void setGeometryPath(const std::string& path);
	void onLoadedSpaceGeometryMapping(NavigationHandlePtr pNavHandle);
	void onAllSpaceGeometryLoaded();
	
	NavigationHandlePtr pNavHandle() const{ return pNavHandle_; }

	/**
		spaceData��ز����ӿ�
	*/
	void setSpaceData(const std::string& key, const std::string& value);
	void delSpaceData(const std::string& key);
	bool hasSpaceData(const std::string& key);
	const std::string& getSpaceData(const std::string& key);
	void onSpaceDataChanged(const std::string& key, const std::string& value, bool isdel);
	static PyObject* __py_SetSpaceData(PyObject* self, PyObject* args);
	static PyObject* __py_GetSpaceData(PyObject* self, PyObject* args);
	static PyObject* __py_DelSpaceData(PyObject* self, PyObject* args);

	CoordinateSystem* pCoordinateSystem(){ return &coordinateSystem_; }

	bool isDestroyed() const{ return state_ == STATE_DESTROYED; }
	bool isGood() const{ return state_ == STATE_NORMAL; }

protected:
	void _addSpaceDatasToEntityClient(const Entity* pEntity);

	void _clearGhosts();
	
	enum STATE
	{
		STATE_NORMAL = 0,
		STATE_DESTROYING = 1,
		STATE_DESTROYED = 2
	};

protected:
	// ���space��ID
	SPACE_ID					id_;														

	// �������spaceʱ�õ�ʵ��ű�ģ������
	std::string					scriptModuleName_;

	// ���space�ϵ�entity
	SPACE_ENTITIES				entities_;							

	// �Ƿ���ع���������
	bool						hasGeometry_;

	// ÿ��space���ֻ��һ��cell
	Cell*						pCell_;

	CoordinateSystem			coordinateSystem_;

	NavigationHandlePtr			pNavHandle_;

	// spaceData, ֻ�ܴ洢�ַ�����Դ�� �����ܱȽϺõļ��ݿͻ��ˡ�
	// �����߿��Խ���������ת�����ַ������д���
	SPACE_DATA					datas_;

	int8						state_;
	
	uint64						destroyTime_;	
};


}
#endif
