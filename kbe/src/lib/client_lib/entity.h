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

#ifndef KBE_CLIENTAPP_ENTITY_H
#define KBE_CLIENTAPP_ENTITY_H

#include "entity_aspect.h"
#include "client_lib/profile.h"
#include "common/timer.h"
#include "common/common.h"
#include "helper/debug_helper.h"
#include "entitydef/entity_call.h"
#include "pyscript/math.h"
#include "pyscript/scriptobject.h"
#include "entitydef/datatypes.h"	
#include "entitydef/entitydef.h"	
#include "entitydef/scriptdef_module.h"
#include "entitydef/entity_macro.h"	
#include "server/script_timers.h"	
	
namespace KBEngine{
class EntityCall;
class ClientObjectBase;

namespace Network
{
class Channel;
}

namespace client
{

class Entity : public script::ScriptObject
{
	/** ���໯ ��һЩpy�������������� */
	BASE_SCRIPT_HREADER(Entity, ScriptObject)	
	ENTITY_HEADER(Entity)
		
public:
	Entity(ENTITY_ID id, const ScriptDefModule* pScriptModule, EntityCall* base, EntityCall* cell);
	~Entity();
	
	/** 
		�����������ݱ��ı��� 
	*/
	void onDefDataChanged(const PropertyDescription* propertyDescription, 
			PyObject* pyData);
	
	/** 
		entitycall section
	*/
	INLINE EntityCall* baseEntityCall() const;
	DECLARE_PY_GET_MOTHOD(pyGetBaseEntityCall);
	INLINE void baseEntityCall(EntityCall* entitycall);
	
	INLINE EntityCall* cellEntityCall() const;
	DECLARE_PY_GET_MOTHOD(pyGetCellEntityCall);
	INLINE void cellEntityCall(EntityCall* entitycall);

	/** 
		�ű���ȡ������entity��position 
	*/
	INLINE Position3D& position();
	INLINE Position3D& serverPosition();
	INLINE void position(const Position3D& pos);
	INLINE void serverPosition(const Position3D& pos);
	void onPositionChanged();
	DECLARE_PY_GETSET_MOTHOD(pyGetPosition, pySetPosition);

	/** 
		�ű���ȡ������entity�ķ��� 
	*/
	INLINE Direction3D& direction();
	INLINE void direction(const Direction3D& dir);
	void onDirectionChanged();
	DECLARE_PY_GETSET_MOTHOD(pyGetDirection, pySetDirection);
	
	/**
		ʵ��ͻ��˵�λ�úͳ���
	*/
	INLINE Position3D& clientPos();
	INLINE void clientPos(const Position3D& pos);
	INLINE void clientPos(float x, float y, float z);

	INLINE Direction3D& clientDir();
	INLINE void clientDir(const Direction3D& dir);
	INLINE void clientDir(float roll, float pitch, float yaw);

	/**
		�ƶ��ٶ�
	*/
	INLINE void moveSpeed(float speed);
	INLINE float moveSpeed() const;
	void onMoveSpeedChanged();
	DECLARE_PY_GETSET_MOTHOD(pyGetMoveSpeed, pySetMoveSpeed);

	/** 
		pClientApp section
	*/
	DECLARE_PY_GET_MOTHOD(pyGetClientApp);
	void pClientApp(ClientObjectBase* p);
	INLINE ClientObjectBase* pClientApp() const;
	
	const EntityAspect* getAspect() const{ return &aspect_; }

	/** 
		entity�ƶ���ĳ���� 
	*/
	uint32 moveToPoint(const Position3D& destination, float velocity, float distance,
			PyObject* userData, bool faceMovement, bool moveVertically);
	
	DECLARE_PY_MOTHOD_ARG6(pyMoveToPoint, PyObject_ptr, float, float, PyObject_ptr, int32, int32);

	/** 
		ֹͣ�κ��ƶ���Ϊ
	*/
	bool stopMove();

	/** 
		entity��һ���ƶ���� 
	*/
	void onMove(uint32 controllerId, int layer, const Position3D& oldPos, PyObject* userarg);

	/** 
		entity���ƶ���� 
	*/
	void onMoveOver(uint32 controllerId, int layer, const Position3D& oldPos, PyObject* userarg);

	/** 
		entity�ƶ�ʧ��
	*/
	void onMoveFailure(uint32 controllerId, PyObject* userarg);

	/** 
		ɾ��һ��������  
	*/
	void cancelController(uint32 id);
	static PyObject* __py_pyCancelController(PyObject* self, PyObject* args);

	/** 
		�������entity 
	*/
	void onDestroy(bool callScript){};

	void onEnterWorld();
	void onLeaveWorld();

	void onEnterSpace();
	void onLeaveSpace();

	/**
		Զ�̺��б�entity�ķ��� 
	*/
	void onRemoteMethodCall(Network::Channel* pChannel, MemoryStream& s);

	/**
		����������entity����
	*/
	void onUpdatePropertys(MemoryStream& s);
	
	/**
	    ����Entity�����ݵ�һ������ʱ�������Ƿ�Ҫ�ص��ű����set_*����
	*/
	void callPropertysSetMethods();

	bool inWorld() const{ return enterworld_; }

	void onBecomePlayer();
	void onBecomeNonPlayer();
	
	bool isOnGround() const { return isOnGround_;}
	void isOnGround(bool v) { isOnGround_ = v;}

	INLINE bool isInited();
	INLINE void isInited(bool status);

    bool isControlled() { return isControlled_; }
    void onControlled(bool p_controlled);

	bool isPlayer();
	DECLARE_PY_MOTHOD_ARG0(pyIsPlayer);

protected:
	EntityCall*								cellEntityCall_;					// ���entity��cell-entitycall
	EntityCall*								baseEntityCall_;					// ���entity��base-entitycall

	Position3D								position_, serverPosition_;			// entity�ĵ�ǰλ��
	Direction3D								direction_;							// entity�ĵ�ǰ����

	Position3D								clientPos_;							// �ͻ���λ�ã����ʵ�屻�ͻ��˿��������������ͬ��λ��
	Direction3D								clientDir_;							// �ͻ��˳������ʵ�屻�ͻ��˿��������������ͬ������

	ClientObjectBase*						pClientApp_;

	EntityAspect							aspect_;

	float									velocity_;

	bool									enterworld_;						// �Ƿ��Ѿ�enterworld�ˣ� restoreʱ����
	
	bool									isOnGround_;

	ScriptID								pMoveHandlerID_;
	
	bool									inited_;							// __init__����֮������Ϊtrue

    bool                                    isControlled_;                      // �Ƿ񱻿���
};																										

}
}

#ifdef CODE_INLINE
#include "entity.inl"
#endif
#endif
