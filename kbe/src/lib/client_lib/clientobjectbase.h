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


#ifndef CLIENT_OBJECT_BASE_H
#define CLIENT_OBJECT_BASE_H

#include "event.h"
#include "script_callbacks.h"
#include "common/common.h"
#include "common/memorystream.h"
#include "helper/debug_helper.h"
#include "helper/script_loglevel.h"
#include "pyscript/scriptobject.h"
#include "entitydef/entities.h"
#include "entitydef/common.h"
#include "server/callbackmgr.h"
#include "server/server_errors.h"
#include "math/math.h"

namespace KBEngine{

namespace client{
class Entity;
}

class EntityCall;

namespace Network
{
class Channel;
}

class ClientObjectBase : public script::ScriptObject
{
	/** 
		���໯ ��һЩpy�������������� 
	*/
	INSTANCE_SCRIPT_HREADER(ClientObjectBase, ScriptObject)	
public:
	ClientObjectBase(Network::NetworkInterface& ninterface, PyTypeObject* pyType = NULL);
	virtual ~ClientObjectBase();

	Network::Channel* pServerChannel() const{ return pServerChannel_; }
	void pServerChannel(Network::Channel* pChannel){ pServerChannel_ = pChannel; }

	virtual void finalise(void);
	virtual void reset(void);
	virtual void canReset(bool v){ canReset_ = v; }

	Entities<client::Entity>* pEntities() const{ return pEntities_; }

	/**
		����һ��entity 
	*/
	client::Entity* createEntity(const char* entityType, PyObject* params,
		bool isInitializeScript = true, ENTITY_ID eid = 0, bool initProperty = true, 
		EntityCall* base = NULL, EntityCall* cell = NULL);

	PY_CALLBACKMGR& callbackMgr(){ return pyCallbackMgr_; }	

	/**
		ͨ��entityID����һ��entity 
	*/
	virtual bool destroyEntity(ENTITY_ID entityID, bool callScript);

	void tickSend();
	
	virtual Network::Channel* initLoginappChannel(std::string accountName, 
		std::string passwd, std::string ip, KBEngine::uint32 port);
	virtual Network::Channel* initBaseappChannel();

	bool createAccount();
	bool login();
	virtual void onLogin(Network::Bundle* pBundle);

	bool loginBaseapp();
	bool reloginBaseapp();

	int32 appID() const{ return appID_; }
	const char* name(){ return name_.c_str(); }

	ENTITY_ID entityID(){ return entityID_; }
	DBID dbid(){ return dbid_; }

	bool registerEventHandle(EventHandle* pEventHandle);
	bool deregisterEventHandle(EventHandle* pEventHandle);
	
	void fireEvent(const EventData* pEventData);
	
	EventHandler& eventHandler(){ return eventHandler_; }

	static PyObject* __pyget_pyGetEntities(PyObject *self, void *closure)
	{
		ClientObjectBase* pClientObjectBase = static_cast<ClientObjectBase*>(self);
		Py_INCREF(pClientObjectBase->pEntities());
		return pClientObjectBase->pEntities(); 
	}

	static PyObject* __pyget_pyGetID(PyObject *self, void *closure){
		
		ClientObjectBase* pClientObjectBase = static_cast<ClientObjectBase*>(self);
		return PyLong_FromLong(pClientObjectBase->appID());	
	}

	static PyObject* __py_getPlayer(PyObject *self, void *args);
	
	static PyObject* __py_callback(PyObject* self, PyObject* args);
	static PyObject* __py_cancelCallback(PyObject* self, PyObject* args);

	static PyObject* __py_getWatcher(PyObject* self, PyObject* args);
	static PyObject* __py_getWatcherDir(PyObject* self, PyObject* args);

	static PyObject* __py_disconnect(PyObject* self, PyObject* args);

	/**
		���entitiessizeС��256
		ͨ������λ������ȡentityID
		����ֱ��ȡID
	*/
	ENTITY_ID readEntityIDFromStream(MemoryStream& s);

