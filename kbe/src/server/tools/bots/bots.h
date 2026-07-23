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


#ifndef KBE_BOTS_H
#define KBE_BOTS_H
	
// common include	
#include "profile.h"
#include "create_and_login_handler.h"
#include "common/timer.h"
#include "pyscript/script.h"
#include "network/endpoint.h"
#include "helper/debug_helper.h"
#include "helper/script_loglevel.h"
#include "xml/xml.h"	
#include "common/singleton.h"
#include "common/smartpointer.h"
#include "common/timer.h"
#include "network/interfaces.h"
#include "network/event_dispatcher.h"
#include "network/network_interface.h"
#include "network/event_poller.h"
#include "client_lib/clientapp.h"
#include "pyscript/pyobject_pointer.h"
#include "entitydef/entitydef.h"

//#define NDEBUG
// windows include	
#if KBE_PLATFORM == PLATFORM_WIN32
#else
// linux include
#endif
	
namespace KBEngine{

class ClientObject;
class PyBots;
class TelnetServer;

class Bots  : public ClientApp
{
public:
	Bots(Network::EventDispatcher& dispatcher, 
		Network::NetworkInterface& ninterface, 
		COMPONENT_TYPE componentType,
		COMPONENT_ID componentID);

	~Bots();

	virtual bool initialize();
	virtual void finalise();

	virtual bool initializeBegin();
	virtual bool initializeEnd();

	virtual bool installPyModules();
	virtual void onInstallPyModules() {};
	virtual bool uninstallPyModules();
	bool uninstallPyScript();
	bool installEntityDef();

	virtual void handleTimeout(TimerHandle, void * pUser);
	virtual void handleGameTick();

	static Bots& getSingleton(){ 
		return *static_cast<Bots*>(ClientApp::getSingletonPtr()); 
	}

	/**
		���ýű��������ǰ׺
	*/
	static PyObject* __py_setScriptLogType(PyObject* self, PyObject* args);

	bool run(void);

	/**
		��entitycall�����Ի�ȡһ��channel��ʵ��
	*/
	virtual Network::Channel* findChannelByEntityCall(EntityCall& entitycall);

	/** ����ӿ�
		ĳ��app����鿴��app
	*/
	virtual void lookApp(Network::Channel* pChannel);

	/** ����ӿ�
		����رշ�����
	*/
	virtual void reqCloseServer(Network::Channel* pChannel, MemoryStream& s);

	/** ����ӿ�
		����رշ�����
	*/
	void reqKillServer(Network::Channel* pChannel, MemoryStream& s);

	void onExecScriptCommand(Network::Channel* pChannel, KBEngine::MemoryStream& s);

	typedef std::map< Network::Channel*, ClientObject* > CLIENTS;
	CLIENTS& clients(){ return clients_; }

	uint32 reqCreateAndLoginTotalCount(){ return reqCreateAndLoginTotalCount_; }
	void reqCreateAndLoginTotalCount(uint32 v){ reqCreateAndLoginTotalCount_ = v; }

	uint32 reqCreateAndLoginTickCount(){ return reqCreateAndLoginTickCount_; }
	void reqCreateAndLoginTickCount(uint32 v){ reqCreateAndLoginTickCount_ = v; }

	float reqCreateAndLoginTickTime(){ return reqCreateAndLoginTickTime_; }
	void reqCreateAndLoginTickTime(float v){ reqCreateAndLoginTickTime_ = v; }

	bool addClient(ClientObject* pClient);
	bool delClient(ClientObject* pClient);
	bool delClient(Network::Channel * pChannel);

	ClientObject* findClient(Network::Channel * pChannel);
	ClientObject* findClientByAppID(int32 appID);

	static PyObject* __py_addBots(PyObject* self, PyObject* args);

	/** ����ӿ�
	   ����bots
	   @total uint32: �ܹ����ӵĸ���
	   @ticknum uint32: ÿ��tick���Ӷ��ٸ�
	   @ticktime float: һ��tick��ʱ��
	*/
	virtual void addBots(Network::Channel * pChannel, MemoryStream& s);

	/** ����ӿ�
		ĳ��app��app��֪���ڻ״̬��
	*/
	void onAppActiveTick(Network::Channel* pChannel, COMPONENT_TYPE componentType, COMPONENT_ID componentID);

