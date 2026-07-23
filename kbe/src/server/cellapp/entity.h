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

#ifndef KBE_ENTITY_H
#define KBE_ENTITY_H
	
// common include
//#include "entitymovecontroller.h"
#include "profile.h"
#include "common/timer.h"
#include "common/common.h"
#include "common/smartpointer.h"
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

class Chunk;
class Entity;
class EntityCall;
class Cellapp;
class Witness;
class AllClients;
class CoordinateSystem;
class EntityCoordinateNode;
class Controller;
class Controllers;
class Space;
class VolatileInfo;

namespace Network
{
class Channel;
class Bundle;
}

typedef SmartPointer<Entity> EntityPtr;
typedef std::vector<EntityPtr> SPACE_ENTITIES;

class Entity : public script::ScriptObject
{
	/** ���໯ ��һЩpy�������������� */
	BASE_SCRIPT_HREADER(Entity, ScriptObject)	
	ENTITY_HEADER(Entity)

public:
	Entity(ENTITY_ID id, const ScriptDefModule* pScriptModule,
		PyTypeObject* pyType = getScriptType(), bool isInitialised = true);
	~Entity();
	
	/** 
		�������entity 
	*/
	void onDestroy(bool callScript);
	
	/**
		���ٳ���
	*/
	DECLARE_PY_MOTHOD_ARG0(pyDestroySpace);
	void destroySpace(void);

	/** 
		��ǰʵ�����ڵ�space��Ҫ����ʱ����  
	*/
	void onSpaceGone();
	
	/** 
		�ж������Ƿ���һ��realEntity 
	*/
	INLINE bool isReal(void) const;

	/** 
		�ж������Ƿ���ghostEntity 
	*/
	INLINE bool hasGhost(void) const;

	/** 
		�ж������Ƿ���һ��realEntity 
	*/
	INLINE COMPONENT_ID realCell(void) const;
	INLINE void realCell(COMPONENT_ID cellID);

	/** 
		�ж������Ƿ���ghostEntity 
	*/
	INLINE COMPONENT_ID ghostCell(void) const;
	INLINE void ghostCell(COMPONENT_ID cellID);

	/** 
		�����������ݱ��ı��� 
	*/
	void onDefDataChanged(const PropertyDescription* propertyDescription, 
			PyObject* pyData);
	
	/** 
		��entityͨ��ͨ��
	*/
	INLINE void pChannel(Network::Channel* pchannel);
	INLINE Network::Channel* pChannel(void) const ;

public:
	/** 
		entityCall section
	*/
	INLINE EntityCall* baseEntityCall() const;
	DECLARE_PY_GET_MOTHOD(pyGetBaseEntityCall);
	INLINE void baseEntityCall(EntityCall* entityCall);
	
	INLINE EntityCall* clientEntityCall() const;
	DECLARE_PY_GET_MOTHOD(pyGetClientEntityCall);
	INLINE void clientEntityCall(EntityCall* entityCall);

	/**
		all_clients
	*/
	INLINE AllClients* allClients() const;
	DECLARE_PY_GET_MOTHOD(pyGetAllClients);
	INLINE void allClients(AllClients* clients);

	/**
		other_clients
	*/
	INLINE AllClients* otherClients() const;
	DECLARE_PY_GET_MOTHOD(pyGetOtherClients);
	INLINE void otherClients(AllClients* clients);

	/**
		�ű���ȡcontrolledBy����
	*/
	INLINE bool isControlledNotSelfClient() const;
	INLINE EntityCall* controlledBy() const;
	INLINE void controlledBy(EntityCall* baseEntityCall);
	DECLARE_PY_GETSET_MOTHOD(pyGetControlledBy, pySetControlledBy);
	bool setControlledBy(EntityCall* baseEntityCall);
	void sendControlledByStatusMessage(EntityCall* baseEntityCall, int8 isControlled);

	/** 
		�ű���ȡ������entity��position 
	*/
	INLINE Position3D& position();
	INLINE void position(const Position3D& pos);
	DECLARE_PY_GETSET_MOTHOD(pyGetPosition, pySetPosition);

	/** 
		�ű���ȡ������entity�ķ��� 
	*/
	INLINE Direction3D& direction();
	INLINE void direction(const Direction3D& dir);
	DECLARE_PY_GETSET_MOTHOD(pyGetDirection, pySetDirection);

	/**
		�Ƿ��ڵ�����
	*/
	INLINE void isOnGround(bool v);
	INLINE bool isOnGround() const;
	DECLARE_PY_GET_MOTHOD(pyGetIsOnGround);

