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

#ifndef KBE_WITNESS_H
#define KBE_WITNESS_H

// common include
#include "updatable.h"
#include "entityref.h"
#include "helper/debug_helper.h"
#include "common/common.h"
#include "common/objectpool.h"
#include "math/math.h"

// #define NDEBUG
// windows include	
#if KBE_PLATFORM == PLATFORM_WIN32	
#else
// linux include
#endif

namespace KBEngine{

namespace Network
{
	class Bundle;
	class MessageHandler;
}

class Entity;
class MemoryStream;
class ViewTrigger;
class Space;

/** �۲�����Ϣ�ṹ */
struct WitnessInfo
{
	WitnessInfo(const int8& lv, Entity* e, const float& r):
	detailLevel(lv),
	entity(e),
	range(r)
	{
		for(int i=0; i<3; ++i)
			if(lv == i)
				detailLevelLog[i] = true;
			else
				detailLevelLog[i] = false;
	}
	
	int8 detailLevel;							// ��ǰ�������鼶��
	Entity* entity;								// �������entity
	float range;								// ��ǰ�����entity�ľ���
	bool detailLevelLog[3];						// ��ʾ���entity���������entity����Щ���鼶�� �ṩ���Թ㲥�Ż��õ�
												// ��û�н����ĳ����ʱ�� �Ὣ���������������Ը��¸����� ����ֻ���½���ʱ�������ı��������
	std::vector<uint32> changeDefDataLogs[3];	// entity�뿪��ĳ�����鼶��(û������witness)�� ���ڼ���ĳ�����鼶������Ըı����¼������
};

/**
	���ģ�������������Ǹ���Ȥ��entity���ݣ� �磺view�� ���Ը��£� ����entity�ķ���
	�����䴫��������ߡ�
*/
class Witness : public PoolObject, public Updatable
{
public:
	typedef std::list<EntityRef*> VIEW_ENTITIES;
	typedef std::map<ENTITY_ID, EntityRef*> VIEW_ENTITIES_MAP;

	Witness();
	~Witness();
	
	virtual uint8 updatePriority() const {
		return 1;
	}

	void addToStream(KBEngine::MemoryStream& s);
	void createFromStream(KBEngine::MemoryStream& s);

	typedef KBEShared_ptr< SmartPoolObject< Witness > > SmartPoolObjectPtr;
	static SmartPoolObjectPtr createSmartPoolObj(const std::string& logPoint);

	static ObjectPool<Witness>& ObjPool();
	static Witness* createPoolObject(const std::string& logPoint);
	static void reclaimPoolObject(Witness* obj);
	static void destroyObjPool();
	void onReclaimObject();

	virtual size_t getPoolObjectBytes()
	{
		size_t bytes = sizeof(pEntity_)
		 + sizeof(viewRadius_) + sizeof(viewHysteresisArea_)
		 + sizeof(pViewTrigger_) + sizeof(pViewHysteresisAreaTrigger_) + sizeof(clientViewSize_)
		 + sizeof(lastBasePos_) + (sizeof(EntityRef*) * viewEntities_map_.size());

		return bytes;
	}

	INLINE void pEntity(Entity* pEntity);
	INLINE Entity* pEntity();

	void attach(Entity* pEntity);
	void detach(Entity* pEntity);
	void clear(Entity* pEntity);
	void onAttach(Entity* pEntity);

	void setViewRadius(float radius, float hyst = 5.0f);
	
	INLINE float viewRadius() const;
	INLINE float viewHysteresisArea() const;

	typedef std::vector<Network::Bundle*> Bundles;
	bool pushBundle(Network::Bundle* pBundle);

	/**
		����λ�ã� ������������λ�ÿ����������
	*/
	INLINE const Position3D& basePos();

	/**
	�������� ��������������������������
	*/
	INLINE const Direction3D& baseDir();

	bool update();
	
	void onEnterSpace(Space* pSpace);
	void onLeaveSpace(Space* pSpace);

	void onEnterView(ViewTrigger* pViewTrigger, Entity* pEntity);
	void onLeaveView(ViewTrigger* pViewTrigger, Entity* pEntity);
	void _onLeaveView(EntityRef* pEntityRef);

	/**
		���ʵ�屾��ͬ��Volatile���ݵı��
	*/
	uint32 getEntityVolatileDataUpdateFlags(Entity* otherEntity);
	

	const Network::MessageHandler& getViewEntityMessageHandler(const Network::MessageHandler& normalMsgHandler,
											   const Network::MessageHandler& optimizedMsgHandler, ENTITY_ID entityID, int& ialiasID);

	bool entityID2AliasID(ENTITY_ID id, uint8& aliasID);

	/**
		ʹ�ú���Э�������¿ͻ���
	*/
	void addUpdateToStream(Network::Bundle* pForwardBundle, uint32 flags, EntityRef* pEntityRef);

	/**
		���ӻ���λ�õ����°�
	*/
	void addBaseDataToStream(Network::Bundle* pSendBundle);

	/**
		��witness�ͻ�������һ����Ϣ
	*/
	bool sendToClient(const Network::MessageHandler& msgHandler, Network::Bundle* pBundle);
	Network::Channel* pChannel();
		
	INLINE VIEW_ENTITIES_MAP& viewEntitiesMap();
	INLINE VIEW_ENTITIES& viewEntities();

	/** ���viewentity������ */
	INLINE EntityRef* getViewEntityRef(ENTITY_ID entityID);

	/** entityID�Ƿ���view�� */
	INLINE bool entityInView(ENTITY_ID entityID);

	INLINE ViewTrigger* pViewTrigger();
	INLINE ViewTrigger* pViewHysteresisAreaTrigger();
	
	void installViewTrigger();
	void uninstallViewTrigger();

	/**
		����View��Χ�ڵ�entities�� ʹ��ͬ��״̬�ָ������δͬ����״̬
	*/
	void resetViewEntities();

private:
	/**
		���view��entity����С��256��ֻ��������λ��
	*/
	INLINE void _addViewEntityIDToBundle(Network::Bundle* pBundle, EntityRef* pEntityRef);
	
	/**
		��updateִ��ʱview�б��иı��ʱ����Ҫ����entityRef��aliasID
	*/
	void updateEntitiesAliasID();
		
private:
	Entity*									pEntity_;

	// ��ǰentity��view�뾶
	float									viewRadius_;
	// ��ǰentityview��һ���ͺ�Χ
	float									viewHysteresisArea_;

	ViewTrigger*							pViewTrigger_;
	ViewTrigger*							pViewHysteresisAreaTrigger_;

	VIEW_ENTITIES							viewEntities_;
	VIEW_ENTITIES_MAP						viewEntities_map_;

	Position3D								lastBasePos_;
	Direction3D								lastBaseDir_;

	uint16									clientViewSize_;
};

}

#ifdef CODE_INLINE
#include "witness.inl"
#endif
#endif