	virtual void onHelloCB_(Network::Channel* pChannel, const std::string& verInfo,
		const std::string& scriptVerInfo, const std::string& protocolMD5, 
		const std::string& entityDefMD5, COMPONENT_TYPE componentType);

	/** ����ӿ�
		�ͷ���˵İ汾��ƥ��
	*/
	virtual void onVersionNotMatch(Network::Channel* pChannel, MemoryStream& s);

	/** ����ӿ�
		�ͷ���˵Ľű���汾��ƥ��
	*/
	virtual void onScriptVersionNotMatch(Network::Channel* pChannel, MemoryStream& s);

	/** ����ӿ�
		�����˺ųɹ���ʧ�ܻص�
	   @failedcode: ʧ�ܷ����� NETWORK_ERR_SRV_NO_READY:������û��׼����, 
									NETWORK_ERR_ACCOUNT_CREATE:����ʧ�ܣ��Ѿ����ڣ�, 
									NETWORK_SUCCESS:�˺Ŵ����ɹ�

									SERVER_ERROR_CODE failedcode;
		@�����Ƹ�������:�����ƶ�������: uint32���� + bytearray
	*/
	virtual void onCreateAccountResult(Network::Channel * pChannel, MemoryStream& s);

	Network::EventPoller* pEventPoller(){ return pEventPoller_; }

	/** ����ӿ�
	   ��¼ʧ�ܻص�
	   @failedcode: ʧ�ܷ����� NETWORK_ERR_SRV_NO_READY:������û��׼����, 
									NETWORK_ERR_SRV_OVERLOAD:���������ع���, 
									NETWORK_ERR_NAME_PASSWORD:�û����������벻��ȷ
	*/
	virtual void onLoginFailed(Network::Channel * pChannel, MemoryStream& s);

	/** ����ӿ�
	   ��¼�ɹ�
	   @ip: ������ip��ַ
	   @port: �������˿�
	*/
	virtual void onLoginSuccessfully(Network::Channel * pChannel, MemoryStream& s);

	/** ����ӿ�
	   ��¼ʧ�ܻص�
	   @failedcode: ʧ�ܷ����� NETWORK_ERR_SRV_NO_READY:������û��׼����, 
									NETWORK_ERR_ILLEGAL_LOGIN:�Ƿ���¼, 
									NETWORK_ERR_NAME_PASSWORD:�û����������벻��ȷ
	*/
	virtual void onLoginBaseappFailed(Network::Channel * pChannel, SERVER_ERROR_CODE failedcode);

	/** ����ӿ�
	   �ص�½baseapp�ɹ�
	*/
	virtual void onReloginBaseappSuccessfully(Network::Channel * pChannel, MemoryStream& s);

	/** ����ӿ�
		���������Ѿ�������һ����ͻ��˹����Ĵ���Entity
	   �ڵ�¼ʱҲ�ɱ���ɹ��ص�
	   @datas: �˺�entity����Ϣ
	*/
	virtual void onCreatedProxies(Network::Channel * pChannel, uint64 rndUUID, 
		ENTITY_ID eid, std::string& entityType);

	/** ����ӿ�
		�������ϵ�entity�Ѿ�������Ϸ������
	*/
	virtual void onEntityEnterWorld(Network::Channel * pChannel, MemoryStream& s);


	/** ����ӿ�
		�������ϵ�entity�Ѿ��뿪��Ϸ������
	*/
	virtual void onEntityLeaveWorld(Network::Channel * pChannel, ENTITY_ID eid);
	virtual void onEntityLeaveWorldOptimized(Network::Channel * pChannel, MemoryStream& s);

	/** ����ӿ�
		���߿ͻ���ĳ��entity�����ˣ� ����entityͨ���ǻ�δonEntityEnterWorld
	*/
	virtual void onEntityDestroyed(Network::Channel * pChannel, ENTITY_ID eid);

	/** ����ӿ�
		�������ϵ�entity�Ѿ�����space��
	*/
	virtual void onEntityEnterSpace(Network::Channel * pChannel, MemoryStream& s);

	/** ����ӿ�
		�������ϵ�entity�Ѿ��뿪space��
	*/
	virtual void onEntityLeaveSpace(Network::Channel * pChannel, ENTITY_ID eid);

