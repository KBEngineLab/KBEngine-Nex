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

#include "pybots.h"
#include "bots.h"
#include "clientobject.h"
#include "server/telnet_server.h"
#include "server/components.h"
#include "server/asyncio_helper.h"
#include "server/plugin_runtime.h"
#include "client_lib/entity.h"
#include "entitydef/entity_component.h"
#include "clientobject.h"
#include "bots_interface.h"
#include "bots_active_report_handler.h"
#include "resmgr/resmgr.h"
#include "network/common.h"
#include "network/tcp_packet.h"
#include "network/udp_packet.h"
#include "network/message_handler.h"
#include "thread/threadpool.h"
#include "server/components.h"
#include "server/serverconfig.h"
#include "helper/watch_pools.h"
#include "helper/console_helper.h"
#include "helper/watcher.h"
#include "helper/profile.h"
#include "helper/profiler.h"
#include "helper/profile_handler.h"
#include "pyscript/pyprofile_handler.h"

#include "../../../server/baseapp/baseapp_interface.h"
#include "../../../server/loginapp/loginapp_interface.h"

namespace KBEngine{

//-------------------------------------------------------------------------------------
Bots::Bots(Network::EventDispatcher& dispatcher, 
			 Network::NetworkInterface& ninterface, 
			 COMPONENT_TYPE componentType,
			 COMPONENT_ID componentID):
ClientApp(dispatcher, ninterface, componentType, componentID),
pPyBots_(NULL),
clients_(),
reqCreateAndLoginTotalCount_(g_kbeSrvConfig.getBots().defaultAddBots_totalCount),
reqCreateAndLoginTickCount_(g_kbeSrvConfig.getBots().defaultAddBots_tickCount),
reqCreateAndLoginTickTime_(g_kbeSrvConfig.getBots().defaultAddBots_tickTime),
pCreateAndLoginHandler_(NULL),
pEventPoller_(Network::EventPoller::create()),
pTelnetServer_(NULL),
pActiveReportHandler_(NULL),
totalKcpHandshakeSuccesses_(0),
totalTcpConnections_(0),
totalTcpFallbacks_(0),
totalNetworkErrors_(0),
totalRemovedClients_(0),
lastBotsTickMicros_(0),
maxBotsTickMicros_(0)
{
	// Bots 同时承载多个客户端，组件 owner 查找必须先通过 componentID 路由到对应的 ClientObject。
	// Bots hosts multiple clients, so component owner lookup must route through componentID to the matching ClientObject first.
	EntityCall::setGetEntityFunc(std::bind(&Bots::tryGetEntity, this,
		std::placeholders::_1, std::placeholders::_2));

	KBEngine::Network::MessageHandlers::pMainMessageHandlers = &BotsInterface::messageHandlers;
	Components::getSingleton().initialize(&ninterface, componentType, componentID);

	// 使用内部通道超时的一半作为报告周期，并设置一秒下限，兼顾失联检测速度与大量组件场景下的消息开销。
	// Report at half the internal-channel timeout with a one-second floor, balancing failure detection against message volume in large deployments.
	pActiveReportHandler_ = new BotsActiveReportHandler(this);
	pActiveReportHandler_->start(KBE_MAX(1.f, Network::g_channelInternalTimeout / 2.f));
}

//-------------------------------------------------------------------------------------
Bots::~Bots()
{
	SAFE_RELEASE(pActiveReportHandler_);
	Components::getSingleton().finalise();
	SAFE_RELEASE(pEventPoller_);
}

//-------------------------------------------------------------------------------------
bool Bots::initialize()
{
	// 广播自己的地址给网上上的所有kbemachine
	this->dispatcher().addTask(&Components::getSingleton());
	return ClientApp::initialize() && initializeWatcher();
}

//-------------------------------------------------------------------------------------
bool Bots::initializeWatcher()
{
	WATCH_OBJECT("bots/clients/total", this, &Bots::numClients);
	WATCH_OBJECT("bots/clients/kcp", this, &Bots::numKcpClients);
	WATCH_OBJECT("bots/clients/tcp", this, &Bots::numTcpClients);
	WATCH_OBJECT("bots/clients/kcpHandshaking", this, &Bots::numKcpHandshakes);
	WATCH_OBJECT("bots/clients/destroyed", this, &Bots::numDestroyedClients);
	WATCH_OBJECT("bots/totals/kcpHandshakeSuccesses", this, &Bots::totalKcpHandshakeSuccesses);
	WATCH_OBJECT("bots/totals/tcpConnections", this, &Bots::totalTcpConnections);
	WATCH_OBJECT("bots/totals/tcpFallbacks", this, &Bots::totalTcpFallbacks);
	WATCH_OBJECT("bots/totals/networkErrors", this, &Bots::totalNetworkErrors);
	WATCH_OBJECT("bots/totals/removedClients", this, &Bots::totalRemovedClients);
	WATCH_OBJECT("bots/tick/lastMicros", this, &Bots::lastBotsTickMicros);
	WATCH_OBJECT("bots/tick/maxMicros", this, &Bots::maxBotsTickMicros);
	// Bots 不经过 ServerApp 的 Watcher 初始化，必须显式暴露客户端 ACK 共用的 UDP completion 队列。
	// Bots bypasses ServerApp watcher initialization, so explicitly expose the UDP completion queue shared by client ACKs.
	WATCH_OBJECT("bots/network/poller/udpSendBacklogBytes", &networkInterface(), &Network::NetworkInterface::pollerUdpSendBacklogBytes);
	WATCH_OBJECT("bots/network/poller/udpSendBacklogPeakBytes", &networkInterface(), &Network::NetworkInterface::pollerUdpSendBacklogPeakBytes);
	WATCH_OBJECT("bots/network/poller/udpSendBackpressure", &networkInterface(), &Network::NetworkInterface::pollerUdpSendBackpressureCount);
	// Bots 不继承 ServerApp 的完整 Watcher 注册，显式暴露 completion context，
	// 以区分 IOCP 缓存与每个 ClientObject/Entity 的长期内存。
	// Bots does not inherit ServerApp's complete watcher registration. Expose
	// completion-context counters explicitly so IOCP cache memory can be separated
	// from the long-lived ClientObject/Entity cost.
	WATCH_OBJECT("bots/network/poller/contextAllocations", &networkInterface(), &Network::NetworkInterface::pollerContextAllocations);
	WATCH_OBJECT("bots/network/poller/contextReuses", &networkInterface(), &Network::NetworkInterface::pollerContextReuses);
	WATCH_OBJECT("bots/network/poller/contextsOutstanding", &networkInterface(), &Network::NetworkInterface::pollerContextsOutstanding);
	WATCH_OBJECT("bots/network/poller/contextsCached", &networkInterface(), &Network::NetworkInterface::pollerContextsCached);
	WATCH_OBJECT("bots/network/poller/contextsPeakOutstanding", &networkInterface(), &Network::NetworkInterface::pollerContextsPeakOutstanding);
	WATCH_OBJECT("bots/network/poller/contextsOutstandingBytes", &networkInterface(), &Network::NetworkInterface::pollerContextsOutstandingBytes);
	WATCH_OBJECT("bots/network/poller/contextsCachedBytes", &networkInterface(), &Network::NetworkInterface::pollerContextsCachedBytes);
	// 聚合目录只复用现有 getter，使性能控制器一次请求取得关键快照，避免高负载时串行查询多个目录放大主线程等待。
	// The aggregate directory reuses existing getters so one controller request obtains the critical snapshot without serial main-thread waits.
	WATCH_OBJECT("bots/performance/clientsTotal", this, &Bots::numClients);
	WATCH_OBJECT("bots/performance/clientsKcp", this, &Bots::numKcpClients);
	WATCH_OBJECT("bots/performance/clientsTcp", this, &Bots::numTcpClients);
	WATCH_OBJECT("bots/performance/clientsDestroyed", this, &Bots::numDestroyedClients);
	WATCH_OBJECT("bots/performance/kcpHandshakeSuccesses", this, &Bots::totalKcpHandshakeSuccesses);
	WATCH_OBJECT("bots/performance/tcpFallbacks", this, &Bots::totalTcpFallbacks);
	WATCH_OBJECT("bots/performance/networkErrors", this, &Bots::totalNetworkErrors);
	WATCH_OBJECT("bots/performance/removedClients", this, &Bots::totalRemovedClients);
	WATCH_OBJECT("bots/performance/tickLastMicros", this, &Bots::lastBotsTickMicros);
	WATCH_OBJECT("bots/performance/tickMaxMicros", this, &Bots::maxBotsTickMicros);
	WATCH_OBJECT("bots/performance/udpSendBacklogBytes", &networkInterface(), &Network::NetworkInterface::pollerUdpSendBacklogBytes);
	WATCH_OBJECT("bots/performance/udpSendBacklogPeakBytes", &networkInterface(), &Network::NetworkInterface::pollerUdpSendBacklogPeakBytes);
	WATCH_OBJECT("bots/performance/udpSendBackpressure", &networkInterface(), &Network::NetworkInterface::pollerUdpSendBackpressureCount);
	// KCP 调度与队列指标用于区分空闲定时维护开销和真实业务流量，避免仅凭进程 CPU 猜测热点。
	// KCP scheduler and queue metrics distinguish idle maintenance cost from real traffic instead of inferring hotspots from process CPU alone.
	WATCH_OBJECT("bots/performance/kcpScheduledChannels", &networkInterface(), &Network::NetworkInterface::kcpScheduledChannelCount);
	WATCH_OBJECT("bots/performance/kcpUpdateCalls", &networkInterface(), &Network::NetworkInterface::kcpUpdateCallCount);
	WATCH_OBJECT("bots/performance/kcpTimerWakeups", &networkInterface(), &Network::NetworkInterface::kcpTimerWakeupCount);
	WATCH_OBJECT("bots/performance/kcpTimerRearms", &networkInterface(), &Network::NetworkInterface::kcpTimerRearmCount);
	WATCH_OBJECT("bots/performance/kcpPendingSegments", &networkInterface(), &Network::NetworkInterface::kcpPendingSegmentCount);
	WATCH_OBJECT("bots/performance/kcpQueuedSegments", &networkInterface(), &Network::NetworkInterface::kcpQueuedSegmentCount);
	WATCH_OBJECT("bots/performance/kcpUnackedSegments", &networkInterface(), &Network::NetworkInterface::kcpUnackedSegmentCount);
	WATCH_OBJECT("bots/performance/kcpFixedAllocatedBytes", this, &Bots::kcpFixedAllocatedBytes);
	WATCH_OBJECT("bots/performance/kcpDynamicAllocatedBytes", this, &Bots::kcpDynamicAllocatedBytes);
	WATCH_OBJECT("bots/performance/clientEntities", this, &Bots::numClientEntities);
	return WatchPool::initWatchPools();
}

//-------------------------------------------------------------------------------------	
bool Bots::initializeBegin()
{
	Network::g_extReceiveWindowBytesOverflow = 0;
	Network::g_intReceiveWindowBytesOverflow = 0;
	Network::g_intReceiveWindowMessagesOverflow = 0;
	Network::g_extReceiveWindowMessagesOverflow = 0;
	Network::g_receiveWindowMessagesOverflowCritical = 0;

	gameTimer_ = this->dispatcher().addTimer(1000000 / g_kbeSrvConfig.gameUpdateHertz(), this,
							reinterpret_cast<void *>(TIMEOUT_GAME_TICK));

	// Bots 不继承 EntityApp/PythonApp，必须在自身生命周期显式安装 asyncio dispatcher timer。
	// Bots does not inherit EntityApp or PythonApp, so it must install the asyncio dispatcher timer in its own lifecycle.
	if (!AsyncioHelper::installTimer(this->dispatcher()))
		return false;

	ProfileVal::setWarningPeriod(stampsPerSecond() / g_kbeSrvConfig.gameUpdateHertz());
	return true;
}

//-------------------------------------------------------------------------------------	
bool Bots::initializeEnd()
{
	pTelnetServer_ = new TelnetServer(&dispatcher(), &networkInterface());
	pTelnetServer_->pScript(&getScript());

	if(!pTelnetServer_->start(g_kbeSrvConfig.getBots().telnet_passwd, 
		g_kbeSrvConfig.getBots().telnet_deflayer, 
		g_kbeSrvConfig.getBots().telnet_port))
	{
		ERROR_MSG("Bots::initialize: initializeEnd error!\n");
		return false;
	}

	// 所有脚本都加载完毕
	PyObject* pyResult = PyObject_CallMethod(getEntryScript().get(), 
										const_cast<char*>("onInit"), 
										const_cast<char*>("i"), 
										0);

	if(pyResult != NULL)
	{
		AsyncioHelper::submitCoroutine(pyResult);
		Py_DECREF(pyResult);
	}
	else
	{
		SCRIPT_ERROR_CHECK();
		return false;
	}

	// Bots 在 EntityDef 和宿主入口就绪后启动插件，避免插件访问尚未注册的客户端实体类型。
	// Bots starts plugins after EntityDef and the host entry are ready, preventing access to unregistered client entity types.
	if (!PluginRuntime::instance().initialize(BOTS_TYPE, false))
		return false;

	if (!PluginRuntime::instance().onComponentReady(true))
		return false;

	return true;
}

//-------------------------------------------------------------------------------------
void Bots::finalise()
{
	// 结束通知脚本
	PyObject* pyResult = PyObject_CallMethod(getEntryScript().get(), 
										const_cast<char*>("onFinish"),
										const_cast<char*>(""));

	if(pyResult != NULL)
	{
		AsyncioHelper::submitCoroutine(pyResult);
		Py_DECREF(pyResult);
	}
	else
	{
		SCRIPT_ERROR_CHECK();
	}

	// 在客户端实体和 Python 类型释放前停止 Task，避免协程继续访问 Bots 持有的对象。
	// Stop Tasks before client entities and Python types are released so coroutines cannot access objects owned by Bots.
	AsyncioHelper::shutdown();

	// 插件先于机器人客户端和 Python 类型释放，以便 onFini 安全注销回调与共享状态。
	// Plugins stop before bot clients and Python types so onFini can safely unregister callbacks and shared state.
	PluginRuntime::instance().finalise();

	CLIENTS::iterator iter = clients_.begin();
	for(; iter != clients_.end(); ++iter)
	{
		iter->second->finalise();
		Py_DECREF(iter->second);
	}

	clients_.clear();

	reqCreateAndLoginTotalCount_ = 0;
	SAFE_RELEASE(pCreateAndLoginHandler_);
	
	if (pTelnetServer_)
	{
		pTelnetServer_->stop();
		SAFE_RELEASE(pTelnetServer_);
	}

	ClientApp::finalise();
}

//-------------------------------------------------------------------------------------
bool Bots::installEntityDef()
{
	EntityDef::entityAliasID(ServerConfig::getSingleton().getCellApp().aliasEntityID);
	EntityDef::entitydefAliasID(ServerConfig::getSingleton().getCellApp().entitydefAliasID);

	return ClientApp::installEntityDef();
}

//-------------------------------------------------------------------------------------
bool Bots::uninstallPyScript()
{
	return ClientApp::uninstallPyScript();
}

//-------------------------------------------------------------------------------------
bool Bots::installPyModules()
{
	ClientObject::installScript(NULL);
	PyBots::installScript(NULL);
	// 组件脚本会在实体定义初始化期间直接求值，因此必须先把基类导出到 KBEngine 模块。
	// Component scripts are evaluated while entity definitions initialize, so export their base type to the KBEngine module first.
	EntityComponent::installScript(getScript().getModule());

	pPyBots_ = new PyBots();
	registerPyObjectToScript("bots", pPyBots_);
	
	APPEND_SCRIPT_MODULE_METHOD(getScript().getModule(), addBots, __py_addBots,	METH_VARARGS, 0);

	// 注册设置脚本输出类型
	APPEND_SCRIPT_MODULE_METHOD(getScript().getModule(),	scriptLogType,	__py_setScriptLogType,	METH_VARARGS,	0)
	if(PyModule_AddIntConstant(this->getScript().getModule(), "LOG_TYPE_NORMAL", log4cxx::ScriptLevel::SCRIPT_INT))
	{
		ERROR_MSG( "Bots::installPyModules: Unable to set KBEngine.LOG_TYPE_NORMAL.\n");
	}

	if(PyModule_AddIntConstant(this->getScript().getModule(), "LOG_TYPE_INFO", log4cxx::ScriptLevel::SCRIPT_INFO))
	{
		ERROR_MSG( "Bots::installPyModules: Unable to set KBEngine.LOG_TYPE_INFO.\n");
	}

	if(PyModule_AddIntConstant(this->getScript().getModule(), "LOG_TYPE_ERR", log4cxx::ScriptLevel::SCRIPT_ERR))
	{
		ERROR_MSG( "Bots::installPyModules: Unable to set KBEngine.LOG_TYPE_ERR.\n");
	}

	if(PyModule_AddIntConstant(this->getScript().getModule(), "LOG_TYPE_DBG", log4cxx::ScriptLevel::SCRIPT_DBG))
	{
		ERROR_MSG( "Bots::installPyModules: Unable to set KBEngine.LOG_TYPE_DBG.\n");
	}

	if(PyModule_AddIntConstant(this->getScript().getModule(), "LOG_TYPE_WAR", log4cxx::ScriptLevel::SCRIPT_WAR))
	{
		ERROR_MSG( "Bots::installPyModules: Unable to set KBEngine.LOG_TYPE_WAR.\n");
	}

	// EntityDef 在导入 Bots 脚本前必须同时看到实体与组件基类，否则组件脚本无法继承 KBEngine.EntityComponent。
	// EntityDef must see both entity and component base types before importing Bots scripts, otherwise component scripts cannot inherit KBEngine.EntityComponent.
	registerScript(client::Entity::getScriptType());
	registerScript(EntityComponent::getScriptType());

	// 安装入口模块
	PyObject *entryScriptFileName = PyUnicode_FromString(g_kbeSrvConfig.getBots().entryScriptFile);
	if(entryScriptFileName != NULL)
	{
		entryScript_ = PyImport_Import(entryScriptFileName);

		if (PyErr_Occurred())
		{
			INFO_MSG(fmt::format("EntityApp::installPyModules: importing scripts/bots/{}.py...\n",
				g_kbeSrvConfig.getBots().entryScriptFile));

			PyErr_PrintEx(0);
		}

		S_RELEASE(entryScriptFileName);

		if(entryScript_.get() == NULL)
		{
			return false;
		}
	}

	onInstallPyModules();

	return true;
}

//-------------------------------------------------------------------------------------
bool Bots::uninstallPyModules()
{
	Py_XDECREF(pPyBots_);
	pPyBots_ = NULL;

	ClientObject::uninstallScript();
	PyBots::uninstallScript();
	EntityComponent::uninstallScript();
	return ClientApp::uninstallPyModules();
}

//-------------------------------------------------------------------------------------
bool Bots::run(void)
{
	pCreateAndLoginHandler_ = new CreateAndLoginHandler();
	return ClientApp::run();
}

//-------------------------------------------------------------------------------------
void Bots::handleTimeout(TimerHandle handle, void * arg)
{
	ClientApp::handleTimeout(handle, arg);
}

//-------------------------------------------------------------------------------------
void Bots::onChannelTimeOut(Network::Channel* pChannel)
{
	ClientObject* pClient = findClient(pChannel);
	if (pClient != NULL)
	{
		// ClientObject 保留 Channel 所有权并在下一个游戏 Tick 统一释放接收器、socket 与 KCP 定时器。
		// ClientObject retains Channel ownership and releases receivers, sockets, and the KCP timer together on the next game Tick.
		pClient->onNetworkError("channel inactivity timeout");
		return;
	}

	ClientApp::onChannelTimeOut(pChannel);
}

//-------------------------------------------------------------------------------------
void Bots::handleGameTick()
{
	const uint64 botsTickStart = timestamp();
	// time_t t = ::time(NULL);
	// static int kbeTime = 0;
	// DEBUG_MSG(fmt::format("Bots::handleGameTick[{}]:{}\n", t, ++kbeTime));

	ClientApp::handleGameTick();

	pEventPoller_->processPendingEvents(0.0);

	{
		AUTO_SCOPED_PROFILE("updateBots");

		CLIENTS::iterator iter = clients().begin();
		for(;iter != clients().end();)
		{
			Network::Channel* pChannel = iter->first;
			ClientObject* pClientObject = iter->second;
			++iter;

			if(pClientObject->isDestroyed())
			{
				delClient(pChannel);
				continue;
			}

			pClientObject->gameTick();
		}
	}

	const uint64 elapsedStamps = timestamp() - botsTickStart;
	lastBotsTickMicros_ = static_cast<uint64>(
		static_cast<double>(elapsedStamps) * 1000000.0 / static_cast<double>(stampsPerSecond()));
	maxBotsTickMicros_ = KBE_MAX(maxBotsTickMicros_, lastBotsTickMicros_);
}

//-------------------------------------------------------------------------------------
uint32 Bots::numKcpClients() const
{
	uint32 count = 0;
	for (CLIENTS::const_iterator iter = clients_.begin(); iter != clients_.end(); ++iter)
		count += iter->second->isKcpTransport() ? 1 : 0;
	return count;
}

//-------------------------------------------------------------------------------------
uint32 Bots::numTcpClients() const
{
	uint32 count = 0;
	for (CLIENTS::const_iterator iter = clients_.begin(); iter != clients_.end(); ++iter)
		count += iter->second->isTcpTransport() ? 1 : 0;
	return count;
}

//-------------------------------------------------------------------------------------
uint32 Bots::numKcpHandshakes() const
{
	uint32 count = 0;
	for (CLIENTS::const_iterator iter = clients_.begin(); iter != clients_.end(); ++iter)
		count += iter->second->isKcpHandshakePending() ? 1 : 0;
	return count;
}

//-------------------------------------------------------------------------------------
uint32 Bots::numDestroyedClients() const
{
	uint32 count = 0;
	for (CLIENTS::const_iterator iter = clients_.begin(); iter != clients_.end(); ++iter)
		count += iter->second->isDestroyed() ? 1 : 0;
	return count;
}

//-------------------------------------------------------------------------------------
uint64 Bots::numClientEntities() const
{
	uint64 count = 0;
	for (CLIENTS::const_iterator iter = clients_.begin(); iter != clients_.end(); ++iter)
	{
		const ClientObject* pClient = iter->second;
		if (pClient != NULL && pClient->pEntities() != NULL)
			count += static_cast<uint64>(pClient->pEntities()->size());
	}
	return count;
}

//-------------------------------------------------------------------------------------
uint64 Bots::kcpFixedAllocatedBytes() const
{
	uint64 allocatedBytes = 0;
	for (CLIENTS::const_iterator iter = clients_.begin(); iter != clients_.end(); ++iter)
	{
		const Network::Channel* pChannel = iter->second != NULL ? iter->second->pServerChannel() : NULL;
		const ikcpcb* pKcp = pChannel != NULL ? pChannel->pKCP() : NULL;
		if (pKcp == NULL)
			continue;

		// ikcp_create()/ikcp_setmtu() allocate the control block and a three-datagram
		// flush buffer. ACK storage grows by ackblock pairs and remains allocated.
		// ikcp_create()/ikcp_setmtu() 固定分配控制块和三份数据报 flush buffer；
		// ACK 存储按 ackblock 对扩容，并在连接生命周期内保留容量。
		allocatedBytes += sizeof(ikcpcb);
		const IUINT32 protocolOverhead = pKcp->mtu - pKcp->mss;
		allocatedBytes += static_cast<uint64>(pKcp->mtu + protocolOverhead) * 3;
		allocatedBytes += static_cast<uint64>(pKcp->ackblock) * 2 * sizeof(IUINT32);
	}
	return allocatedBytes;
}

//-------------------------------------------------------------------------------------
uint64 Bots::kcpDynamicAllocatedBytes() const
{
	uint64 allocatedBytes = 0;
	for (CLIENTS::const_iterator iter = clients_.begin(); iter != clients_.end(); ++iter)
	{
		const Network::Channel* pChannel = iter->second != NULL ? iter->second->pServerChannel() : NULL;
		const ikcpcb* pKcp = pChannel != NULL ? pChannel->pKCP() : NULL;
		if (pKcp == NULL)
			continue;

		const IQUEUEHEAD* queues[] = {
			&pKcp->snd_queue, &pKcp->rcv_queue, &pKcp->snd_buf, &pKcp->rcv_buf
		};
		for (size_t queueIndex = 0; queueIndex < sizeof(queues) / sizeof(queues[0]); ++queueIndex)
		{
			const IQUEUEHEAD* head = queues[queueIndex];
			for (const IQUEUEHEAD* node = head->next; node != head; node = node->next)
			{
				const IKCPSEG* segment = iqueue_entry(node, IKCPSEG, node);
				allocatedBytes += sizeof(IKCPSEG) + static_cast<uint64>(segment->len);
			}
		}
	}
	return allocatedBytes;
}

//-------------------------------------------------------------------------------------
Network::Channel* Bots::findChannelByEntityCall(EntityCall& entitycall)
{
	int32 appID = (int32)entitycall.componentID();
	ClientObject* pClient = findClientByAppID(appID);

	if(pClient)
		return pClient->findChannelByEntityCall(entitycall);

	return NULL;
}

//-------------------------------------------------------------------------------------
PyObject* Bots::tryGetEntity(COMPONENT_ID componentID, ENTITY_ID entityID)
{
	ClientObject* pClient = findClientByAppID(static_cast<int32>(componentID));
	if (!pClient)
		return NULL;

	return pClient->tryGetEntity(componentID, entityID);
}

//-------------------------------------------------------------------------------------
void Bots::addBots(Network::Channel * pChannel, MemoryStream& s)
{
	uint32	reqCreateAndLoginTotalCount;
	uint32 reqCreateAndLoginTickCount = 0;
	float reqCreateAndLoginTickTime = 0;

	s >> reqCreateAndLoginTotalCount;

	reqCreateAndLoginTotalCount_ += reqCreateAndLoginTotalCount;

	if(s.length() > 0)
	{
		s >> reqCreateAndLoginTickCount >> reqCreateAndLoginTickTime;

		if(reqCreateAndLoginTickCount > 0)
			reqCreateAndLoginTickCount_ = reqCreateAndLoginTickCount;
		
		if(reqCreateAndLoginTickTime > 0)
			reqCreateAndLoginTickTime_ = reqCreateAndLoginTickTime;
	}
}

//-------------------------------------------------------------------------------------
PyObject* Bots::__py_addBots(PyObject* self, PyObject* args)
{
	uint32	reqCreateAndLoginTotalCount;
	uint32 reqCreateAndLoginTickCount = 0;
	float reqCreateAndLoginTickTime = 0;

	if(PyTuple_Size(args) == 1)
	{
		if(!PyArg_ParseTuple(args, "I", &reqCreateAndLoginTotalCount))
		{
			PyErr_Format(PyExc_TypeError, "KBEngine::addBots: args error!");
			PyErr_PrintEx(0);
			return NULL;
		}

		Bots::getSingleton().reqCreateAndLoginTotalCount(
			Bots::getSingleton().reqCreateAndLoginTotalCount() + reqCreateAndLoginTotalCount);
	}
	else if(PyTuple_Size(args) == 3)
	{
		if(!PyArg_ParseTuple(args, "I|I|f", &reqCreateAndLoginTotalCount, 
			&reqCreateAndLoginTickCount, &reqCreateAndLoginTickTime))
		{
			PyErr_Format(PyExc_TypeError, "KBEngine::addBots: args error!");
			PyErr_PrintEx(0);
			return NULL;
		}

		Bots::getSingleton().reqCreateAndLoginTotalCount(
			Bots::getSingleton().reqCreateAndLoginTotalCount() + reqCreateAndLoginTotalCount);

		if(reqCreateAndLoginTickCount > 0)
			Bots::getSingleton().reqCreateAndLoginTickCount(reqCreateAndLoginTickCount);
		
		if(reqCreateAndLoginTickTime > 0)
			Bots::getSingleton().reqCreateAndLoginTickTime(reqCreateAndLoginTickTime);
	}
	else
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::addBots: args error!");
		PyErr_PrintEx(0);
		return NULL;
	}