	/** 
		����entity�����λ�� 
	*/
	void setPositionAndDirection(const Position3D& pos, 
		const Direction3D& dir);
	
	void onPositionChanged();
	void onDirectionChanged();
	
	void onPyPositionChanged();
	void onPyDirectionChanged();
	
	void updateLastPos();

	bool checkMoveForTopSpeed(const Position3D& position);

	/** ����ӿ�
		�ͻ���������λ��
	*/
	void setPosition_XZ_int(Network::Channel* pChannel, int32 x, int32 z);

	/** ����ӿ�
		�ͻ���������λ��
	*/
	void setPosition_XYZ_int(Network::Channel* pChannel, int32 x, int32 y, int32 z);

	/** ����ӿ�
		�ͻ�������λ��
	*/
	void setPosition_XZ_float(Network::Channel* pChannel, float x, float z);

	/** ����ӿ�
		�ͻ�������λ��
	*/
	void setPosition_XYZ_float(Network::Channel* pChannel, float x, float y, float z);

	/**
		cell�ϵĴ��ͷ���
	*/
	DECLARE_PY_MOTHOD_ARG3(pyTeleport, PyObject_ptr, PyObject_ptr, PyObject_ptr);
	void teleport(PyObject_ptr nearbyMBRef, Position3D& pos, Direction3D& dir);
	void teleportLocal(PyObject_ptr nearbyMBRef, Position3D& pos, Direction3D& dir);
	void teleportRefEntity(Entity* entity, Position3D& pos, Direction3D& dir);
	void teleportRefEntityCall(EntityCall* nearbyMBRef, Position3D& pos, Direction3D& dir);
	void onTeleportRefEntityCall(EntityCall* nearbyMBRef, Position3D& pos, Direction3D& dir);

	/**
		���ͳɹ���ʧ����ػص�
	*/
	void onTeleport();
	void onTeleportFailure();
	void onTeleportSuccess(PyObject* nearbyEntity, SPACE_ID lastSpaceID);
	void onReqTeleportOtherAck(Network::Channel* pChannel, ENTITY_ID nearbyMBRefID, 
		SPACE_ID destSpaceID, COMPONENT_ID componentID);

	/**
		�����뿪cell�Ȼص�
	*/
	void onEnteredCell();
	void onEnteringCell();
	void onLeavingCell();
	void onLeftCell();
	
	/**
		�����뿪space�Ȼص�
	*/
	void onEnterSpace(Space* pSpace);
	void onLeaveSpace(Space* pSpace);

	/** 
		��cellapp������ֹ�� baseapp������ҵ����ʵ�cellapp����ָ���
		����ô˷���
	*/
	void onRestore();

	/**
		�ű�����view
	*/
	void debugView();
	DECLARE_PY_MOTHOD_ARG0(pyDebugView);

	/** 
		��ǰentity����������View�뾶��Χ 
	*/
	int32 setViewRadius(float radius, float hyst);
	float getViewRadius(void) const;
	float getViewHystArea(void) const;
	DECLARE_PY_MOTHOD_ARG2(pySetViewRadius, float, float);
	DECLARE_PY_MOTHOD_ARG0(pyGetViewRadius);
	DECLARE_PY_MOTHOD_ARG0(pyGetViewHystArea);

	/**
		���ع۲��ʵ������й۲���
	*/
	DECLARE_PY_MOTHOD_ARG0(pyGetWitnesses);

	/** 
		��ǰentity�Ƿ�Ϊreal 
	*/
	DECLARE_PY_MOTHOD_ARG0(pyIsReal);
	
	/** 
		��baseapp���ͱ�������
	*/
	void backupCellData();

	/** 
		��Ҫ���浽���ݿ�֮ǰ��֪ͨ 
	*/
	void onWriteToDB();

	/** 
		�ű���ȡ������entity��position 
	*/
	INLINE int8 layer() const;
	DECLARE_PY_GETSET_MOTHOD(pyGetLayer, pySetLayer);

	/** 
		entity�ƶ����� 
	*/
	bool canNavigate();
	uint32 navigate(const Position3D& destination, float velocity, float distance,
					float maxMoveDistance, float maxSearchDistance,
					bool faceMovement, int8 layer, PyObject* userData);
	bool navigatePathPoints(std::vector<Position3D>& outPaths, const Position3D& destination, float maxSearchDistance, int8 layer);

	DECLARE_PY_MOTHOD_ARG0(pycanNavigate);
	DECLARE_PY_MOTHOD_ARG3(pyNavigatePathPoints, PyObject_ptr, float, int8);
	DECLARE_PY_MOTHOD_ARG8(pyNavigate, PyObject_ptr, float, float, float, float, int8, int8, PyObject_ptr);