	/** ����ӿ�
		Զ�̵���entity�ķ��� 
	*/
	virtual void onRemoteMethodCall(Network::Channel* pChannel, MemoryStream& s);
	virtual void onRemoteMethodCallOptimized(Network::Channel* pChannel, MemoryStream& s);

	/** ����ӿ�
	   ���߳�������
	*/
	virtual void onKicked(Network::Channel * pChannel, SERVER_ERROR_CODE failedcode);

	/** ����ӿ�
		����������entity����
	*/
	virtual void onUpdatePropertys(Network::Channel* pChannel, MemoryStream& s);
	virtual void onUpdatePropertysOptimized(Network::Channel* pChannel, MemoryStream& s);

	/** ����ӿ�
		����������avatar����λ�úͳ���
	*/
	virtual void onUpdateBasePos(Network::Channel* pChannel, float x, float y, float z);
	virtual void onUpdateBasePosXZ(Network::Channel* pChannel, float x, float z);
	virtual void onUpdateBaseDir(Network::Channel* pChannel, MemoryStream& s);

	/** ����ӿ�
		������ǿ������entity��λ���볯��
	*/
	virtual void onSetEntityPosAndDir(Network::Channel* pChannel, MemoryStream& s);

	/** ����ӿ�
		����������VolatileData
	*/
	virtual void onUpdateData(Network::Channel* pChannel, MemoryStream& s);

	/** ����ӿ�
		���Ż��߾���ͬ��
	*/
	virtual void onUpdateData_ypr(Network::Channel* pChannel, MemoryStream& s);
	virtual void onUpdateData_yp(Network::Channel* pChannel, MemoryStream& s);
	virtual void onUpdateData_yr(Network::Channel* pChannel, MemoryStream& s);
	virtual void onUpdateData_pr(Network::Channel* pChannel, MemoryStream& s);
	virtual void onUpdateData_y(Network::Channel* pChannel, MemoryStream& s);
	virtual void onUpdateData_p(Network::Channel* pChannel, MemoryStream& s);
	virtual void onUpdateData_r(Network::Channel* pChannel, MemoryStream& s);

	virtual void onUpdateData_xz(Network::Channel* pChannel, MemoryStream& s);
	virtual void onUpdateData_xz_ypr(Network::Channel* pChannel, MemoryStream& s);
	virtual void onUpdateData_xz_yp(Network::Channel* pChannel, MemoryStream& s);
	virtual void onUpdateData_xz_yr(Network::Channel* pChannel, MemoryStream& s);
	virtual void onUpdateData_xz_pr(Network::Channel* pChannel, MemoryStream& s);
	virtual void onUpdateData_xz_y(Network::Channel* pChannel, MemoryStream& s);
	virtual void onUpdateData_xz_p(Network::Channel* pChannel, MemoryStream& s);
	virtual void onUpdateData_xz_r(Network::Channel* pChannel, MemoryStream& s);

	virtual void onUpdateData_xyz(Network::Channel* pChannel, MemoryStream& s);
	virtual void onUpdateData_xyz_ypr(Network::Channel* pChannel, MemoryStream& s);
	virtual void onUpdateData_xyz_yp(Network::Channel* pChannel, MemoryStream& s);
	virtual void onUpdateData_xyz_yr(Network::Channel* pChannel, MemoryStream& s);
	virtual void onUpdateData_xyz_pr(Network::Channel* pChannel, MemoryStream& s);
	virtual void onUpdateData_xyz_y(Network::Channel* pChannel, MemoryStream& s);
	virtual void onUpdateData_xyz_p(Network::Channel* pChannel, MemoryStream& s);
	virtual void onUpdateData_xyz_r(Network::Channel* pChannel, MemoryStream& s);

	/** ����ӿ�
		�Ż���λ��ͬ��
	*/
	virtual void onUpdateData_ypr_optimized(Network::Channel* pChannel, MemoryStream& s);
	virtual void onUpdateData_yp_optimized(Network::Channel* pChannel, MemoryStream& s);
	virtual void onUpdateData_yr_optimized(Network::Channel* pChannel, MemoryStream& s);
	virtual void onUpdateData_pr_optimized(Network::Channel* pChannel, MemoryStream& s);
	virtual void onUpdateData_y_optimized(Network::Channel* pChannel, MemoryStream& s);
	virtual void onUpdateData_p_optimized(Network::Channel* pChannel, MemoryStream& s);
	virtual void onUpdateData_r_optimized(Network::Channel* pChannel, MemoryStream& s);