	S_Return;
}

//-------------------------------------------------------------------------------------	
PyObject* Bots::__py_setScriptLogType(PyObject* self, PyObject* args)
{
	int argCount = (int)PyTuple_Size(args);
	if(argCount != 1)
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::scriptLogType(): args error!");
		PyErr_PrintEx(0);
		return 0;
	}

	int type = -1;

	if(!PyArg_ParseTuple(args, "i", &type))
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::scriptLogType(): args error!");
		PyErr_PrintEx(0);
	}

	DebugHelper::getSingleton().setScriptMsgType(type);
	S_Return;
}

//-------------------------------------------------------------------------------------
void Bots::lookApp(Network::Channel* pChannel)
{
	//DEBUG_MSG(fmt::format("Bots::lookApp: {0}\n", pChannel->c_str()));

	Network::Bundle* pBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
	
	(*pBundle) << g_componentType;
	(*pBundle) << componentID_;
	int8 istate = 0;
	(*pBundle) << istate;

	pChannel->send(pBundle);
}

//-------------------------------------------------------------------------------------
void Bots::reqCloseServer(Network::Channel* pChannel, MemoryStream& s)
{
	DEBUG_MSG(fmt::format("Bots::reqCloseServer: {0}\n", pChannel->c_str()));

	Network::Bundle* pBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
	
	bool success = true;
	(*pBundle) << success;
	pChannel->send(pBundle);

	this->shutDown();
}