	/** 
		entity�������� 
	*/
	bool getRandomPoints(std::vector<Position3D>& outPoints, const Position3D& centerPos, float maxRadius, uint32 maxPoints, int8 layer);
	DECLARE_PY_MOTHOD_ARG4(pyGetRandomPoints, PyObject_ptr, float, uint32, int8);

	/** 
		entity�ƶ���ĳ���� 
	*/
	uint32 moveToPoint(const Position3D& destination, float velocity, float distance,
			PyObject* userData, bool faceMovement, bool moveVertically);
	
	DECLARE_PY_MOTHOD_ARG6(pyMoveToPoint, PyObject_ptr, float, float, PyObject_ptr, int32, int32);

	/** 
		entity�ƶ���ĳ��entity 
	*/
	uint32 moveToEntity(ENTITY_ID targetID, float velocity, float distance,
			PyObject* userData, bool faceMovement, bool moveVertically, const Position3D& offsetPos);
	
	static PyObject* __py_pyMoveToEntity(PyObject* self, PyObject* args);

	/**
	entity�ƶ�����
	*/
	float accelerate(const char* type, float acceleration);
	DECLARE_PY_MOTHOD_ARG2(pyAccelerate, const_charptr, float);

	/** 
		�ű���ȡ������entity�����xz�ƶ��ٶ� 
	*/
	float topSpeed() const{ return topSpeed_; }
	INLINE void topSpeed(float speed);
	DECLARE_PY_GETSET_MOTHOD(pyGetTopSpeed, pySetTopSpeed);
	
	/** 
		�ű���ȡ������entity�����y�ƶ��ٶ� 
	*/
	INLINE float topSpeedY() const;
	INLINE void topSpeedY(float speed);
	DECLARE_PY_GETSET_MOTHOD(pyGetTopSpeedY, pySetTopSpeedY);
	
	/** 
		�ű�������һ����Χ�ڵ�ĳ�����͵�entities 
	*/
	static PyObject* __py_pyEntitiesInRange(PyObject* self, PyObject* args);

	/** 
		�ű�������View��Χ�ڵ�entities 
	*/
	static PyObject* __py_pyEntitiesInView(PyObject* self, PyObject* args);
	PyObject* entitiesInView(bool pending);

	/**
		���û�ȡ�Ƿ��Զ�����
	*/
	INLINE int8 shouldAutoBackup() const;
	INLINE void shouldAutoBackup(int8 v);
	DECLARE_PY_GETSET_MOTHOD(pyGetShouldAutoBackup, pySetShouldAutoBackup);

	/** ����ӿ�
		Զ�̺��б�entity�ķ��� 
	*/
	void onRemoteMethodCall(Network::Channel* pChannel, MemoryStream& s);
	void onRemoteCallMethodFromClient(Network::Channel* pChannel, ENTITY_ID srcEntityID, MemoryStream& s);
	void onRemoteMethodCall_(MethodDescription* pMethodDescription, ENTITY_ID srcEntityID, MemoryStream& s);

	/**
		�۲���
	*/
	INLINE Witness* pWitness() const;
	INLINE void pWitness(Witness* w);

	/** 
		�Ƿ��κ�proxy���ӵ�, ������entityû�пͻ��ˣ� �����ֵ��Ч 
	*/
	INLINE bool isWitnessed(void) const;
	DECLARE_PY_GET_MOTHOD(pyIsWitnessed);

	/** 
		entity�Ƿ���һ���۲��� 
	*/
	INLINE bool hasWitness(void) const;
	DECLARE_PY_GET_MOTHOD(pyHasWitness);

	/** 
		������һ���۲��߹۲쵽�� 
	*/
	void addWitnessed(Entity* entity);

	/** 
		�Ƴ�һ���۲������Ĺ۲��� 
	*/
	void delWitnessed(Entity* entity);
	void onDelWitnessed();

	/**
		 ָ����entity�Ƿ��ǹ۲��Լ�����֮һ
	*/
	bool entityInWitnessed(ENTITY_ID entityID);

	INLINE const std::list<ENTITY_ID>&	witnesses();
	INLINE size_t witnessesSize() const;

	/** ����ӿ�
		entity����һ���۲���(�ͻ���)

	*/
	void setWitness(Witness* pWitness);
	void onGetWitnessFromBase(Network::Channel* pChannel);
	void onGetWitness(bool fromBase = false);

	/** ����ӿ�
		entity��ʧ��һ���۲���(�ͻ���)

	*/
	void onLoseWitness(Network::Channel* pChannel);

