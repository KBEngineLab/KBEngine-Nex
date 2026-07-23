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

#ifndef KBE_GHOST_MANAGER_HANDLER_H
#define KBE_GHOST_MANAGER_HANDLER_H

// common include
#include "helper/debug_helper.h"
#include "common/common.h"

namespace KBEngine{

namespace Network
{
class Bundle;
}

class Entity;

/*
	* cell1: entity(1) is real, ����GhostManager�д����entityIDs_���м��  (������ghost����)

	* cell2: entity(1) is ghost, ���cell2������Ǩ���ߣ�����Ҫ��ghost_route_��ʱ����һ��·�ɵ�ַ�� ·�������һ���հ�����һ��ʱ�������
	                    ����ڼ���һЩ����ת�������� ��ô�Ҳ���entity�Ͳ�ѯ·�ɱ���������ת����ghostEntity(����real������Ҫ����������ghost)��

	* cell1: entity(1) is real, �������Ǩ�Ƶ�cell3�� ����Ҫ��ghost_route_��ʱ����һ��·�ɵ�ַ�� ·�������һ����ghost���������һ��ʱ�������
	                    ����ڼ���һЩghost�������ת�������� ��ô�Ҳ���entity�Ͳ�ѯ·�ɱ���������ת����realEntity��
*/
class GhostManager : public TimerHandler
{
public:
	GhostManager();
	~GhostManager();

	void pushMessage(COMPONENT_ID componentID, Network::Bundle* pBundle);
	void pushRouteMessage(ENTITY_ID entityID, COMPONENT_ID componentID, Network::Bundle* pBundle);

	COMPONENT_ID getRoute(ENTITY_ID entityID);
	void addRoute(ENTITY_ID entityID, COMPONENT_ID componentID);

	/**
	��������bundle����bundle�����Ǵ�send���뷢�Ͷ����л�ȡ�ģ��������Ϊ��
	�򴴽�һ���µ�
	*/
	Network::Bundle* createSendBundle(COMPONENT_ID componentID);

private:
	virtual void handleTimeout(TimerHandle handle, void * pUser);

	virtual void onRelease( TimerHandle handle, void * /*pUser*/ ){};

	void cancel();

	void start();

private:
	void syncMessages();
	void syncGhosts();

	void checkRoute();

	struct ROUTE_INFO
	{
		ROUTE_INFO():
		componentID(0),
		lastTime(0)
		{
		}

		COMPONENT_ID componentID;
		uint64 lastTime;
	};

private:
	// ���д���ghost�����entity
	std::map<ENTITY_ID, Entity*> 	realEntities_;
	
	// ghost·�ɣ� �ֲ�ʽ����ĳЩʱ���޷���֤ͬ���� ��ô�ڱ����ϵ�ĳЩentity��Ǩ�����˵�
	// ʱ����ܻỹ���յ�һЩ������Ϣ�� ��Ϊ����app���ܻ��޷������õ�Ǩ�Ƶ�ַ�� ��ʱ����
	// �����ڵ�ǰapp�Ͻ�Ǩ���ߵ�entityָ�򻺴�һ�£� ��������Ϣ�������ǿ��Լ���ת�����µĵ�ַ
	std::map<ENTITY_ID, ROUTE_INFO> ghost_route_;

	// ������Ҫ�㲥���¼���Ϣ
	std::map<COMPONENT_ID, std::vector< Network::Bundle* > > messages_;

	TimerHandle* pTimerHandle_;

	uint64 checkTime_;
};


}

#endif // KBE_GHOST_MANAGER_HANDLER_H
