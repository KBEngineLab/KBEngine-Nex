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


#ifndef KBE_BASEAPPMGR_H
#define KBE_BASEAPPMGR_H

#include "baseapp.h"
#include "server/kbemain.h"
#include "server/serverapp.h"
#include "server/idallocate.h"
#include "server/serverconfig.h"
#include "server/forward_messagebuffer.h"
#include "common/timer.h"
#include "network/endpoint.h"

namespace KBEngine{

class Baseappmgr :	public ServerApp, 
					public Singleton<Baseappmgr>
{
public:
	enum TimeOutType
	{
		TIMEOUT_GAME_TICK = TIMEOUT_SERVERAPP_MAX + 1
	};
	
	Baseappmgr(Network::EventDispatcher& dispatcher, 
		Network::NetworkInterface& ninterface, 
		COMPONENT_TYPE componentType,
		COMPONENT_ID componentID);

	~Baseappmgr();
	
	bool run();
	
	virtual void onChannelDeregister(Network::Channel * pChannel);
	virtual void onAddComponent(const Components::ComponentInfos* pInfos);

	void handleTimeout(TimerHandle handle, void * arg);
	void handleGameTick();

	/* ��ʼ����ؽӿ� */
	bool initializeBegin();
	bool inInitialize();
	bool initializeEnd();
	void finalise();

	COMPONENT_ID findFreeBaseapp();
	void updateBestBaseapp();

	/** ����ӿ�
		baseapp::createEntityAnywhere��ѯ��ǰ��õ����ID
	*/
	void reqCreateEntityAnywhereFromDBIDQueryBestBaseappID(Network::Channel* pChannel, MemoryStream& s);

	/** ����ӿ�
		�յ�baseapp::createEntityAnywhere������ĳ�����е�baseapp�ϴ���һ��baseEntity
		@param sp: ������ݰ��д洢���� entityType	: entity����� entities.xml�еĶ���ġ�
										strInitData	: ���entity��������Ӧ�ø�����ʼ����һЩ���ݣ� 
													  ��Ҫʹ��pickle.loads���.
										componentID	: ���󴴽�entity��baseapp�����ID
	*/
	void reqCreateEntityAnywhere(Network::Channel* pChannel, MemoryStream& s);

	/** ����ӿ�
	�յ�baseapp::createEntityRemotely������ĳ�����е�baseapp�ϴ���һ��baseEntity
	@param sp: ������ݰ��д洢���� entityType	: entity����� entities.xml�еĶ���ġ�
	strInitData	: ���entity��������Ӧ�ø�����ʼ����һЩ���ݣ�
	��Ҫʹ��pickle.loads���.
	componentID	: ���󴴽�entity��baseapp�����ID
	*/
	void reqCreateEntityRemotely(Network::Channel* pChannel, MemoryStream& s);

	/** ����ӿ�
		�յ�baseapp::createEntityAnywhereFromDBID������ĳ�����е�baseapp�ϴ���һ��baseEntity
	*/
	void reqCreateEntityAnywhereFromDBID(Network::Channel* pChannel, MemoryStream& s);

	/** ����ӿ�
		�յ�baseapp::createEntityRemotelyFromDBID������ĳ�����е�baseapp�ϴ���һ��baseEntity
	*/
	void reqCreateEntityRemotelyFromDBID(Network::Channel* pChannel, MemoryStream& s);
	
	/** ����ӿ�
		��Ϣת���� ��ĳ��app��ͨ����app����Ϣת����ĳ��app��
	*/
	void forwardMessage(Network::Channel* pChannel, MemoryStream& s);

	/** ����ӿ�
		һ���µ�¼���˺Ż�úϷ�����baseapp��Ȩ���� ������Ҫ���˺�ע���baseapp
		ʹ�������ڴ�baseapp�ϵ�¼��
	*/
	void registerPendingAccountToBaseapp(Network::Channel* pChannel, MemoryStream& s);

	/** ����ӿ�
		һ���µ�¼���˺Ż�úϷ�����baseapp��Ȩ���� ������Ҫ���˺�ע���ָ����baseapp
		ʹ�������ڴ�baseapp�ϵ�¼��
	*/
	void registerPendingAccountToBaseappAddr(Network::Channel* pChannel, MemoryStream& s);

	/** ����ӿ�
		baseapp���Լ��ĵ�ַ���͸�loginapp��ת�����ͻ��ˡ�
	*/
	void onPendingAccountGetBaseappAddr(Network::Channel* pChannel, 
								  std::string& loginName, std::string& accountName, 
								  std::string& addr, uint16 port);

	/** ����ӿ�
		����baseapp�����
	*/
	void updateBaseapp(Network::Channel* pChannel, COMPONENT_ID componentID,
								ENTITY_ID numEntitys, ENTITY_ID numProxices, float load, uint32 flags);

	/** ����ӿ�
		baseappͬ���Լ��ĳ�ʼ����Ϣ
		startGlobalOrder: ȫ������˳�� �������ֲ�ͬ���
		startGroupOrder: ��������˳�� ����������baseapp�еڼ���������
	*/
	void onBaseappInitProgress(Network::Channel* pChannel, COMPONENT_ID cid, float progress);

	/** 
		�������baseapp��ַ���͸�loginapp��ת�����ͻ��ˡ�
	*/
	void sendAllocatedBaseappAddr(Network::Channel* pChannel, 
								  std::string& loginName, std::string& accountName, 
								  const std::string& addr, uint16 port);

	bool componentsReady();
	bool componentReady(COMPONENT_ID cid);

	std::map< COMPONENT_ID, Baseapp >& baseapps();

	uint32 numLoadBalancingApp();

	/** ����ӿ�
		��ѯ������ؽ��̸�����Ϣ
	*/
	void queryAppsLoads(Network::Channel* pChannel, MemoryStream& s);

	/** ����ӿ�
		baseapp�����email������ʱ��Ҫ�ҵ�loginapp�ĵ�ַ��
	*/
	void reqAccountBindEmailAllocCallbackLoginapp(Network::Channel* pChannel, COMPONENT_ID reqBaseappID, ENTITY_ID entityID, std::string& accountName, std::string& email,
		SERVER_ERROR_CODE failedcode, std::string& code);

	/** ����ӿ�
		�����email, loginapp������Ҫ�ҵ�loginapp�ĵ�ַ
	*/
	void onReqAccountBindEmailCBFromLoginapp(Network::Channel* pChannel, COMPONENT_ID reqBaseappID, ENTITY_ID entityID, std::string& accountName, std::string& email,
		SERVER_ERROR_CODE failedcode, std::string& code, std::string& loginappCBHost, uint16 loginappCBPort);

protected:
	TimerHandle													gameTimer_;

	ForwardAnywhere_MessageBuffer								forward_anywhere_baseapp_messagebuffer_;
	ForwardComponent_MessageBuffer								forward_baseapp_messagebuffer_;

	COMPONENT_ID												bestBaseappID_;

	std::map< COMPONENT_ID, Baseapp >							baseapps_;

	KBEUnordered_map< std::string, COMPONENT_ID >				pending_logins_;

	float														baseappsInitProgress_;
};

}

#endif // KBE_BASEAPPMGR_H