//-------------------------------------------------------------------------------------
void Bots::reqKillServer(Network::Channel* pChannel, MemoryStream& s)
{
	COMPONENT_ID componentID;
	COMPONENT_TYPE componentType;
	std::string username;
	int32 uid;
	std::string reason;

	s >> componentID >> componentType >> username >> uid >> reason;

	INFO_MSG(fmt::format("Bots::reqKillServer: requester(uid:{}, username:{}, componentType:{}, "
				"componentID:{}, reason:{}, from {})\n",
				uid ,
				username , 
				COMPONENT_NAME_EX((COMPONENT_TYPE)componentType),
				componentID,
				reason,
				pChannel->c_str()));

	CRITICAL_MSG("The application was killed!\n");
}

//-------------------------------------------------------------------------------------
void Bots::onExecScriptCommand(Network::Channel* pChannel, KBEngine::MemoryStream& s)
{
	std::string cmd;
	s.readBlob(cmd);

	PyObject* pycmd = PyUnicode_DecodeUTF8(cmd.data(), cmd.size(), NULL);
	if(pycmd == NULL)
	{
		SCRIPT_ERROR_CHECK();
		return;
	}

	DEBUG_MSG(fmt::format("EntityApp::onExecScriptCommand: size({}), command={}.\n",
		cmd.size(), cmd));

	std::string retbuf = "";
	PyObject* pycmd1 = PyUnicode_AsEncodedString(pycmd, "utf-8", NULL);

	if(getScript().run_simpleString(PyBytes_AsString(pycmd1), &retbuf) == 0)
	{
		// 将结果返回给客户端
		Network::Bundle* pBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
		ConsoleInterface::ConsoleExecCommandCBMessageHandler msgHandler;
		(*pBundle).newMessage(msgHandler);
		ConsoleInterface::ConsoleExecCommandCBMessageHandlerArgs1::staticAddToBundle((*pBundle), retbuf);
		pChannel->send(pBundle);
	}

	Py_DECREF(pycmd);
	Py_DECREF(pycmd1);
}

