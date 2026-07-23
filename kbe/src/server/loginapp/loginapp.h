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


#ifndef KBE_LOGINAPP_H
#define KBE_LOGINAPP_H
	
// common include	
#include "server/kbemain.h"
#include "server/serverapp.h"
#include "server/idallocate.h"
#include "server/serverconfig.h"
#include "server/pendingLoginmgr.h"
#include "server/python_app.h"
#include "common/timer.h"
#include "network/endpoint.h"
	
namespace KBEngine{

class HTTPCBHandler;
class TelnetServer;

class Loginapp :	public PythonApp, 
					public Singleton<Loginapp>
{
public:
	enum TimeOutType
	{
		TIMEOUT_TICK = TIMEOUT_PYTHONAPP_MAX + 1
	};

	Loginapp(Network::EventDispatcher& dispatcher, 
		Network::NetworkInterface& ninterface, 
		COMPONENT_TYPE componentType,
		COMPONENT_ID componentID);

	~Loginapp();
	
	bool run();
	
	virtual void onChannelDeregister(Network::Channel * pChannel);

	virtual void handleTimeout(TimerHandle handle, void * arg);
	void handleMainTick();

	/* ��ʼ����ؽӿ� */
	bool initializeBegin();
	bool inInitialize();
	bool initializeEnd();
	void finalise();
	void onInstallPyModules();
	
	virtual void onShutdownBegin();
	virtual void onShutdownEnd();

	/** �źŴ���
	*/
	virtual bool installSignals();
	virtual void onSignalled(int sigNum);

	virtual void onHello(Network::Channel* pChannel, 
		const std::string& verInfo, 
		const std::string& scriptVerInfo, 
		const std::string& encryptedKey);

	/** ����ӿ�
		ĳ��client��app��֪���ڻ״̬��
	*/
	void onClientActiveTick(Network::Channel* pChannel);

	/** ����ӿ�
		�����˺�
	*/
	bool _createAccount(Network::Channel* pChannel, std::string& accountName, 
		std::string& password, std::string& datas, ACCOUNT_TYPE type = ACCOUNT_TYPE_NORMAL);
	void reqCreateAccount(Network::Channel* pChannel, MemoryStream& s);

	/** ����ӿ�
		����email�˺�
	*/
	void reqCreateMailAccount(Network::Channel* pChannel, MemoryStream& s);

	/** ����ӿ�
		�����˺�
	*/
	void onReqCreateAccountResult(Network::Channel* pChannel, MemoryStream& s);
	void onReqCreateMailAccountResult(Network::Channel* pChannel, MemoryStream& s);

	/** ����ӿ�
		�����˺��������루��������?��
	*/
	void reqAccountResetPassword(Network::Channel* pChannel, std::string& accountName);
	void onReqAccountResetPasswordCB(Network::Channel* pChannel, std::string& accountName, std::string& email,
		SERVER_ERROR_CODE failedcode, std::string& code);

	/** ����ӿ�
		dbmgr�˺ż����
	*/
	void onAccountActivated(Network::Channel* pChannel, std::string& code, bool success);

	/** ����ӿ�
		dbmgr�˺Ű�email����
	*/
	void onAccountBindedEmail(Network::Channel* pChannel, std::string& code, bool success);

	/** ����ӿ�
		dbmgr�˺��������뷵��
	*/
	void onAccountResetPassword(Network::Channel* pChannel, std::string& code, bool success);

	/** ����ӿ�
	baseapp�����email������ʱ��Ҫ�ҵ�loginapp�ĵ�ַ��
	*/
	void onReqAccountBindEmailAllocCallbackLoginapp(Network::Channel* pChannel, COMPONENT_ID reqBaseappID, ENTITY_ID entityID, std::string& accountName, std::string& email,
		SERVER_ERROR_CODE failedcode, std::string& code);

	/** ����ӿ�
		�û���¼������
		clientType[COMPONENT_CLIENT_TYPE]: ǰ�����(�ֻ��� web�� pcexe��)
		clientData[str]: ǰ�˸�������(����������ģ� ���總���ֻ��ͺţ� ��������͵�)
		accountName[str]: �ʺ���
		password[str]: ����
	*/
	void login(Network::Channel* pChannel, MemoryStream& s);

	/*
		��¼ʧ��
		failedcode: ʧ�ܷ����� NETWORK_ERR_SRV_NO_READY:������û��׼����, 
									NETWORK_ERR_SRV_OVERLOAD:���������ع���, 
									NETWORK_ERR_NAME_PASSWORD:�û����������벻��ȷ
	*/
	void _loginFailed(Network::Channel* pChannel, std::string& loginName, 
		SERVER_ERROR_CODE failedcode, std::string& datas, bool force = false);
	
	/** ����ӿ�
		dbmgr���صĵ�¼�˺ż����
	*/
	void onLoginAccountQueryResultFromDbmgr(Network::Channel* pChannel, MemoryStream& s);

	/** ����ӿ�
		baseappmgr���صĵ�¼���ص�ַ
	*/
	void onLoginAccountQueryBaseappAddrFromBaseappmgr(Network::Channel* pChannel, std::string& loginName, 
		std::string& accountName, std::string& addr, uint16 port);


	/** ����ӿ�
		dbmgr���ͳ�ʼ��Ϣ
		startGlobalOrder: ȫ������˳�� �������ֲ�ͬ���
		startGroupOrder: ��������˳�� ����������baseapp�еڼ���������
	*/
	void onDbmgrInitCompleted(Network::Channel* pChannel, COMPONENT_ORDER startGlobalOrder, 
		COMPONENT_ORDER startGroupOrder, const std::string& digest);

	/** ����ӿ�
		�ͻ���Э�鵼��
	*/
	void importClientMessages(Network::Channel* pChannel);

	/** ����ӿ�
		��������������
	*/
	void importServerErrorsDescr(Network::Channel* pChannel);

	/** ����ӿ�
	�ͻ���SDK����
	*/
	void importClientSDK(Network::Channel* pChannel, MemoryStream& s);

	// ����汾��ƥ��
	virtual void onVersionNotMatch(Network::Channel* pChannel);

	// ����ű���汾��ƥ��
	virtual void onScriptVersionNotMatch(Network::Channel* pChannel);

	/** ����ӿ�
		baseappͬ���Լ��ĳ�ʼ����Ϣ
		startGlobalOrder: ȫ������˳�� �������ֲ�ͬ���
		startGroupOrder: ��������˳�� ����������baseapp�еڼ���������
	*/
	void onBaseappInitProgress(Network::Channel* pChannel, float progress);

protected:
	TimerHandle							mainProcessTimer_;

	// ��¼ע���˺Ż�δ��½������
	PendingLoginMgr						pendingCreateMgr_;

	// ��¼��¼������������δ������ϵ��˺�
	PendingLoginMgr						pendingLoginMgr_;

	std::string							digest_;

	HTTPCBHandler*						pHttpCBHandler;

	float								initProgress_;
	
	TelnetServer*						pTelnetServer_;
};

}

#endif // KBE_LOGINAPP_H
