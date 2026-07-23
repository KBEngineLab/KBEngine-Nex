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


#ifndef KBE_CLIENT_APP_H
#define KBE_CLIENT_APP_H

#include "clientobjectbase.h"
#include "common/common.h"
#include "helper/debug_helper.h"
#include "helper/script_loglevel.h"
#include "xml/xml.h"	
#include "common/singleton.h"
#include "common/smartpointer.h"
#include "common/timer.h"
#include "network/interfaces.h"
#include "network/encryption_filter.h"
#include "network/event_dispatcher.h"
#include "network/network_interface.h"
#include "thread/threadpool.h"
#include "pyscript/script.h"
#include "resmgr/resmgr.h"
	
namespace KBEngine{

namespace Network
{
class Channel;
class TCPPacketSender;
class TCPPacketReceiver;
}

class ClientApp : 
	public Singleton<ClientApp>,
	public ClientObjectBase,
	public TimerHandler, 
	public Network::ChannelTimeOutHandler,
	public Network::ChannelDeregisterHandler
{
public:
	enum TimeOutType
	{
		TIMEOUT_GAME_TICK
	};

	enum C_STATE
	{
		C_STATE_INIT = 0,
		C_STATE_INITLOGINAPP_CHANNEL = 1,
		C_STATE_LOGIN = 2,
		C_STATE_LOGIN_BASEAPP_CHANNEL = 3,
		C_STATE_LOGIN_BASEAPP = 4,
		C_STATE_PLAY = 5,
	};
public:
	ClientApp(Network::EventDispatcher& dispatcher, 
			Network::NetworkInterface& ninterface, 
			COMPONENT_TYPE componentType,
			COMPONENT_ID componentID);

	~ClientApp();

	virtual bool initialize();
	virtual bool initializeBegin();
	virtual bool inInitialize(){ return true; }
	virtual bool initializeEnd();
	virtual void finalise();
	virtual bool run();
	
	virtual void reset(void);

	virtual int processOnce(bool shouldIdle = false);

	virtual bool installPyModules();
	virtual void onInstallPyModules(){};
	virtual bool uninstallPyModules();
	virtual bool uninstallPyScript();
	virtual bool installEntityDef();

	void registerScript(PyTypeObject*);
	int registerPyObjectToScript(const char* attrName, PyObject* pyObj);
	int unregisterPyObjectToScript(const char* attrName);

	PyObjectPtr getEntryScript(){ return entryScript_; }

	const char* name(){return COMPONENT_NAME_EX(componentType_);}
	
	virtual void handleTimeout(TimerHandle, void * pUser);
	virtual void handleGameTick();

	void shutDown();

	bool createAccount(std::string accountName, std::string passwd, std::string datas,
		std::string ip, KBEngine::uint32 port);

	bool login(std::string accountName, std::string passwd, std::string datas,
		std::string ip, KBEngine::uint32 port);

	bool updateChannel(bool loginapp, std::string accountName, std::string passwd, 
								   std::string ip, KBEngine::uint32 port);

	GAME_TIME time() const { return g_kbetime; }
	double gameTimeInSeconds() const;

	Network::EventDispatcher & dispatcher()				{ return dispatcher_; }
	Network::NetworkInterface & networkInterface()		{ return networkInterface_; }

	COMPONENT_ID componentID() const	{ return componentID_; }
	COMPONENT_TYPE componentType() const	{ return componentType_; }
		
	KBEngine::script::Script& getScript(){ return *pScript_; }
	void setScript(KBEngine::script::Script* p){ pScript_ = p; }

	static PyObject* __py_getAppPublish(PyObject* self, PyObject* args);
	static PyObject* __py_getPlayer(PyObject* self, PyObject* args);
	static PyObject* __py_fireEvent(PyObject* self, PyObject* args);

	virtual void onServerClosed();

	/**
		���ýű��������ǰ׺
	*/
	static PyObject* __py_setScriptLogType(PyObject* self, PyObject* args);

	virtual void onChannelTimeOut(Network::Channel * pChannel);
	virtual void onChannelDeregister(Network::Channel * pChannel);

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
	   ��¼�ɹ�
	   @ip: ������ip��ַ
	   @port: �������˿�
	*/
	virtual void onLoginSuccessfully(Network::Channel * pChannel, MemoryStream& s);

	/** ����ӿ�
	   ��¼ʧ�ܻص�
	   @failedcode: ʧ�ܷ����� NETWORK_ERR_SRV_NO_READY:������û��׼����, 
									NETWORK_ERR_SRV_OVERLOAD:���������ع���, 
									NETWORK_ERR_NAME_PASSWORD:�û����������벻��ȷ
	*/
	virtual void onLoginFailed(Network::Channel * pChannel, MemoryStream& s);

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

	virtual void onTargetChanged();

	/** 
		�����������ĳ��space�ļ���ӳ��
	*/
	virtual void onAddSpaceGeometryMapping(SPACE_ID spaceID, std::string& respath);

	static PyObject* __py_GetSpaceData(PyObject *self, PyObject* args)
	{
		return ClientObjectBase::__py_GetSpaceData(&ClientApp::getSingleton(), args);	
	}

	static PyObject* __py_callback(PyObject *self, PyObject* args)
	{
		return ClientObjectBase::__py_callback(&ClientApp::getSingleton(), args);	
	}

	static PyObject* __py_cancelCallback(PyObject *self, PyObject* args)
	{
		return ClientObjectBase::__py_cancelCallback(&ClientApp::getSingleton(), args);	
	}

	static PyObject* __py_getWatcher(PyObject *self, PyObject* args)
	{
		return ClientObjectBase::__py_getWatcher(&ClientApp::getSingleton(), args);	
	}

	static PyObject* __py_getWatcherDir(PyObject *self, PyObject* args)
	{
		return ClientObjectBase::__py_getWatcherDir(&ClientApp::getSingleton(), args);	
	}

	static PyObject* __py_disconnect(PyObject *self, PyObject* args)
	{
		return ClientObjectBase::__py_disconnect(&ClientApp::getSingleton(), args);	
	}

	/**
		ͨ�����·����ȡ��Դ��ȫ·��
	*/
	static PyObject* __py_getResFullPath(PyObject* self, PyObject* args);

	/**
		ͨ�����·���ж���Դ�Ƿ����
	*/
	static PyObject* __py_hasRes(PyObject* self, PyObject* args);

	/**
		open�ļ�
	*/
	static PyObject* __py_kbeOpen(PyObject* self, PyObject* args);

	/**
		�г�Ŀ¼�������ļ�
	*/
	static PyObject* __py_listPathRes(PyObject* self, PyObject* args);

	/**
		ƥ�����·�����ȫ·��
	*/
	static PyObject* __py_matchPath(PyObject* self, PyObject* args);

protected:
	KBEngine::script::Script*								pScript_;
	std::vector<PyTypeObject*>								scriptBaseTypes_;

	TimerHandle												gameTimer_;

	COMPONENT_TYPE											componentType_;

	// �������ID
	COMPONENT_ID											componentID_;									

	Network::EventDispatcher& 								dispatcher_;
	Network::NetworkInterface&								networkInterface_;
	
	Network::TCPPacketSender*								pTCPPacketSender_;
	Network::TCPPacketReceiver*								pTCPPacketReceiver_;
	Network::BlowfishFilter*								pBlowfishFilter_;

	// �̳߳�
	thread::ThreadPool										threadPool_;

	PyObjectPtr												entryScript_;

	C_STATE													state_;
};

}
#endif
