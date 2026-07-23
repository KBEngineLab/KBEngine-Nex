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


#ifndef KBE_SERVER_APP_H
#define KBE_SERVER_APP_H

#include "common/common.h"
#if KBE_PLATFORM == PLATFORM_WIN32
#pragma warning (disable : 4996)
#endif

#include <stdarg.h> 
#include "helper/debug_helper.h"
#include "helper/watcher.h"
#include "helper/profile.h"
#include "helper/profiler.h"
#include "helper/profile_handler.h"
#include "xml/xml.h"	
#include "server/common.h"
#include "server/components.h"
#include "server/serverconfig.h"
#include "server/signal_handler.h"
#include "server/shutdown_handler.h"
#include "common/smartpointer.h"
#include "common/timer.h"
#include "common/singleton.h"
#include "network/interfaces.h"
#include "network/event_dispatcher.h"
#include "network/network_interface.h"
#include "thread/threadpool.h"

	
namespace KBEngine{

namespace Network
{

class Channel;
}

class Shutdowner;
class ComponentActiveReportHandler;

class ServerApp : 
	public SignalHandler, 
	public TimerHandler, 
	public ShutdownHandler,
	public Network::ChannelTimeOutHandler,
	public Network::ChannelDeregisterHandler,
	public Components::ComponentsNotificationHandler
{
public:
	enum TimeOutType
	{
		TIMEOUT_SERVERAPP_MAX
	};

public:
	ServerApp(Network::EventDispatcher& dispatcher, 
			Network::NetworkInterface& ninterface, 
			COMPONENT_TYPE componentType,
			COMPONENT_ID componentID);

	~ServerApp();

	virtual bool initialize();
	virtual bool initializeBegin() { return true; }
	virtual bool inInitialize(){ return true; }
	virtual bool initializeEnd() {return true; }
	virtual void finalise();
	virtual bool run();
	
	virtual bool initThreadPool();

	virtual bool installSignals();

	virtual bool initializeWatcher();

	virtual bool loadConfig();
	const char* name(){return COMPONENT_NAME_EX(componentType_);}
	
	virtual void handleTimeout(TimerHandle, void * pUser);

	GAME_TIME time() const { return g_kbetime; }
	Timers & timers() { return timers_; }
	double gameTimeInSeconds() const;
	void handleTimers();

	thread::ThreadPool& threadPool() { return threadPool_; }

	Network::EventDispatcher & dispatcher()				{ return dispatcher_; }
	Network::NetworkInterface & networkInterface()			{ return networkInterface_; }

	COMPONENT_ID componentID() const	{ return componentID_; }
	COMPONENT_TYPE componentType() const	{ return componentType_; }
		
	virtual void onSignalled(int sigNum);
	virtual void onChannelTimeOut(Network::Channel * pChannel);
	virtual void onChannelDeregister(Network::Channel * pChannel);
	virtual void onAddComponent(const Components::ComponentInfos* pInfos);
	virtual void onRemoveComponent(const Components::ComponentInfos* pInfos);
	virtual void onIdentityillegal(COMPONENT_TYPE componentType, COMPONENT_ID componentID, uint32 pid, const char* pAddr);

	virtual void onShutdownBegin();
	virtual void onShutdown(bool first);
	virtual void onShutdownEnd();

	/** ����ӿ�
		����鿴watcher
	*/
	void queryWatcher(Network::Channel* pChannel, MemoryStream& s);

	void shutDown(float shutdowntime = -FLT_MAX);

	COMPONENT_ORDER globalOrder() const { return startGlobalOrder_; }
	COMPONENT_ORDER groupOrder() const { return startGroupOrder_; }

	/** ����ӿ�
		ע��һ���¼����baseapp����cellapp����dbmgr
		ͨ����һ���µ�app�������ˣ� ����Ҫ��ĳЩ���ע���Լ���
	*/
	virtual void onRegisterNewApp(Network::Channel* pChannel, 
							int32 uid, 
							std::string& username, 
							COMPONENT_TYPE componentType, COMPONENT_ID componentID, COMPONENT_ORDER globalorderID, COMPONENT_ORDER grouporderID,
							uint32 intaddr, uint16 intport, uint32 extaddr, uint16 extport, std::string& extaddrEx);

	/** ����ӿ�
		ĳ��app��app��֪���ڻ״̬��
	*/
	void onAppActiveTick(Network::Channel* pChannel, COMPONENT_TYPE componentType, COMPONENT_ID componentID);
	
	/** ����ӿ�
		����Ͽ�������������
	*/
	virtual void reqClose(Network::Channel* pChannel);

	/** ����ӿ�
		ĳ��app����鿴��app
	*/
	virtual void lookApp(Network::Channel* pChannel);

	/** ����ӿ�
		����رշ�����
	*/
	virtual void reqCloseServer(Network::Channel* pChannel, MemoryStream& s);

	/** ����ӿ�
		ĳ��app����鿴��app����״̬�� ͨ����console����鿴
	*/
	virtual void queryLoad(Network::Channel* pChannel);

	/** ����ӿ�
		����رշ�����
	*/
	void reqKillServer(Network::Channel* pChannel, MemoryStream& s);

	/** ����ӿ�
		�ͻ��������˵�һ�ν�������, �ͻ��˷����Լ��İ汾����ͨѶ��Կ����Ϣ
		������ˣ� ����˷����Ƿ����ֳɹ�
	*/
	virtual void hello(Network::Channel* pChannel, MemoryStream& s);
	virtual void onHello(Network::Channel* pChannel, 
		const std::string& verInfo, 
		const std::string& scriptVerInfo, 
		const std::string& encryptedKey);

	// ����汾��ƥ��
	virtual void onVersionNotMatch(Network::Channel* pChannel);

	// ����ű���汾��ƥ��
	virtual void onScriptVersionNotMatch(Network::Channel* pChannel);

	/** ����ӿ�
		console����ʼprofile
	*/
	void startProfile(Network::Channel* pChannel, KBEngine::MemoryStream& s);
	virtual void startProfile_(Network::Channel* pChannel, std::string profileName, int8 profileType, uint32 timelen);
		
protected:
	COMPONENT_TYPE											componentType_;
	COMPONENT_ID											componentID_;									// �������ID

	Network::EventDispatcher& 								dispatcher_;	
	Network::NetworkInterface&								networkInterface_;
	
	Timers													timers_;

	// app����˳�� globalΪȫ��(��dbmgr��cellapp��˳��)����˳�� 
	// groupΪ������˳��(��:����baseappΪһ��)
	COMPONENT_ORDER											startGlobalOrder_;
	COMPONENT_ORDER											startGroupOrder_;

	Shutdowner*												pShutdowner_;
	ComponentActiveReportHandler*							pActiveTimerHandle_;

	// �̳߳�
	thread::ThreadPool										threadPool_;	
};

}

#endif // KBE_SERVER_APP_H