//-------------------------------------------------------------------------------------
bool Bots::addClient(ClientObject* pClient)
{
	clients().insert(std::make_pair(pClient->pServerChannel(),
		pClient));

	return true;
}

//-------------------------------------------------------------------------------------
bool Bots::delClient(ClientObject* pClient)
{
	return delClient(pClient->pServerChannel());
}

//-------------------------------------------------------------------------------------
bool Bots::delClient(Network::Channel * pChannel)
{
	ClientObject* pClient = findClient(pChannel);
	if(!pClient)
		return false;

	pClient->finalise();
	clients().erase(pChannel);
	Py_DECREF(pClient);
	++totalRemovedClients_;
	return true;
}

//-------------------------------------------------------------------------------------
ClientObject* Bots::findClient(Network::Channel * pChannel)
{
	CLIENTS::iterator iter = clients().find(pChannel);
	if(iter != clients().end())
	{
		return iter->second;
	}

	return NULL;
}

//-------------------------------------------------------------------------------------
ClientObject* Bots::findClientByAppID(int32 appID)
{
	CLIENTS::iterator iter = clients().begin();
	for(; iter != clients().end(); ++iter)
	{
		if(iter->second->appID() == appID)
			return iter->second;
	}

	return NULL;
}

//-------------------------------------------------------------------------------------
void Bots::onAppActiveTick(Network::Channel* pChannel, COMPONENT_TYPE componentType, COMPONENT_ID componentID)
{
	if(componentType != CLIENT_TYPE)
		if(pChannel->isExternal())
			return;
	
	Network::Channel* pTargetChannel = NULL;
	if(componentType != CONSOLE_TYPE && componentType != CLIENT_TYPE)
	{
		Components::ComponentInfos* cinfos = 
			Components::getSingleton().findComponent(componentType, KBEngine::getUserUID(), componentID);

		if(cinfos == NULL)
		{
			ERROR_MSG(fmt::format("Bots::onAppActiveTick[{0:p}]: {1}:{2} not found.\n", 
				(void*)pChannel, COMPONENT_NAME_EX(componentType), componentID));

			return;
		}

		pTargetChannel = cinfos->pChannel;
		pTargetChannel->updateLastReceivedTime();
	}
	else
	{
		pChannel->updateLastReceivedTime();
		pTargetChannel = pChannel;
	}

	//DEBUG_MSG(fmt::format("Bots::onAppActiveTick[:p]: {}:{} lastReceivedTime:{} at {}.\n",
	//	(void*)pChannel, COMPONENT_NAME_EX(componentType), componentID, pChannel->lastReceivedTime(), pChannel->c_str()));
}