	/**
		��entitycall�����Ի�ȡһ��channel��ʵ��
	*/
	virtual Network::Channel* findChannelByEntityCall(EntityCall& entitycall);

	/** ����ӿ�
		�ͻ��������˵�һ�ν�������, ����˷���
	*/
	virtual void onHelloCB_(Network::Channel* pChannel, const std::string& verInfo,
		const std::string& scriptVerInfo, const std::string& protocolMD5, 
		const std::string& entityDefMD5, COMPONENT_TYPE componentType);

	virtual void onHelloCB(Network::Channel* pChannel, MemoryStream& s);

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
	virtual void onReloginBaseappFailed(Network::Channel * pChannel, SERVER_ERROR_CODE failedcode);

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
	void onRemoteMethodCall_(ENTITY_ID eid, MemoryStream& s);

	/** ����ӿ�
	   ���߳�������
	*/
	virtual void onKicked(Network::Channel * pChannel, SERVER_ERROR_CODE failedcode);

	/** ����ӿ�
		����������entity����
	*/
	virtual void onUpdatePropertys(Network::Channel* pChannel, MemoryStream& s);
	virtual void onUpdatePropertysOptimized(Network::Channel* pChannel, MemoryStream& s);
	void onUpdatePropertys_(ENTITY_ID eid, MemoryStream& s);

	/** ����ӿ�
		������ǿ������entity��λ���볯��
	*/
	virtual void onSetEntityPosAndDir(Network::Channel* pChannel, MemoryStream& s);

	/** ����ӿ�
		����������avatar����λ�úͳ���
	*/
	virtual void onUpdateBasePos(Network::Channel* pChannel, float x, float y, float z);
	virtual void onUpdateBasePosXZ(Network::Channel* pChannel, float x, float z);
	virtual void onUpdateBaseDir(Network::Channel* pChannel, MemoryStream& s);

	/** ����ӿ�
		����������VolatileData
	*/
	virtual void onUpdateData(Network::Channel* pChannel, MemoryStream& s);

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
	
	void _updateVolatileData(ENTITY_ID entityID, float x, float y, float z, float roll, 
		float pitch, float yaw, int8 isOnGround, bool isOptimized);

	/** 
		������ҵ������ 
	*/
	virtual void updatePlayerToServer();

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
		���������߿ͻ��ˣ��㵱ǰ��ȡ��������˭��λ��ͬ��
	*/
	virtual void onControlEntity(Network::Channel* pChannel, int32 eid, int8 p_isControlled);

	/** ����ӿ�
		���յ�ClientMessages(ͨ����web�ȲŻ�Ӧ�õ�)
	*/
	virtual void onImportClientMessages(Network::Channel* pChannel, MemoryStream& s){}

	/** ����ӿ�
		���յ�entitydef(ͨ����web�ȲŻ�Ӧ�õ�)
	*/
	virtual void onImportClientEntityDef(Network::Channel* pChannel, MemoryStream& s){}
	
	/** ����ӿ�
		��������������(ͨ����web�ȲŻ�Ӧ�õ�)
	*/
	virtual void onImportServerErrorsDescr(Network::Channel* pChannel, MemoryStream& s){}

	/** ����ӿ�
	���յ���sdk��Ϣ(ͨ���ǿ�����ʹ�ã����¿ͻ���sdk��)
	*/
	virtual void onImportClientSDK(Network::Channel* pChannel, MemoryStream& s) {}

	/** ����ӿ�
		�����˺��������󷵻�
	*/
	virtual void onReqAccountResetPasswordCB(Network::Channel* pChannel, SERVER_ERROR_CODE failedcode){}

	/** ����ӿ�
		��������䷵��
	*/
	virtual void onReqAccountBindEmailCB(Network::Channel* pChannel, SERVER_ERROR_CODE failedcode){}

	/** ����ӿ�
		�����޸����뷵��
	*/
	virtual void onReqAccountNewPasswordCB(Network::Channel* pChannel, SERVER_ERROR_CODE failedcode){}