	/** 
		client��������
	*/
	void onUpdateDataFromClient(KBEngine::MemoryStream& s);

	/** 
		����һ����Χ������  
	*/
	uint32 addProximity(float range_xz, float range_y, int32 userarg);
	DECLARE_PY_MOTHOD_ARG3(pyAddProximity, float, float, int32);

	/** 
		���ÿͻ���ʵ��ķ���  
	*/
	DECLARE_PY_MOTHOD_ARG1(pyClientEntity, ENTITY_ID);

	/** 
		�ָ����еķ�Χ������ 
		��teleportʱ��������������
	*/
	void restoreProximitys();

	/** 
		ɾ��һ�������� 
	*/
	void cancelController(uint32 id);
	static PyObject* __py_pyCancelController(PyObject* self, PyObject* args);

	/** 
		һ��entity���������entity��ĳ����Χ������  
	*/
	void onEnterTrap(Entity* entity, float range_xz, float range_y, 
							uint32 controllerID, int32 userarg);

	/** 
		һ��entity�뿪�����entity��ĳ����Χ������  
	*/
	void onLeaveTrap(Entity* entity, float range_xz, float range_y, 
							uint32 controllerID, int32 userarg);

	/** 
		��entity����һ���µ�space��ȥ���뿪��Χ�������¼�����������ӿ� 
	*/
	void onLeaveTrapID(ENTITY_ID entityID, 
							float range_xz, float range_y, 
							uint32 controllerID, int32 userarg);

	/** 
		һ��entity������View����
	*/
	void onEnteredView(Entity* entity);

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
		entityת������
	*/
	uint32 addYawRotator(float yaw, float velocity,
		PyObject* userData);

	DECLARE_PY_MOTHOD_ARG3(pyAddYawRotator, float, float, PyObject_ptr);
	
	/**
		entityת�����
	*/
	void onTurn(uint32 controllerId, PyObject* userarg);
	
	/**
		��ȡ������space��entities�е�λ��
	*/
	INLINE SPACE_ENTITIES::size_type spaceEntityIdx() const;
	INLINE void spaceEntityIdx(SPACE_ENTITIES::size_type idx);

	/**
		��ȡentity���ڽڵ�
	*/
	INLINE EntityCoordinateNode* pEntityCoordinateNode() const;
	INLINE void pEntityCoordinateNode(EntityCoordinateNode* pNode);

	/**
		��װж�ؽڵ�
	*/
	void installCoordinateNodes(CoordinateSystem* pCoordinateSystem);
	void uninstallCoordinateNodes(CoordinateSystem* pCoordinateSystem);
	void onCoordinateNodesDestroy(EntityCoordinateNode* pEntityCoordinateNode);

	/**
		��ȡentityλ�ó�����ĳʱ���Ƿ�ı��
	*/
	INLINE GAME_TIME posChangedTime() const;
	INLINE GAME_TIME dirChangedTime() const;

	/** 
		real����������Ե�ghost
	*/
	void onUpdateGhostPropertys(KBEngine::MemoryStream& s);
	
	/** 
		ghost�������def����real
	*/
	void onRemoteRealMethodCall(KBEngine::MemoryStream& s);

	/** 
		real����������Ե�ghost
	*/
	void onUpdateGhostVolatileData(KBEngine::MemoryStream& s);

	/** 
		ת��Ϊghost, ��������Ϊreal
	*/
	void changeToGhost(COMPONENT_ID realCell, KBEngine::MemoryStream& s);

	/** 
		ת��Ϊreal, ��������Ϊghost
	*/
	void changeToReal(COMPONENT_ID ghostCell, KBEngine::MemoryStream& s);

	void addToStream(KBEngine::MemoryStream& s);
	void createFromStream(KBEngine::MemoryStream& s);

	void addTimersToStream(KBEngine::MemoryStream& s);
	void createTimersFromStream(KBEngine::MemoryStream& s);

	void addControllersToStream(KBEngine::MemoryStream& s);
	void createControllersFromStream(KBEngine::MemoryStream& s);

	void addWitnessToStream(KBEngine::MemoryStream& s);
	void createWitnessFromStream(KBEngine::MemoryStream& s);

	void addMovementHandlerToStream(KBEngine::MemoryStream& s);
	void createMovementHandlerFromStream(KBEngine::MemoryStream& s);
	
	/** 
		���ʵ�������������
	*/
	INLINE Controllers*	pControllers() const;