//-------------------------------------------------------------------------------------
void Bots::onHelloCB_(Network::Channel* pChannel, const std::string& verInfo, 
		const std::string& scriptVerInfo, const std::string& protocolMD5, const std::string& entityDefMD5, 
		COMPONENT_TYPE componentType)
{
	ClientObject* pClient = findClient(pChannel);
	if(pClient)
	{
		pClient->onHelloCB_(pChannel, verInfo, scriptVerInfo, protocolMD5, entityDefMD5, componentType);
	}
}

//-------------------------------------------------------------------------------------	
void Bots::onVersionNotMatch(Network::Channel* pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if(pClient)
	{
		pClient->onVersionNotMatch(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------	
void Bots::onScriptVersionNotMatch(Network::Channel* pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if(pClient)
	{
		pClient->onScriptVersionNotMatch(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onCreateAccountResult(Network::Channel * pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if(pClient)
	{
		pClient->onCreateAccountResult(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------	
void Bots::onLoginSuccessfully(Network::Channel * pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if(pClient)
	{
		pClient->onLoginSuccessfully(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------	
void Bots::onLoginFailed(Network::Channel * pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if(pClient)
	{
		pClient->onLoginFailed(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------	
void Bots::onLoginBaseappFailed(Network::Channel * pChannel, SERVER_ERROR_CODE failedcode)
{
	ClientObject* pClient = findClient(pChannel);
	if(pClient)
	{
		pClient->onLoginBaseappFailed(pChannel, failedcode);
	}
}

//-------------------------------------------------------------------------------------	
void Bots::onReloginBaseappSuccessfully(Network::Channel * pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if(pClient)
	{
		pClient->onReloginBaseappSuccessfully(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------	
void Bots::onCreatedProxies(Network::Channel * pChannel, 
								 uint64 rndUUID, ENTITY_ID eid, std::string& entityType)
{
	ClientObject* pClient = findClient(pChannel);
	if(pClient)
	{
		pClient->onCreatedProxies(pChannel, rndUUID, eid, entityType);
	}
}

//-------------------------------------------------------------------------------------	
void Bots::onEntityEnterWorld(Network::Channel * pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if(pClient)
	{
		pClient->onEntityEnterWorld(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------	
void Bots::onEntityLeaveWorld(Network::Channel * pChannel, ENTITY_ID eid)
{
	ClientObject* pClient = findClient(pChannel);
	if(pClient)
	{
		pClient->onEntityLeaveWorld(pChannel, eid);
	}
}

//-------------------------------------------------------------------------------------	
void Bots::onEntityLeaveWorldOptimized(Network::Channel * pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if(pClient)
	{
		pClient->onEntityLeaveWorldOptimized(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------	
void Bots::onEntityEnterSpace(Network::Channel * pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if(pClient)
	{
		pClient->onEntityEnterSpace(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------	
void Bots::onEntityLeaveSpace(Network::Channel * pChannel, ENTITY_ID eid)
{
	ClientObject* pClient = findClient(pChannel);
	if(pClient)
	{
		pClient->onEntityLeaveSpace(pChannel, eid);
	}
}

//-------------------------------------------------------------------------------------	
void Bots::onEntityDestroyed(Network::Channel * pChannel, ENTITY_ID eid)
{
	ClientObject* pClient = findClient(pChannel);
	if(pClient)
	{
		pClient->onEntityDestroyed(pChannel, eid);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onRemoteMethodCall(Network::Channel* pChannel, KBEngine::MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if(pClient)
	{
		pClient->onRemoteMethodCall(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onRemoteMethodCallOptimized(Network::Channel* pChannel, KBEngine::MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if(pClient)
	{
		pClient->onRemoteMethodCallOptimized(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------	
void Bots::onKicked(Network::Channel * pChannel, SERVER_ERROR_CODE failedcode)
{
	ClientObject* pClient = findClient(pChannel);
	if(pClient)
	{
		pClient->onKicked(pChannel, failedcode);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onUpdatePropertys(Network::Channel* pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if(pClient)
	{
		pClient->onUpdatePropertys(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onUpdatePropertysOptimized(Network::Channel* pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if(pClient)
	{
		pClient->onUpdatePropertysOptimized(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onUpdateBasePos(Network::Channel* pChannel, float x, float y, float z)
{
	ClientObject* pClient = findClient(pChannel);
	if(pClient)
	{
		pClient->onUpdateBasePos(pChannel, x, y, z);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onUpdateBasePosXZ(Network::Channel* pChannel, float x, float z)
{
	ClientObject* pClient = findClient(pChannel);
	if(pClient)
	{
		pClient->onUpdateBasePosXZ(pChannel, x, z);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onUpdateBaseDir(Network::Channel* pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if (pClient)
	{
		pClient->onUpdateBaseDir(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onSetEntityPosAndDir(Network::Channel* pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if(pClient)
	{
		pClient->onSetEntityPosAndDir(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onUpdateData(Network::Channel* pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if(pClient)
	{
		pClient->onUpdateData(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onUpdateData_ypr(Network::Channel* pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if(pClient)
	{
		pClient->onUpdateData_ypr(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onUpdateData_yp(Network::Channel* pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if(pClient)
	{
		pClient->onUpdateData_yp(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onUpdateData_yr(Network::Channel* pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if(pClient)
	{
		pClient->onUpdateData_yr(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onUpdateData_pr(Network::Channel* pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if(pClient)
	{
		pClient->onUpdateData_pr(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onUpdateData_y(Network::Channel* pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if(pClient)
	{
		pClient->onUpdateData_y(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onUpdateData_p(Network::Channel* pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if(pClient)
	{
		pClient->onUpdateData_p(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onUpdateData_r(Network::Channel* pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if(pClient)
	{
		pClient->onUpdateData_r(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onUpdateData_xz(Network::Channel* pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if(pClient)
	{
		pClient->onUpdateData_xz(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onUpdateData_xz_ypr(Network::Channel* pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if(pClient)
	{
		pClient->onUpdateData_xz_ypr(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onUpdateData_xz_yp(Network::Channel* pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if(pClient)
	{
		pClient->onUpdateData_xz_yp(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onUpdateData_xz_yr(Network::Channel* pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if(pClient)
	{
		pClient->onUpdateData_xz_yr(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onUpdateData_xz_pr(Network::Channel* pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if(pClient)
	{
		pClient->onUpdateData_xz_pr(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onUpdateData_xz_y(Network::Channel* pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if(pClient)
	{
		pClient->onUpdateData_xz_y(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onUpdateData_xz_p(Network::Channel* pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if(pClient)
	{
		pClient->onUpdateData_xz_p(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onUpdateData_xz_r(Network::Channel* pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if(pClient)
	{
		pClient->onUpdateData_xz_r(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onUpdateData_xyz(Network::Channel* pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if(pClient)
	{
		pClient->onUpdateData_xyz(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onUpdateData_xyz_ypr(Network::Channel* pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if(pClient)
	{
		pClient->onUpdateData_xyz_ypr(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onUpdateData_xyz_yp(Network::Channel* pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if(pClient)
	{
		pClient->onUpdateData_xyz_yp(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onUpdateData_xyz_yr(Network::Channel* pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if(pClient)
	{
		pClient->onUpdateData_xyz_yr(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onUpdateData_xyz_pr(Network::Channel* pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if(pClient)
	{
		pClient->onUpdateData_xyz_pr(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onUpdateData_xyz_y(Network::Channel* pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if(pClient)
	{
		pClient->onUpdateData_xyz_y(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onUpdateData_xyz_p(Network::Channel* pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if(pClient)
	{
		pClient->onUpdateData_xyz_p(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onUpdateData_xyz_r(Network::Channel* pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if(pClient)
	{
		pClient->onUpdateData_xyz_r(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onUpdateData_ypr_optimized(Network::Channel* pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if (pClient)
	{
		pClient->onUpdateData_ypr_optimized(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onUpdateData_yp_optimized(Network::Channel* pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if (pClient)
	{
		pClient->onUpdateData_yp_optimized(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onUpdateData_yr_optimized(Network::Channel* pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if (pClient)
	{
		pClient->onUpdateData_yr_optimized(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onUpdateData_pr_optimized(Network::Channel* pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if (pClient)
	{
		pClient->onUpdateData_pr_optimized(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onUpdateData_y_optimized(Network::Channel* pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if (pClient)
	{
		pClient->onUpdateData_y_optimized(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onUpdateData_p_optimized(Network::Channel* pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if (pClient)
	{
		pClient->onUpdateData_p_optimized(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onUpdateData_r_optimized(Network::Channel* pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if (pClient)
	{
		pClient->onUpdateData_r_optimized(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onUpdateData_xz_optimized(Network::Channel* pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if (pClient)
	{
		pClient->onUpdateData_xz_optimized(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onUpdateData_xz_ypr_optimized(Network::Channel* pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if (pClient)
	{
		pClient->onUpdateData_xz_ypr_optimized(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onUpdateData_xz_yp_optimized(Network::Channel* pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if (pClient)
	{
		pClient->onUpdateData_xz_yp_optimized(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onUpdateData_xz_yr_optimized(Network::Channel* pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if (pClient)
	{
		pClient->onUpdateData_xz_yr_optimized(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onUpdateData_xz_pr_optimized(Network::Channel* pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if (pClient)
	{
		pClient->onUpdateData_xz_pr_optimized(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onUpdateData_xz_y_optimized(Network::Channel* pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if (pClient)
	{
		pClient->onUpdateData_xz_y_optimized(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onUpdateData_xz_p_optimized(Network::Channel* pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if (pClient)
	{
		pClient->onUpdateData_xz_p_optimized(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onUpdateData_xz_r_optimized(Network::Channel* pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if (pClient)
	{
		pClient->onUpdateData_xz_r_optimized(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onUpdateData_xyz_optimized(Network::Channel* pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if (pClient)
	{
		pClient->onUpdateData_xyz_optimized(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onUpdateData_xyz_ypr_optimized(Network::Channel* pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if (pClient)
	{
		pClient->onUpdateData_xyz_ypr_optimized(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onUpdateData_xyz_yp_optimized(Network::Channel* pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if (pClient)
	{
		pClient->onUpdateData_xyz_yp_optimized(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onUpdateData_xyz_yr_optimized(Network::Channel* pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if (pClient)
	{
		pClient->onUpdateData_xyz_yr_optimized(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onUpdateData_xyz_pr_optimized(Network::Channel* pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if (pClient)
	{
		pClient->onUpdateData_xyz_pr_optimized(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onUpdateData_xyz_y_optimized(Network::Channel* pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if (pClient)
	{
		pClient->onUpdateData_xyz_y_optimized(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onUpdateData_xyz_p_optimized(Network::Channel* pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if (pClient)
	{
		pClient->onUpdateData_xyz_p_optimized(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onUpdateData_xyz_r_optimized(Network::Channel* pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if (pClient)
	{
		pClient->onUpdateData_xyz_r_optimized(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onControlEntity(Network::Channel* pChannel, int32 entityID, int8 isControlled)
{
	ClientObject* pClient = findClient(pChannel);
	if (pClient)
	{
		pClient->onControlEntity(pChannel, entityID, isControlled);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onStreamDataStarted(Network::Channel* pChannel, int16 id, uint32 datasize, std::string& descr)
{
	ClientObject* pClient = findClient(pChannel);
	if(pClient)
	{
		pClient->onStreamDataStarted(pChannel, id, datasize, descr);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onStreamDataRecv(Network::Channel* pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if(pClient)
	{
		pClient->onStreamDataRecv(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------
void Bots::onStreamDataCompleted(Network::Channel* pChannel, int16 id)
{
	ClientObject* pClient = findClient(pChannel);
	if(pClient)
	{
		pClient->onStreamDataCompleted(pChannel, id);
	}
}

//-------------------------------------------------------------------------------------	
void Bots::initSpaceData(Network::Channel* pChannel, MemoryStream& s)
{
	ClientObject* pClient = findClient(pChannel);
	if (pClient)
	{
		pClient->initSpaceData(pChannel, s);
	}
}

//-------------------------------------------------------------------------------------	
void Bots::setSpaceData(Network::Channel* pChannel, SPACE_ID spaceID, const std::string& key, const std::string& value)
{
	ClientObject* pClient = findClient(pChannel);
	if(pClient)
	{
		pClient->setSpaceData(pChannel, spaceID, key, value);
	}
}

//-------------------------------------------------------------------------------------	
void Bots::delSpaceData(Network::Channel* pChannel, SPACE_ID spaceID, const std::string& key)
{
	ClientObject* pClient = findClient(pChannel);
	if(pClient)
	{
		pClient->delSpaceData(pChannel, spaceID, key);
	}
}

//-------------------------------------------------------------------------------------		
void Bots::queryWatcher(Network::Channel* pChannel, MemoryStream& s)
{
	AUTO_SCOPED_PROFILE("watchers");

	std::string path;
	s >> path;

	MemoryStream::SmartPoolObjectPtr readStreamPtr = MemoryStream::createSmartPoolObj(OBJECTPOOL_POINT);
	WatcherPaths::root().readWatchers(path, readStreamPtr.get()->get());

	MemoryStream::SmartPoolObjectPtr readStreamPtr1 = MemoryStream::createSmartPoolObj(OBJECTPOOL_POINT);
	WatcherPaths::root().readChildPaths(path, path, readStreamPtr1.get()->get());

	Network::Bundle* pBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
	ConsoleInterface::ConsoleWatcherCBMessageHandler msgHandler;
	(*pBundle).newMessage(msgHandler);

	uint8 type = 0;
	(*pBundle) << type;
	(*pBundle).append(readStreamPtr.get()->get());
	pChannel->send(pBundle);

	Network::Bundle* pBundle1 = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
	(*pBundle1).newMessage(msgHandler);

	type = 1;
	(*pBundle1) << type;
	(*pBundle1).append(readStreamPtr1.get()->get());
	pChannel->send(pBundle1);
}

//-------------------------------------------------------------------------------------
void Bots::startProfile(Network::Channel* pChannel, KBEngine::MemoryStream& s)
{
	std::string profileName;
	int8 profileType;
	uint32 timelen;

	s >> profileName >> profileType >> timelen;

	startProfile_(pChannel, profileName, profileType, timelen);
}

//-------------------------------------------------------------------------------------
void Bots::startProfile_(Network::Channel* pChannel, std::string profileName, int8 profileType, uint32 timelen)
{
	switch(profileType)
	{
	case 0:	// pyprofile
		new PyProfileHandler(this->networkInterface(), timelen, profileName, pChannel->addr());
		break;
	case 1:	// cprofile
		new CProfileHandler(this->networkInterface(), timelen, profileName, pChannel->addr());
		break;
	case 2:	// eventprofile
		new EventProfileHandler(this->networkInterface(), timelen, profileName, pChannel->addr());
		break;
	case 3:	// networkprofile
		new NetworkProfileHandler(this->networkInterface(), timelen, profileName, pChannel->addr());
		break;
	default:
		ERROR_MSG(fmt::format("Bots::startProfile_: type({}:{}) not support!\n", 
			profileType, profileName));

		break;
	};
}

//-------------------------------------------------------------------------------------
void Bots::onAppActiveTickCB(Network::Channel* pChannel)
{
	ClientObject* pClient = findClient(pChannel);
	if (pClient)
	{
		pClient->onAppActiveTickCB(pChannel);
	}
}

//-------------------------------------------------------------------------------------

}