	virtual void onUpdateData_xz_optimized(Network::Channel* pChannel, MemoryStream& s);
	virtual void onUpdateData_xz_ypr_optimized(Network::Channel* pChannel, MemoryStream& s);
	virtual void onUpdateData_xz_yp_optimized(Network::Channel* pChannel, MemoryStream& s);
	virtual void onUpdateData_xz_yr_optimized(Network::Channel* pChannel, MemoryStream& s);
	virtual void onUpdateData_xz_pr_optimized(Network::Channel* pChannel, MemoryStream& s);
	virtual void onUpdateData_xz_y_optimized(Network::Channel* pChannel, MemoryStream& s);
	virtual void onUpdateData_xz_p_optimized(Network::Channel* pChannel, MemoryStream& s);
	virtual void onUpdateData_xz_r_optimized(Network::Channel* pChannel, MemoryStream& s);

	virtual void onUpdateData_xyz_optimized(Network::Channel* pChannel, MemoryStream& s);
	virtual void onUpdateData_xyz_ypr_optimized(Network::Channel* pChannel, MemoryStream& s);
	virtual void onUpdateData_xyz_yp_optimized(Network::Channel* pChannel, MemoryStream& s);
	virtual void onUpdateData_xyz_yr_optimized(Network::Channel* pChannel, MemoryStream& s);
	virtual void onUpdateData_xyz_pr_optimized(Network::Channel* pChannel, MemoryStream& s);
	virtual void onUpdateData_xyz_y_optimized(Network::Channel* pChannel, MemoryStream& s);
	virtual void onUpdateData_xyz_p_optimized(Network::Channel* pChannel, MemoryStream& s);
	virtual void onUpdateData_xyz_r_optimized(Network::Channel* pChannel, MemoryStream& s);

	/** ����ӿ�
		download stream��ʼ�� 
	*/
	virtual void onStreamDataStarted(Network::Channel* pChannel, int16 id, uint32 datasize, std::string& descr);

	/** ����ӿ�
		���յ�streamData
	*/
	virtual void onStreamDataRecv(Network::Channel* pChannel, MemoryStream& s);

	/** ����ӿ�
		download stream����� 
	*/
	virtual void onStreamDataCompleted(Network::Channel* pChannel, int16 id);

	/** ����ӿ�
		space��ز����ӿ�
		�����������ĳ��space�ļ���ӳ��
	*/
	virtual void initSpaceData(Network::Channel* pChannel, MemoryStream& s);
	virtual void setSpaceData(Network::Channel* pChannel, SPACE_ID spaceID, const std::string& key, const std::string& value);
	virtual void delSpaceData(Network::Channel* pChannel, SPACE_ID spaceID, const std::string& key);

	/** ����ӿ�
		����鿴watcher
	*/
	void queryWatcher(Network::Channel* pChannel, MemoryStream& s);

	/** ����ӿ�
		console����ʼprofile
	*/
	void startProfile(Network::Channel* pChannel, KBEngine::MemoryStream& s);
	virtual void startProfile_(Network::Channel* pChannel, std::string profileName, int8 profileType, uint32 timelen);

	/** ����ӿ�
	    ���������߿ͻ��ˣ��㵱ǰ��ȡ��������˭��λ��ͬ��
	*/
	virtual void onControlEntity(Network::Channel* pChannel, int32 entityID, int8 isControlled);

	/** ����ӿ�
		��������������
	*/
	void onAppActiveTickCB(Network::Channel* pChannel);

protected:
	PyBots*													pPyBots_;

	CLIENTS													clients_;

	// console���󴴽�������˵�bots����
	uint32													reqCreateAndLoginTotalCount_;
	uint32													reqCreateAndLoginTickCount_;
	float													reqCreateAndLoginTickTime_;

	// �����������¼��handler
	CreateAndLoginHandler*									pCreateAndLoginHandler_;

	Network::EventPoller*									pEventPoller_;

	TelnetServer*											pTelnetServer_;
};

}

#endif // KBE_BOTS_H