	/** 
		����ʵ��־û������Ƿ����࣬���˻��Զ��浵 
	*/
	INLINE void setDirty(uint32* digest = NULL);
	INLINE bool isDirty() const;
	
	/**
		VolatileInfo section
	*/
	INLINE VolatileInfo* pCustomVolatileinfo(void);
	DECLARE_PY_GETSET_MOTHOD(pyGetVolatileinfo, pySetVolatileinfo);

	/**
		����ʵ��Ļص��������п��ܱ�����
	*/
	bool bufferOrExeCallback(const char * funcName, PyObject * funcArgs, bool notFoundIsOK = true);
	static void bufferCallback(bool enable);

private:
	/** 
		����teleport�����base��
	*/
	void _sendBaseTeleportResult(ENTITY_ID sourceEntityID, COMPONENT_ID sourceBaseAppID, 
		SPACE_ID spaceID, SPACE_ID lastSpaceID, bool fromCellTeleport);

private:
	struct BufferedScriptCall
	{
		EntityPtr		entityPtr;
		PyObject *		pyCallable;
		// ����ΪNULL�� NULL˵��û�в���
		PyObject *		pyFuncArgs;
	};

	typedef std::list<BufferedScriptCall*>					BufferedScriptCallArray;
	static BufferedScriptCallArray							_scriptCallbacksBuffer;
	static int32											_scriptCallbacksBufferNum;
	static int32											_scriptCallbacksBufferCount;

protected:
	// ���entity�Ŀͻ��˲��ֵ�entityCall
	EntityCall*												clientEntityCall_;

	// ���entity��baseapp���ֵ�entityCall
	EntityCall*												baseEntityCall_;

	/** ���entity������ͳ���ǰ��˭�Ŀͻ��˿���
	    null��ʾû�пͻ����ڿ��ƣ���ϵͳ���ƣ���
	    ����ָ��������entity�Ķ����baseEntityCall_��
		����Լ������Լ���Entity.controlledBy = self.base
	*/
	EntityCall *											controlledBy_;

	// ���һ��entityΪghost����ôentity�����һ��Դcell��ָ��
	COMPONENT_ID											realCell_;

	// ���һ��entityΪreal����ôentity���ܻ����һ��ghost��ָ��
	COMPONENT_ID											ghostCell_;	

	// entity�ĵ�ǰλ��
	Position3D												lastpos_;
	Position3D												position_;
	script::ScriptVector3*									pPyPosition_;

	// entity�ĵ�ǰ����
	Direction3D												direction_;	
	script::ScriptVector3*									pPyDirection_;

	// entityλ�ó�����ĳʱ���Ƿ�ı��
	// �����Կ�������:������ĳ�ڼ��Ƿ�Ҫ�߶�ͬ����entity
	GAME_TIME												posChangedTime_;
	GAME_TIME												dirChangedTime_;

	// �Ƿ��ڵ�����
	bool													isOnGround_;

	// entity x,z������ƶ��ٶ�
	float													topSpeed_;

	// entity y������ƶ��ٶ�
	float													topSpeedY_;

	// ������space��entities�е�λ��
	SPACE_ENTITIES::size_type								spaceEntityIdx_;

	// �Ƿ��κι۲��߼��ӵ�
	std::list<ENTITY_ID>									witnesses_;
	size_t													witnesses_count_;

	// �۲��߶���
	Witness*												pWitness_;

	AllClients*												allClients_;
	AllClients*												otherClients_;

	// entity�ڵ�
	EntityCoordinateNode*									pEntityCoordinateNode_;	

	// ������������
	Controllers*											pControllers_;
	KBEShared_ptr<Controller>								pMoveController_;
	KBEShared_ptr<Controller>								pTurnController_;
	
	script::ScriptVector3::PYVector3ChangedCallback			pyPositionChangedCallback_;
	script::ScriptVector3::PYVector3ChangedCallback			pyDirectionChangedCallback_;
	
	// entity�㣬�����������ʾ������tile����Ϸ���Ա�ʾΪ��½�յȲ㣬��3dҲ���Ա�ʾ���ֲ�
	// �ڽű�����������ʱ����԰�������.
	int8													layer_;
	
	// ��Ҫ�־û��������Ƿ���ࣨ�ڴ�sha1�������û�б��಻��Ҫ�־û�
	uint32													persistentDigest_[5];

	// ����û������ù�Volatileinfo����˴�����Volatileinfo������ΪNULLʹ��ScriptDefModule��Volatileinfo
	VolatileInfo*											pCustomVolatileinfo_;
};

}

#ifdef CODE_INLINE
#include "entity.inl"
#endif
#endif // KBE_ENTITY_H