	/** 
		���playerʵ��
	*/
	client::Entity* pPlayer();

	void setTargetID(ENTITY_ID id){ 
		targetID_ = id; 
		onTargetChanged();
	}
	ENTITY_ID getTargetID() const{ return targetID_; }
	virtual void onTargetChanged(){}

	ENTITY_ID getViewEntityID(ENTITY_ID id);
	ENTITY_ID getViewEntityIDFromStream(MemoryStream& s);
	ENTITY_ID getViewEntityIDByAliasID(uint8 id);

	/** 
		space��ز����ӿ�
		�����������ĳ��space�ļ���ӳ��
	*/
	virtual void addSpaceGeometryMapping(SPACE_ID spaceID, const std::string& respath);
	virtual void onAddSpaceGeometryMapping(SPACE_ID spaceID, const std::string& respath){}
	virtual void onLoadedSpaceGeometryMapping(SPACE_ID spaceID){
		isLoadedGeometry_ = true;
	}

	const std::string& getGeometryPath();
	
	virtual void initSpaceData(Network::Channel* pChannel, MemoryStream& s);
	virtual void setSpaceData(Network::Channel* pChannel, SPACE_ID spaceID, const std::string& key, const std::string& value);
	virtual void delSpaceData(Network::Channel* pChannel, SPACE_ID spaceID, const std::string& key);
	bool hasSpaceData(const std::string& key);
	const std::string& getSpaceData(const std::string& key);
	static PyObject* __py_GetSpaceData(PyObject* self, PyObject* args);
	void clearSpace(bool isAll);

	Timers & timers() { return timers_; }
	void handleTimers();

	ScriptCallbacks & scriptCallbacks() { return scriptCallbacks_; }

	void locktime(uint64 t){ locktime_ = t; }
	uint64 locktime() const{ return locktime_; }

	virtual void onServerClosed();

	uint64 rndUUID() const{ return rndUUID_; }

	Network::NetworkInterface* pNetworkInterface()const { return &networkInterface_; }

	/** ����ӿ�
		��������������
	*/
	void onAppActiveTickCB(Network::Channel* pChannel);

	/**
		�����ű�assert�ײ�
	*/
	static PyObject* __py_assert(PyObject* self, PyObject* args);

protected:				
	int32													appID_;

	// ���������ͨ��
	Network::Channel*										pServerChannel_;

	// �洢���е�entity������
	Entities<client::Entity>*								pEntities_;	
	std::vector<ENTITY_ID>									pEntityIDAliasIDList_;

	PY_CALLBACKMGR											pyCallbackMgr_;

	ENTITY_ID												entityID_;
	SPACE_ID												spaceID_;

	DBID													dbid_;

	std::string												ip_;
	uint16													port_;

	std::string												baseappIP_;
	uint16													baseappPort_;

	uint64													lastSentActiveTickTime_;
	uint64													lastSentUpdateDataTime_;

	bool													connectedBaseapp_;
	bool													canReset_;

	std::string												name_;
	std::string												password_;

	std::string												clientDatas_;
	std::string												serverDatas_;

	CLIENT_CTYPE											typeClient_;

	typedef std::map<ENTITY_ID, KBEShared_ptr<MemoryStream> > BUFFEREDMESSAGE;
	BUFFEREDMESSAGE											bufferedCreateEntityMessage_;

	EventHandler											eventHandler_;

	Network::NetworkInterface&								networkInterface_;

	// ��ǰ�ͻ�����ѡ���Ŀ��
	ENTITY_ID												targetID_;

	// �Ƿ���ع���������
	bool													isLoadedGeometry_;

	SPACE_DATA												spacedatas_;

	Timers													timers_;
	ScriptCallbacks											scriptCallbacks_;

	uint64													locktime_;
	
	// �����ص�½����ʱ��key
	uint64													rndUUID_; 

    // �ܱ��ͻ��˿��Ƶ�entity�б�
    std::list<client::Entity *>                             controlledEntities_;
};



}
#endif
