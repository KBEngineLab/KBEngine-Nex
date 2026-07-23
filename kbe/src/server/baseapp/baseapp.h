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


#ifndef KBE_BASEAPP_H
#define KBE_BASEAPP_H
	
// common include	
#include "entity.h"
#include "proxy.h"
#include "profile.h"
#include "server/entity_app.h"
#include "server/pendingLoginmgr.h"
#include "server/forward_messagebuffer.h"
#include "network/endpoint.h"

//#define NDEBUG
// windows include	
#if KBE_PLATFORM == PLATFORM_WIN32
#else
// linux include
#endif
	
namespace KBEngine{

namespace Network{
	class Channel;
	class Bundle;
}

class Proxy;
class Backuper;
class Archiver;
class TelnetServer;
class RestoreEntityHandler;
class InitProgressHandler;

class Baseapp :	public EntityApp<Entity>, 
				public Singleton<Baseapp>
{
public:
	enum TimeOutType
	{
		TIMEOUT_CHECK_STATUS = TIMEOUT_ENTITYAPP_MAX + 1,
		TIMEOUT_MAX
	};
	
	Baseapp(Network::EventDispatcher& dispatcher, 
		Network::NetworkInterface& ninterface, 
		COMPONENT_TYPE componentType,
		COMPONENT_ID componentID);

	~Baseapp();
	
	virtual bool installPyModules();
	virtual void onInstallPyModules();
	virtual bool uninstallPyModules();

	bool run();
	
	/** 
		��ش����ӿ� 
	*/
	virtual void handleTimeout(TimerHandle handle, void * arg);
	virtual void handleGameTick();
	void handleCheckStatusTick();
	void handleBackup();
	void handleArchive();

	/** 
		��ʼ����ؽӿ� 
	*/
	bool initialize();
	bool initializeBegin();
	bool initializeEnd();
	void finalise();
	
	virtual ShutdownHandler::CAN_SHUTDOWN_STATE canShutdown();
	virtual void onShutdownBegin();
	virtual void onShutdown(bool first);
	virtual void onShutdownEnd();

	virtual bool initializeWatcher();

	static PyObject* __py_quantumPassedPercent(PyObject* self, PyObject* args);
	float _getLoad() const { return getLoad(); }
	virtual void onUpdateLoad();

	virtual void onChannelDeregister(Network::Channel * pChannel);

	/**
		һ��cellapp����
	*/
	void onCellAppDeath(Network::Channel * pChannel);

	/** ����ӿ�
		dbmgr��֪�Ѿ�����������baseapp����cellapp�ĵ�ַ
		��ǰapp��Ҫ������ȥ�����ǽ�������
	*/
	virtual void onGetEntityAppFromDbmgr(Network::Channel* pChannel, 
							int32 uid, 
							std::string& username, 
							COMPONENT_TYPE componentType, COMPONENT_ID componentID, COMPONENT_ORDER globalorderID, COMPONENT_ORDER grouporderID,
							uint32 intaddr, uint16 intport, uint32 extaddr, uint16 extport, std::string& extaddrEx);
	
	/** ����ӿ�
		ĳ��client��app��֪���ڻ״̬��
	*/
	void onClientActiveTick(Network::Channel* pChannel);

	/** ����ӿ�
		���ݿ��в�ѯ���Զ�entity������Ϣ����
	*/
	void onEntityAutoLoadCBFromDBMgr(Network::Channel* pChannel, MemoryStream& s);

	/** 
		������һ��entity�ص�
	*/
	virtual Entity* onCreateEntity(PyObject* pyEntity, ScriptDefModule* sm, ENTITY_ID eid);

	/** 
		����һ��entity 
	*/
	static PyObject* __py_createEntity(PyObject* self, PyObject* args);
	static PyObject* __py_createEntityAnywhere(PyObject* self, PyObject* args);
	static PyObject* __py_createEntityRemotely(PyObject* self, PyObject* args);
	static PyObject* __py_createEntityFromDBID(PyObject* self, PyObject* args);
	static PyObject* __py_createEntityAnywhereFromDBID(PyObject* self, PyObject* args);
	static PyObject* __py_createEntityRemotelyFromDBID(PyObject* self, PyObject* args);
	
	/**
		����һ���µ�space 
	*/
	void createCellEntityInNewSpace(Entity* pEntity, PyObject* pyCellappIndex);

	/**
		�ָ�һ��space 
	*/
	void restoreSpaceInCell(Entity* pEntity);

	/** 
		��һ�����ؽϵ͵�baseapp�ϴ���һ��baseEntity 
	*/
	void createEntityAnywhere(const char* entityType, PyObject* params, PyObject* pyCallback);

	/** �յ�baseappmgr������ĳ��baseappҪ��createEntityAnywhere�������ڱ�baseapp��ִ�� 
		@param entityType	: entity����� entities.xml�еĶ���ġ�
		@param strInitData	: ���entity��������Ӧ�ø�����ʼ����һЩ���ݣ� ��Ҫʹ��pickle.loads���.
		@param componentID	: ���󴴽�entity��baseapp�����ID
	*/
	void onCreateEntityAnywhere(Network::Channel* pChannel, MemoryStream& s);

	/**
	baseapp ��createEntityAnywhere�Ļص�
	*/
	void onCreateEntityAnywhereCallback(Network::Channel* pChannel, KBEngine::MemoryStream& s);
	void _onCreateEntityAnywhereCallback(Network::Channel* pChannel, CALLBACK_ID callbackID,
		std::string& entityType, ENTITY_ID eid, COMPONENT_ID componentID);

	/**
	��һ�����ؽϵ͵�baseapp�ϴ���һ��baseEntity
	*/
	void createEntityRemotely(const char* entityType, COMPONENT_ID componentID, PyObject* params, PyObject* pyCallback);

	/** �յ�baseappmgr������ĳ��baseappҪ��createEntityAnywhere�������ڱ�baseapp��ִ��
	@param entityType	: entity����� entities.xml�еĶ���ġ�
	@param strInitData	: ���entity��������Ӧ�ø�����ʼ����һЩ���ݣ� ��Ҫʹ��pickle.loads���.
	@param componentID	: ���󴴽�entity��baseapp�����ID
	*/
	void onCreateEntityRemotely(Network::Channel* pChannel, MemoryStream& s);

	/**
	baseapp ��createEntityAnywhere�Ļص�
	*/
	void onCreateEntityRemotelyCallback(Network::Channel* pChannel, KBEngine::MemoryStream& s);
	void _onCreateEntityRemotelyCallback(Network::Channel* pChannel, CALLBACK_ID callbackID,
		std::string& entityType, ENTITY_ID eid, COMPONENT_ID componentID);

	/** 
		��db��ȡ��Ϣ����һ��entity
	*/
	void createEntityFromDBID(const char* entityType, DBID dbid, PyObject* pyCallback, const std::string& dbInterfaceName);

	/** ����ӿ�
		createEntityFromDBID�Ļص���
	*/
	void onCreateEntityFromDBIDCallback(Network::Channel* pChannel, KBEngine::MemoryStream& s);

	/** 
		��db��ȡ��Ϣ����һ��entity
	*/
	void createEntityAnywhereFromDBID(const char* entityType, DBID dbid, PyObject* pyCallback, const std::string& dbInterfaceName);

	/** ����ӿ�
		createEntityAnywhereFromDBID�Ļص���
	*/
	// ��baseappmgr��ѯ���ڴ���ʵ������id�ص�
	void onGetCreateEntityAnywhereFromDBIDBestBaseappID(Network::Channel* pChannel, KBEngine::MemoryStream& s);

	/** ����ӿ�
		createEntityAnywhereFromDBID�Ļص���
	*/
	// �����ݿ����Ļص�
	void onCreateEntityAnywhereFromDBIDCallback(Network::Channel* pChannel, KBEngine::MemoryStream& s);

	// ��������������ϴ������entity
	void createEntityAnywhereFromDBIDOtherBaseapp(Network::Channel* pChannel, KBEngine::MemoryStream& s);

	// ������Ϻ�Ļص�
	void onCreateEntityAnywhereFromDBIDOtherBaseappCallback(Network::Channel* pChannel, COMPONENT_ID createByBaseappID, 
							std::string entityType, ENTITY_ID createdEntityID, CALLBACK_ID callbackID, DBID dbid);
	
	/**
	��db��ȡ��Ϣ����һ��entity
	*/
	void createEntityRemotelyFromDBID(const char* entityType, DBID dbid, COMPONENT_ID createToComponentID, 
		PyObject* pyCallback, const std::string& dbInterfaceName);

	/** ����ӿ�
	createEntityRemotelyFromDBID�Ļص���
	*/
	// �����ݿ����Ļص�
	void onCreateEntityRemotelyFromDBIDCallback(Network::Channel* pChannel, KBEngine::MemoryStream& s);

	// ��������������ϴ������entity
	void createEntityRemotelyFromDBIDOtherBaseapp(Network::Channel* pChannel, KBEngine::MemoryStream& s);

	// ������Ϻ�Ļص�
	void onCreateEntityRemotelyFromDBIDOtherBaseappCallback(Network::Channel* pChannel, COMPONENT_ID createByBaseappID,
		std::string entityType, ENTITY_ID createdEntityID, CALLBACK_ID callbackID, DBID dbid);

	/** 
		Ϊһ��baseEntity��ָ����cell�ϴ���һ��cellEntity 
	*/
	void createCellEntity(EntityCallAbstract* createToCellEntityCall, Entity* pEntity);
	
	/** ����ӿ�
		createCellEntityʧ�ܵĻص���
	*/
	void onCreateCellFailure(Network::Channel* pChannel, ENTITY_ID entityID);

	/** ����ӿ�
		createCellEntity��cellʵ�崴���ɹ��ص���
	*/
	void onEntityGetCell(Network::Channel* pChannel, ENTITY_ID id, COMPONENT_ID componentID, SPACE_ID spaceID);

	/** 
		֪ͨ�ͻ��˴���һ��proxy��Ӧ��ʵ�� 
	*/
	bool createClientProxies(Proxy* pEntity, bool reload = false);

	/** 
		��dbmgr����ִ��һ�����ݿ�����
	*/
	static PyObject* __py_executeRawDatabaseCommand(PyObject* self, PyObject* args);
	void executeRawDatabaseCommand(const char* datas, uint32 size, PyObject* pycallback, ENTITY_ID eid, const std::string& dbInterfaceName);
	void onExecuteRawDatabaseCommandCB(Network::Channel* pChannel, KBEngine::MemoryStream& s);

	/** ����ӿ�
		dbmgr���ͳ�ʼ��Ϣ
		startID: ��ʼ����ENTITY_ID ����ʼλ��
		endID: ��ʼ����ENTITY_ID �ν���λ��
		startGlobalOrder: ȫ������˳�� �������ֲ�ͬ���
		startGroupOrder: ��������˳�� ����������baseapp�еڼ���������
		machineGroupOrder: ��machine����ʵ����˳��, �ṩ�ײ���ĳЩʱ���ж��Ƿ�Ϊ��һ��baseappʱʹ��
	*/
	void onDbmgrInitCompleted(Network::Channel* pChannel, 
		GAME_TIME gametime, ENTITY_ID startID, ENTITY_ID endID, COMPONENT_ORDER startGlobalOrder, 
		COMPONENT_ORDER startGroupOrder, const std::string& digest);

	/** ����ӿ�
		dbmgr�㲥global���ݵĸı�
	*/
	void onBroadcastBaseAppDataChanged(Network::Channel* pChannel, KBEngine::MemoryStream& s);

	/** ����ӿ�
		ע�ὫҪ��¼���˺�, ע�����������¼��������
	*/
	void registerPendingLogin(Network::Channel* pChannel, KBEngine::MemoryStream& s);

	/** ����ӿ�
		���û������¼��������
	*/
	void loginBaseapp(Network::Channel* pChannel, std::string& accountName, std::string& password);

	/** ����ӿ�
		�������ϵǳ��������Ͽ����Ӳ�����ʵ���onClientDead��ʵ���������û�����
	*/
	void logoutBaseapp(Network::Channel* pChannel, uint64 key, ENTITY_ID entityID);

	/**
		�߳�һ��Channel
	*/
	void kickChannel(Network::Channel* pChannel, SERVER_ERROR_CODE failedcode);

	/** ����ӿ�
		���µ�¼ ���������ؽ���������ϵ(ǰ����֮ǰ�Ѿ���¼�ˣ� 
		֮��Ͽ��ڷ������ж���ǰ�˵�Entityδ��ʱ���ٵ�ǰ���¿��Կ�����������������Ӳ��ﵽ�ٿظ�entity��Ŀ��)
	*/
	void reloginBaseapp(Network::Channel* pChannel, std::string& accountName, 
		std::string& password, uint64 key, ENTITY_ID entityID);

	/**
	   ��¼ʧ��
	   @failedcode: ʧ�ܷ����� NETWORK_ERR_SRV_NO_READY:������û��׼����, 
									NETWORK_ERR_ILLEGAL_LOGIN:�Ƿ���¼, 
									NETWORK_ERR_NAME_PASSWORD:�û����������벻��ȷ
	*/
	void loginBaseappFailed(Network::Channel* pChannel, std::string& accountName, 
		SERVER_ERROR_CODE failedcode, bool relogin = false);

	/** ����ӿ�
		��dbmgr��ȡ���˺�Entity��Ϣ
	*/
	void onQueryAccountCBFromDbmgr(Network::Channel* pChannel, KBEngine::MemoryStream& s);
	
	/**
		�ͻ�����������������
	*/
	void onClientEntityEnterWorld(Proxy* pEntity, COMPONENT_ID componentID);

	/** ����ӿ�
		entity�յ�Զ��call����, ��ĳ��app�ϵ�entityCall����(ֻ����������ڲ�ʹ�ã� �ͻ��˵�entitycall���÷�����
		onRemoteCellMethodCallFromClient)
	*/
	void onEntityCall(Network::Channel* pChannel, KBEngine::MemoryStream& s);
	
	/** ����ӿ�
		client����entity��cell����
	*/
	void onRemoteCallCellMethodFromClient(Network::Channel* pChannel, KBEngine::MemoryStream& s);

	/** ����ӿ�
		client��������
	*/
	void onUpdateDataFromClient(Network::Channel* pChannel, KBEngine::MemoryStream& s);
	void onUpdateDataFromClientForControlledEntity(Network::Channel* pChannel, KBEngine::MemoryStream& s);


	/** ����ӿ�
		cellapp����entity��cell����
	*/
	void onBackupEntityCellData(Network::Channel* pChannel, KBEngine::MemoryStream& s);

	/** ����ӿ�
		cellapp writeToDB���
	*/
	void onCellWriteToDBCompleted(Network::Channel* pChannel, KBEngine::MemoryStream& s);

	/** ����ӿ�
		cellappת��entity��Ϣ��client
	*/
	void forwardMessageToClientFromCellapp(Network::Channel* pChannel, KBEngine::MemoryStream& s);

	/** ����ӿ�
		cellappת��entity��Ϣ��ĳ��baseEntity��cellEntity
	*/
	void forwardMessageToCellappFromCellapp(Network::Channel* pChannel, KBEngine::MemoryStream& s);
	
	/**
		��ȡ��Ϸʱ��
	*/
	static PyObject* __py_gametime(PyObject* self, PyObject* args);

	/** ����ӿ�
		дentity��db�ص�
	*/
	void onWriteToDBCallback(Network::Channel* pChannel, ENTITY_ID eid, DBID entityDBID, 
		uint16 dbInterfaceIndex, CALLBACK_ID callbackID, bool success);

	/**
		����proxices����
	*/
	void incProxicesCount() { ++numProxices_; }

	/**
		����proxices����
	*/
	void decProxicesCount() { --numProxices_; }

	/**
		���proxices����
	*/
	int32 numProxices() const { return numProxices_; }

	/**
		���numClients����
	*/
	int32 numClients() { return this->networkInterface().numExtChannels(); }
	
	/** 
		�����ֵ
	*/
	static PyObject* __py_charge(PyObject* self, PyObject* args);
	void charge(std::string chargeID, DBID dbid, const std::string& datas, PyObject* pycallback);
	void onChargeCB(Network::Channel* pChannel, KBEngine::MemoryStream& s);

	/**
		hook entitycall call
	*/
	RemoteEntityMethod* createEntityCallCallEntityRemoteMethod(MethodDescription* pMethodDescription, EntityCall* pEntityCall);

	virtual void onHello(Network::Channel* pChannel, 
		const std::string& verInfo, 
		const std::string& scriptVerInfo, 
		const std::string& encryptedKey);

	// ����汾��ƥ��
	virtual void onVersionNotMatch(Network::Channel* pChannel);

	// ����ű���汾��ƥ��
	virtual void onScriptVersionNotMatch(Network::Channel* pChannel);

	/** ����ӿ�
		����������APP���ѻָ����ؽ��
	*/
	void onRequestRestoreCB(Network::Channel* pChannel, KBEngine::MemoryStream& s);

	/**
		һ��cell��entity���ָ����
	*/
	void onRestoreEntitiesOver(RestoreEntityHandler* pRestoreEntityHandler);

	/** ����ӿ�
		ĳ��baseapp�ϵ�space�ָ���cell�� �жϵ�ǰbaseapp�Ƿ������entity��Ҫ�ָ�cell
	*/
	void onRestoreSpaceCellFromOtherBaseapp(Network::Channel* pChannel, KBEngine::MemoryStream& s);

	/** ����ӿ�
		ĳ��app����鿴��app
	*/
	virtual void lookApp(Network::Channel* pChannel);

	/** ����ӿ�
		�ͻ���Э�鵼��
	*/
	void importClientMessages(Network::Channel* pChannel);

	/** ����ӿ�
		�ͻ���entitydef����
	*/
	void importClientEntityDef(Network::Channel* pChannel);

	/**
		���µ������еĽű�
	*/
	static PyObject* __py_reloadScript(PyObject* self, PyObject* args);
	virtual void reloadScript(bool fullReload);
	virtual void onReloadScript(bool fullReload);

	/**
		��ȡ�����Ƿ����ڹر���
	*/
	static PyObject* __py_isShuttingDown(PyObject* self, PyObject* args);

	/**
		��ȡ�����ڲ������ַ
	*/
	static PyObject* __py_address(PyObject* self, PyObject* args);

	/**
		ͨ��dbid�����ݿ���ɾ��һ��ʵ��

		�����ݿ�ɾ��ʵ�壬 ���ʵ�岻���������ֱ��ɾ���ص�����true�� ���������ص����ص���entity��entitycall�� �����κ�ԭ�򶼷���false.
	*/
	static PyObject* __py_deleteEntityByDBID(PyObject* self, PyObject* args);

	/** ����ӿ�
		ͨ��dbid�����ݿ���ɾ��һ��ʵ��Ļص�
	*/
	void deleteEntityByDBIDCB(Network::Channel* pChannel, KBEngine::MemoryStream& s);

	/**
		ͨ��dbid��ѯһ��ʵ���Ƿ�����ݿ���

		���ʵ�����߻ص�����baseentitycall�����ʵ�岻������ص�����true�������κ�ԭ�򶼷���false.
	*/
	static PyObject* __py_lookUpEntityByDBID(PyObject* self, PyObject* args);

	/** ����ӿ�
		���ʵ�����߻ص�����baseentitycall�����ʵ�岻������ص�����true�������κ�ԭ�򶼷���false.
	*/
	void lookUpEntityByDBIDCB(Network::Channel* pChannel, KBEngine::MemoryStream& s);

	/** ����ӿ�
		�����email
	*/
	void reqAccountBindEmail(Network::Channel* pChannel, ENTITY_ID entityID, std::string& password, std::string& email);

	/** ����ӿ�
		�����email, dbmgr���ؽ��
	*/
	void onReqAccountBindEmailCBFromDBMgr(Network::Channel* pChannel, ENTITY_ID entityID, std::string& accountName, std::string& email,
		SERVER_ERROR_CODE failedcode, std::string& code);

	/** ����ӿ�
		�����email, baseappmgr������Ҫ�ҵ�loginapp�ĵ�ַ
	*/
	void onReqAccountBindEmailCBFromBaseappmgr(Network::Channel* pChannel, ENTITY_ID entityID, std::string& accountName, std::string& email,
		SERVER_ERROR_CODE failedcode, std::string& code, std::string& loginappCBHost, uint16 loginappCBPort);

	/** ����ӿ�
		�����email
	*/
	void reqAccountNewPassword(Network::Channel* pChannel, ENTITY_ID entityID, std::string& oldpassworld, std::string& newpassword);

	void onReqAccountNewPasswordCB(Network::Channel* pChannel, ENTITY_ID entityID, std::string& accountName,
		SERVER_ERROR_CODE failedcode);

	uint32 flags() const { return flags_; }
	void flags(uint32 v) { flags_ = v; }
	static PyObject* __py_setFlags(PyObject* self, PyObject* args);
	static PyObject* __py_getFlags(PyObject* self, PyObject* args);
	
protected:
	TimerHandle												loopCheckTimerHandle_;

	// globalBases
	GlobalDataClient*										pBaseAppData_;

	// ��¼��¼������������δ������ϵ��˺�
	PendingLoginMgr											pendingLoginMgr_;

	ForwardComponent_MessageBuffer							forward_messagebuffer_;

	// ���ݴ浵���
	KBEShared_ptr< Backuper >								pBackuper_;	
	KBEShared_ptr< Archiver >								pArchiver_;	

	int32													numProxices_;

	TelnetServer*											pTelnetServer_;

	std::vector< KBEShared_ptr< RestoreEntityHandler > >	pRestoreEntityHandlers_;

	TimerHandle												pResmgrTimerHandle_;

	InitProgressHandler*									pInitProgressHandler_;
	
	// APP�ı�־
	uint32													flags_;

	// ���ڿͻ��˶�̬����entitydefЭ��
	Network::Bundle*										pBundleImportEntityDefDatas_;
};

}

#endif // KBE_BASEAPP_H
