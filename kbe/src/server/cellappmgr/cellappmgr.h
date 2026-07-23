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


#ifndef KBE_CELLAPPMGR_H
#define KBE_CELLAPPMGR_H
	
#include "cellapp.h"
#include "space_viewer.h"
#include "server/kbemain.h"
#include "server/serverapp.h"
#include "server/idallocate.h"
#include "server/serverconfig.h"
#include "server/forward_messagebuffer.h"
#include "common/timer.h"
#include "network/endpoint.h"

namespace KBEngine{


class Cellappmgr :	public ServerApp, 
					public Singleton<Cellappmgr>
{
public:
	enum TimeOutType
	{
		TIMEOUT_GAME_TICK = TIMEOUT_SERVERAPP_MAX + 1
	};
	
	Cellappmgr(Network::EventDispatcher& dispatcher, 
		Network::NetworkInterface& ninterface, 
		COMPONENT_TYPE componentType,
		COMPONENT_ID componentID);

	~Cellappmgr();
	
	bool run();
	
	virtual void onChannelDeregister(Network::Channel * pChannel);

	void handleTimeout(TimerHandle handle, void * arg);
	void handleGameTick();

	/* ��ʼ����ؽӿ� */
	bool initializeBegin();
	bool inInitialize();
	bool initializeEnd();
	void finalise();

	/** �ҳ�һ������е�cellapp */
	COMPONENT_ID findFreeCellapp(void);
	void updateBestCellapp();

	/** ����ӿ�
		baseEntity���󴴽���һ���µ�space��
	*/
	void reqCreateCellEntityInNewSpace(Network::Channel* pChannel, MemoryStream& s);

	/** ����ӿ�
		baseEntity���󴴽���һ���µ�space��
	*/
	void reqRestoreSpaceInCell(Network::Channel* pChannel, MemoryStream& s);
	
	/** ����ӿ�
		��Ϣת���� ��ĳ��app��ͨ����app����Ϣת����ĳ��app��
	*/
	void forwardMessage(Network::Channel* pChannel, MemoryStream& s);

	/** ����ӿ�
		����cellapp�����
	*/
	void updateCellapp(Network::Channel* pChannel, COMPONENT_ID componentID, ENTITY_ID numEntities, float load, uint32 flags);

	/** ����ӿ�
		cellappͬ���Լ��ĳ�ʼ����Ϣ
		startGlobalOrder: ȫ������˳�� �������ֲ�ͬ���
		startGroupOrder: ��������˳�� ����������baseapp�еڼ���������
	*/
	void onCellappInitProgress(Network::Channel* pChannel, COMPONENT_ID cid, float progress, 
		COMPONENT_ORDER componentGlobalOrder, COMPONENT_ORDER componentGroupOrder);

	bool componentsReady();
	bool componentReady(COMPONENT_ID cid);

	void removeCellapp(COMPONENT_ID cid);
	Cellapp& getCellapp(COMPONENT_ID cid);
	std::map< COMPONENT_ID, Cellapp >& cellapps();

	uint32 numLoadBalancingApp();

	/* ��groupOrderIDΪ�����׼��
	   ����һ��cellapp component id��cellapp_cids_�б���
	*/
	void addCellappComponentID(COMPONENT_ID cid);

	/** ����ӿ�
	��ѯ������ؽ��̸�����Ϣ
	*/
	void queryAppsLoads(Network::Channel* pChannel, MemoryStream& s);

	/** ����ӿ�
	��ѯ������ؽ���space��Ϣ
	*/
	void querySpaces(Network::Channel* pChannel, MemoryStream& s);

	/** ����ӿ�
	������ؽ���space��Ϣ��ע�⣺��spaceData����API�ĵ���������spaceData
	��ָspace��һЩ��Ϣ
	*/
	void updateSpaceData(Network::Channel* pChannel, MemoryStream& s);

	/** ����ӿ�
	��������ı�space�鿴���������Ӻ�ɾ�����ܣ�
	�����������²��ҷ������ϲ����ڸõ�ַ�Ĳ鿴�����Զ������������ɾ������ȷ����ɾ��Ҫ��
	*/
	void setSpaceViewer(Network::Channel* pChannel, MemoryStream& s);

protected:
	TimerHandle							gameTimer_;
	ForwardAnywhere_MessageBuffer		forward_anywhere_cellapp_messagebuffer_;
	ForwardComponent_MessageBuffer		forward_cellapp_messagebuffer_;

	COMPONENT_ID						bestCellappID_;

	std::map< COMPONENT_ID, Cellapp >	cellapps_;
	std::vector<COMPONENT_ID>			cellapp_cids_;

	// ͨ�����߲鿴space
	SpaceViewers						spaceViewers_;
};

} 

#endif // KBE_CELLAPPMGR_H
