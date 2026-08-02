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


#include "serverapp.h"
#include "server/component_active_report_handler.h"
#include "server/shutdowner.h"
#include "server/serverconfig.h"
#include "server/components.h"
#include "server/component_routing_guard.h"
#include "network/channel.h"
#include "network/bundle.h"
#include "network/common.h"
#include "common/memorystream.h"
#include "helper/console_helper.h"
#include "helper/sys_info.h"
#include "helper/watch_pools.h"
#include "resmgr/resmgr.h"

#include "../../server/baseappmgr/baseappmgr_interface.h"
#include "../../server/cellappmgr/cellappmgr_interface.h"
#include "../../server/baseapp/baseapp_interface.h"
#include "../../server/cellapp/cellapp_interface.h"
#include "../../server/dbmgr/dbmgr_interface.h"
#include "../../server/loginapp/loginapp_interface.h"
#include "../../server/tools/logger/logger_interface.h"
#include "../../server/tools/interfaces/interfaces_interface.h"

namespace KBEngine{
COMPONENT_TYPE g_componentType = UNKNOWN_COMPONENT_TYPE;
COMPONENT_ID g_componentID = 0;
COMPONENT_ORDER g_componentGlobalOrder = -1;
COMPONENT_ORDER g_componentGroupOrder = -1;
COMPONENT_GUS g_genuuid_sections = -1;

GAME_TIME g_kbetime = 0;

//-------------------------------------------------------------------------------------
ServerApp::ServerApp(Network::EventDispatcher& dispatcher, 
					 Network::NetworkInterface& ninterface, 
					 COMPONENT_TYPE componentType,
					 COMPONENT_ID componentID):
SignalHandler(),
TimerHandler(),
ShutdownHandler(),
Network::ChannelTimeOutHandler(),
Components::ComponentsNotificationHandler(),
componentType_(componentType),
componentID_(componentID),
dispatcher_(dispatcher),
networkInterface_(ninterface),
timers_(),
startGlobalOrder_(-1),
startGroupOrder_(-1),
pShutdowner_(NULL),
pActiveTimerHandle_(NULL),
threadPool_()
{
	networkInterface_.pChannelTimeOutHandler(this);
	networkInterface_.pChannelDeregisterHandler(this);

	// 广播自己的地址给网上上的所有kbemachine
	// 并且从kbemachine获取basappmgr和cellappmgr以及dbmgr地址
	Components::getSingleton().pHandler(this);
	this->dispatcher().addTask(&Components::getSingleton());
	
	pActiveTimerHandle_ = new ComponentActiveReportHandler(this);
	pActiveTimerHandle_->startActiveTick(KBE_MAX(1.f, Network::g_channelInternalTimeout / 2.0f));

	// 默认所有app都设置为这个值， 如果需要调整则各自在派生类重新赋值
	ProfileVal::setWarningPeriod(stampsPerSecond() / g_kbeSrvConfig.gameUpdateHertz());
}

//-------------------------------------------------------------------------------------
ServerApp::~ServerApp()
{
	SAFE_RELEASE(pActiveTimerHandle_);
	SAFE_RELEASE(pShutdowner_);
}

//-------------------------------------------------------------------------------------	
void ServerApp::shutDown(float shutdowntime)
{
	if(pShutdowner_ == NULL)
	{
		pShutdowner_ = new Shutdowner(this);
	}
	else
	{
		WARNING_MSG(fmt::format("ServerApp::shutDown:  In shuttingdown!\n"));
		return;
	}
	
	pShutdowner_->shutdown(shutdowntime < 0.f ? g_kbeSrvConfig.shutdowntime() : shutdowntime, 
		g_kbeSrvConfig.shutdownWaitTickTime(), dispatcher_);
}

//-------------------------------------------------------------------------------------
void ServerApp::onShutdownBegin()
{
#if KBE_PLATFORM == PLATFORM_WIN32
	printf("[INFO]: shutdown begin.\n");
#endif

	dispatcher_.setWaitBreakProcessing();
}

//-------------------------------------------------------------------------------------
void ServerApp::onShutdown(bool first)
{
}

//-------------------------------------------------------------------------------------
void ServerApp::onShutdownEnd()
{
	dispatcher_.breakProcessing();
}

//-------------------------------------------------------------------------------------
bool ServerApp::loadConfig()
{
	return true;
}

//-------------------------------------------------------------------------------------		
bool ServerApp::installSignals()
{
	g_kbeSignalHandlers.attachApp(this);
	g_kbeSignalHandlers.ignoreSignal(SIGPIPE);
	g_kbeSignalHandlers.addSignal(SIGINT, this);
	g_kbeSignalHandlers.addSignal(SIGHUP, this);
	return true;
}

//-------------------------------------------------------------------------------------		
bool ServerApp::initialize()
{
	if (!installSignals())
		return false;

	if(!initThreadPool())
		return false;
	
	if(!loadConfig())
		return false;
	
	if(!initializeBegin())
		return false;
	
	if(!inInitialize())
		return false;

	bool ret = initializeEnd();

	// 最后仍然需要设置一次，避免期间被其他第三方库修改
	if (!installSignals())
		return false;

#ifdef ENABLE_WATCHERS
	return ret &&  Network::initialize() && initializeWatcher();
#else
	return ret && Network::initialize();
#endif
}

//-------------------------------------------------------------------------------------		
bool ServerApp::initializeWatcher()
{
	WATCH_OBJECT("stats/stampsPerSecond", &KBEngine::stampsPerSecond);
	WATCH_OBJECT("uid", &KBEngine::getUserUID);
	WATCH_OBJECT("username", &KBEngine::getUsername);
	WATCH_OBJECT("componentType", componentType_);
	WATCH_OBJECT("componentID", componentID_);
	WATCH_OBJECT("globalOrder", this, &ServerApp::globalOrder);
	WATCH_OBJECT("groupOrder", this, &ServerApp::groupOrder);
	WATCH_OBJECT("gametime", this, &ServerApp::time);
	WATCH_OBJECT("network/channels/external", &networkInterface_, &Network::NetworkInterface::numExtChannels);
	WATCH_OBJECT("network/channels/externalTcp", &networkInterface_, &Network::NetworkInterface::numExternalTcpChannels);
	WATCH_OBJECT("network/channels/externalWebSocket", &networkInterface_, &Network::NetworkInterface::numExternalWebSocketChannels);
	WATCH_OBJECT("network/channels/externalKcp", &networkInterface_, &Network::NetworkInterface::numExternalKcpChannels);
	WATCH_OBJECT("network/channels/externalUdp", &networkInterface_, &Network::NetworkInterface::numExternalUdpChannels);
	WATCH_OBJECT("network/channels/externalKcpControlBlocks", &networkInterface_, &Network::NetworkInterface::numExternalKcpControlBlocks);
	WATCH_OBJECT("network/channels/externalKcpUpdateTimers", &networkInterface_, &Network::NetworkInterface::numExternalKcpUpdateTimers);
	// 关闭维护队列应远小于总 Channel 数，持续增长表示优雅关闭或回收流程没有收敛。
	// The close-maintenance queue should remain far smaller than the Channel population; sustained growth exposes a stalled close or reclamation path.
	WATCH_OBJECT("network/channels/pendingMaintenance", &networkInterface_, &Network::NetworkInterface::pendingChannelMaintenanceCount);
	// completion 重投递队列应只包含暂时失败项；持续非零或 retry 快速增长表示 SQ/驱动资源或 socket 生命周期异常。
	// The completion rearm queue should contain only transient failures; sustained backlog or rapidly growing retries signals SQ/driver pressure or a socket lifecycle fault.
	WATCH_OBJECT("network/poller/pendingRearms", &networkInterface_, &Network::NetworkInterface::pendingPollerRearms);
	WATCH_OBJECT("network/poller/rearmAttempts", &networkInterface_, &Network::NetworkInterface::pollerRearmAttempts);
	WATCH_OBJECT("network/poller/rearmRetries", &networkInterface_, &Network::NetworkInterface::pollerRearmRetries);
	// Context and ownership counters distinguish allocator pressure from unavoidable multi-buffer coalescing without a socket-state scan.
	// context 与所有权指标用于区分分配器压力和不可避免的多缓冲合批，查询过程不扫描 socket 状态。
	WATCH_OBJECT("network/poller/contextAllocations", &networkInterface_, &Network::NetworkInterface::pollerContextAllocations);
	WATCH_OBJECT("network/poller/contextReuses", &networkInterface_, &Network::NetworkInterface::pollerContextReuses);
	WATCH_OBJECT("network/poller/contextsOutstanding", &networkInterface_, &Network::NetworkInterface::pollerContextsOutstanding);
	WATCH_OBJECT("network/poller/contextsCached", &networkInterface_, &Network::NetworkInterface::pollerContextsCached);
	WATCH_OBJECT("network/poller/contextsPeakOutstanding", &networkInterface_, &Network::NetworkInterface::pollerContextsPeakOutstanding);
	WATCH_OBJECT("network/poller/tcpSendOwnershipTransfers", &networkInterface_, &Network::NetworkInterface::pollerTcpSendOwnershipTransfers);
	WATCH_OBJECT("network/poller/tcpSendBatchCopies", &networkInterface_, &Network::NetworkInterface::pollerTcpSendBatchCopies);
	WATCH_OBJECT("network/poller/tcpSendBatchCopiedBytes", &networkInterface_, &Network::NetworkInterface::pollerTcpSendBatchCopiedBytes);
	WATCH_OBJECT("network/poller/tcpSendBacklogBytes", &networkInterface_, &Network::NetworkInterface::pollerTcpSendBacklogBytes);
	WATCH_OBJECT("network/poller/tcpSendBacklogPeakBytes", &networkInterface_, &Network::NetworkInterface::pollerTcpSendBacklogPeakBytes);
	WATCH_OBJECT("network/poller/tcpSendBackpressure", &networkInterface_, &Network::NetworkInterface::pollerTcpSendBackpressureCount);
	WATCH_OBJECT("network/poller/tcpSendOversizedRejects", &networkInterface_, &Network::NetworkInterface::pollerTcpSendOversizedRejectCount);
	WATCH_OBJECT("network/poller/tcpPartialSends", &networkInterface_, &Network::NetworkInterface::pollerTcpPartialSendCount);
	WATCH_OBJECT("network/poller/receiveOwnershipTransfers", &networkInterface_, &Network::NetworkInterface::pollerReceiveOwnershipTransfers);
	WATCH_OBJECT("network/poller/receiveTransferredBytes", &networkInterface_, &Network::NetworkInterface::pollerReceiveTransferredBytes);
	WATCH_OBJECT("network/poller/udpSendBacklogBytes", &networkInterface_, &Network::NetworkInterface::pollerUdpSendBacklogBytes);
	WATCH_OBJECT("network/poller/udpSendBacklogPeakBytes", &networkInterface_, &Network::NetworkInterface::pollerUdpSendBacklogPeakBytes);
	WATCH_OBJECT("network/poller/udpSendBackpressure", &networkInterface_, &Network::NetworkInterface::pollerUdpSendBackpressureCount);
	// 批次计数只在 poll 返回路径做常量时间累加，用于证明 256 completion 公平性预算是否持续饱和。
	// Batch counters add constant-time work only on poll return and reveal whether the 256-completion fairness budget remains saturated.
	WATCH_OBJECT("network/poller/completionProcessRounds", &networkInterface_, &Network::NetworkInterface::pollerCompletionProcessRounds);
	WATCH_OBJECT("network/poller/completionProcessed", &networkInterface_, &Network::NetworkInterface::pollerCompletionProcessedCount);
	WATCH_OBJECT("network/poller/completionLastBatch", &networkInterface_, &Network::NetworkInterface::pollerCompletionLastBatchCount);
	WATCH_OBJECT("network/poller/completionMaxBatch", &networkInterface_, &Network::NetworkInterface::pollerCompletionMaxBatchCount);
	WATCH_OBJECT("network/poller/completionBudgetExhaustions", &networkInterface_, &Network::NetworkInterface::pollerCompletionBudgetExhaustionCount);
	WATCH_OBJECT("network/poller/completionConsecutiveBudgetExhaustions", &networkInterface_, &Network::NetworkInterface::pollerCompletionConsecutiveBudgetExhaustions);
	WATCH_OBJECT("network/poller/completionMaxConsecutiveBudgetExhaustions", &networkInterface_, &Network::NetworkInterface::pollerCompletionMaxConsecutiveBudgetExhaustions);
	// Compare active channels, heap entries, wakeups, and updates together to quantify scheduler aggregation without assuming aligned deadlines.
	// 联合观察活动 Channel、堆项、唤醒和更新次数，用于量化调度聚合效果，不假设各 Channel 截止时间天然对齐。
	WATCH_OBJECT("network/kcp/scheduledChannels", &networkInterface_, &Network::NetworkInterface::kcpScheduledChannelCount);
	WATCH_OBJECT("network/kcp/heapEntries", &networkInterface_, &Network::NetworkInterface::kcpSchedulerHeapEntryCount);
	WATCH_OBJECT("network/kcp/scheduleRequests", &networkInterface_, &Network::NetworkInterface::kcpScheduleRequestCount);
	WATCH_OBJECT("network/kcp/earlierReplacements", &networkInterface_, &Network::NetworkInterface::kcpEarlierReplacementCount);
	WATCH_OBJECT("network/kcp/staleDiscards", &networkInterface_, &Network::NetworkInterface::kcpStaleDiscardCount);
	WATCH_OBJECT("network/kcp/compactions", &networkInterface_, &Network::NetworkInterface::kcpSchedulerCompactionCount);
	WATCH_OBJECT("network/kcp/updateCalls", &networkInterface_, &Network::NetworkInterface::kcpUpdateCallCount);
	WATCH_OBJECT("network/kcp/timerWakeups", &networkInterface_, &Network::NetworkInterface::kcpTimerWakeupCount);
	WATCH_OBJECT("network/kcp/timerRearms", &networkInterface_, &Network::NetworkInterface::kcpTimerRearmCount);
	WATCH_OBJECT("network/kcp/dueChannels", &networkInterface_, &Network::NetworkInterface::kcpDueChannelCount);
	WATCH_OBJECT("network/kcp/overdueChannels", &networkInterface_, &Network::NetworkInterface::kcpOverdueChannelCount);
	WATCH_OBJECT("network/kcp/deadlineMisses", &networkInterface_, &Network::NetworkInterface::kcpDeadlineMissCount);
	WATCH_OBJECT("network/kcp/maxScheduleDelayMicros", &networkInterface_, &Network::NetworkInterface::kcpMaxScheduleDelayMicros);
	WATCH_OBJECT("network/kcp/budgetExhaustions", &networkInterface_, &Network::NetworkInterface::kcpBudgetExhaustionCount);
	WATCH_OBJECT("network/kcp/consecutiveBudgetExhaustions", &networkInterface_, &Network::NetworkInterface::kcpConsecutiveBudgetExhaustions);
	WATCH_OBJECT("network/kcp/maxConsecutiveBudgetExhaustions", &networkInterface_, &Network::NetworkInterface::kcpMaxConsecutiveBudgetExhaustions);
	// waitsnd 指标区分 KCP 自身积压和 completion UDP 队列积压，避免只根据 4 MiB socket backlog 推断根因。
	// The waitsnd metrics separate KCP-owned backlog from the completion UDP queue instead of inferring the cause from a 4 MiB socket backlog alone.
	WATCH_OBJECT("network/kcp/pendingSegments", &networkInterface_, &Network::NetworkInterface::kcpPendingSegmentCount);
	WATCH_OBJECT("network/kcp/queuedSegments", &networkInterface_, &Network::NetworkInterface::kcpQueuedSegmentCount);
	WATCH_OBJECT("network/kcp/unackedSegments", &networkInterface_, &Network::NetworkInterface::kcpUnackedSegmentCount);
	WATCH_OBJECT("network/kcp/acknowledgedSegments", &networkInterface_, &Network::NetworkInterface::kcpAcknowledgedSegmentCount);
	WATCH_OBJECT("network/kcp/retransmissions", &networkInterface_, &Network::NetworkInterface::kcpRetransmissionCount);
	WATCH_OBJECT("network/kcp/timeoutRetransmissions", &networkInterface_, &Network::NetworkInterface::kcpTimeoutRetransmissionCount);
	WATCH_OBJECT("network/kcp/fastRetransmissions", &networkInterface_, &Network::NetworkInterface::kcpFastRetransmissionCount);
	WATCH_OBJECT("network/kcp/acksSent", &networkInterface_, &Network::NetworkInterface::kcpAckSentCount);
	WATCH_OBJECT("network/kcp/acksReceived", &networkInterface_, &Network::NetworkInterface::kcpAckReceivedCount);
	WATCH_OBJECT("network/kcp/maxPendingSegmentsPerChannel", &networkInterface_, &Network::NetworkInterface::kcpMaxPendingSegmentsPerChannel);
	WATCH_OBJECT("network/kcp/sendWindowBlockedChannels", &networkInterface_, &Network::NetworkInterface::kcpSendWindowBlockedChannelCount);
	WATCH_OBJECT("network/kcp/admissionLimitedChannels", &networkInterface_, &Network::NetworkInterface::kcpAdmissionLimitedChannelCount);
	WATCH_OBJECT("network/kcp/remoteWindowZeroChannels", &networkInterface_, &Network::NetworkInterface::kcpRemoteWindowZeroChannelCount);

	return Network::initializeWatcher() && Resmgr::getSingleton().initializeWatcher() &&
		threadPool_.initializeWatcher() && WatchPool::initWatchPools();
}

//-------------------------------------------------------------------------------------		
void ServerApp::queryWatcher(Network::Channel* pChannel, MemoryStream& s)
{
	if(pChannel->isExternal())
		return;
	
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
bool ServerApp::initThreadPool()
{
	if(!threadPool_.isInitialize())
	{
		thread::ThreadPool::timeout = int(g_kbeSrvConfig.thread_timeout_);
		threadPool_.createThreadPool(g_kbeSrvConfig.thread_init_create_, 
			g_kbeSrvConfig.thread_pre_create_, g_kbeSrvConfig.thread_max_create_);

		return true;
	}

	return false;
}

//-------------------------------------------------------------------------------------		
void ServerApp::finalise(void)
{
	ProfileGroup::finalise();
	threadPool_.finalise();
	Network::finalise();
}

//-------------------------------------------------------------------------------------		
double ServerApp::gameTimeInSeconds() const
{
	return double(g_kbetime) / g_kbeSrvConfig.gameUpdateHertz();
}

//-------------------------------------------------------------------------------------
void ServerApp::handleTimeout(TimerHandle, void * arg)
{
}

//-------------------------------------------------------------------------------------
void ServerApp::handleTimers()
{
	AUTO_SCOPED_PROFILE("callScriptTimers");
	timers().process(g_kbetime);
}

//-------------------------------------------------------------------------------------		
bool ServerApp::run(void)
{
	dispatcher_.processUntilBreak();
	return true;
}

//-------------------------------------------------------------------------------------	
void ServerApp::onSignalled(int sigNum)
{
	switch (sigNum)
	{
	case SIGINT:
	case SIGHUP:
		this->shutDown(1.f);
	default:
		break;
	}
}

//-------------------------------------------------------------------------------------	
void ServerApp::onChannelDeregister(Network::Channel * pChannel)
{
	if(pChannel->isInternal())
	{
		Components::getSingleton().onChannelDeregister(pChannel, this->isShuttingdown());
	}
}

//-------------------------------------------------------------------------------------	
void ServerApp::onChannelTimeOut(Network::Channel * pChannel)
{
	INFO_MSG(fmt::format("ServerApp::onChannelTimeOut: "
		"Channel {0} timeout!\n", pChannel->c_str()));

	pChannel->condemn("timedout");
	networkInterface_.deregisterChannel(pChannel);
	pChannel->destroy();
	Network::Channel::reclaimPoolObject(pChannel);
}

//-------------------------------------------------------------------------------------
void ServerApp::onAddComponent(const Components::ComponentInfos* pInfos)
{
	if(pInfos->componentType == LOGGER_TYPE)
	{
		DebugHelper::getSingleton().registerLogger(LoggerInterface::writeLog.msgID, pInfos->pIntAddr.get());
	}
}

//-------------------------------------------------------------------------------------
void ServerApp::onIdentityillegal(COMPONENT_TYPE componentType, COMPONENT_ID componentID, uint32 pid, const char* pAddr)
{
	ERROR_MSG(fmt::format("ServerApp::onIdentityillegal: The current process and {}(componentID={} ->conflicted???, pid={}, addr={}) conflict, the process will exit!\n"
			"Can modify the components-CID and UID to avoid conflict.\n",
		COMPONENT_NAME_EX((COMPONENT_TYPE)componentType), componentID, pid, pAddr));

	this->shutDown(1.f);
}

//-------------------------------------------------------------------------------------
void ServerApp::onRemoveComponent(const Components::ComponentInfos* pInfos)
{
	if(pInfos->componentType == LOGGER_TYPE)
	{
		DebugHelper::getSingleton().unregisterLogger(LoggerInterface::writeLog.msgID, pInfos->pIntAddr.get());
	}
	else if(pInfos->componentType == DBMGR_TYPE)
	{
		if(g_componentType != MACHINE_TYPE && 
			g_componentType != LOGGER_TYPE && 
			g_componentType != INTERFACES_TYPE &&
			g_componentType != BOTS_TYPE &&
			g_componentType != WATCHER_TYPE)
			this->shutDown(0.f);
	}
	else if (pInfos->componentType == CELLAPPMGR_TYPE)
	{
		if (g_componentType == CELLAPP_TYPE)
			this->shutDown(1.f);
	}
	else if (pInfos->componentType == BASEAPPMGR_TYPE)
	{
		if (g_componentType == BASEAPP_TYPE)
			this->shutDown(1.f);
	}
}

//-------------------------------------------------------------------------------------
void ServerApp::onRegisterNewApp(Network::Channel* pChannel, int32 uid, std::string& username, 
						COMPONENT_TYPE componentType, COMPONENT_ID componentID, COMPONENT_ORDER globalorderID, COMPONENT_ORDER grouporderID,
						uint32 intaddr, uint16 intport, uint32 extaddr, uint16 extport, std::string& extaddrEx)
{
	registerNewApp(pChannel, uid, username, componentType, componentID,
		globalorderID, grouporderID, intaddr, intport, extaddr, extport, extaddrEx);
}

//-------------------------------------------------------------------------------------
bool ServerApp::registerNewApp(Network::Channel* pChannel, int32 uid, std::string& username,
						COMPONENT_TYPE componentType, COMPONENT_ID componentID, COMPONENT_ORDER globalorderID, COMPONENT_ORDER grouporderID,
						uint32 intaddr, uint16 intport, uint32 extaddr, uint16 extport, std::string& extaddrEx)
{
	if (pChannel == NULL || pChannel->isExternal() || !VALID_COMPONENT(componentType) ||
		componentID == 0 || uid != KBEngine::getUserUID())
	{
		WARNING_MSG(fmt::format("ServerApp::registerNewApp: rejected uid={}, componentType={}, componentID={}, addr={}.\n",
			uid, componentType, componentID, pChannel != NULL ? pChannel->c_str() : "none"));
		return false;
	}

	// 注册广播中的内部地址会被其他组件直接用于建立 Channel，必须在进入组件表前拒绝空端点。
	// The advertised internal endpoint is used directly to create Channels, so reject empty endpoints before publishing the component.
	if (intaddr == 0 || intport == 0)
	{
		WARNING_MSG(fmt::format("ServerApp::registerNewApp: rejected invalid internal endpoint, componentType={}, componentID={}, intaddr={}, intport={}, addr={} .\n",
			componentType, componentID, intaddr, ntohs(intport), pChannel->c_str()));
		return false;
	}

	INFO_MSG(fmt::format("ServerApp::onRegisterNewApp: uid:{0}, username:{1}, componentType:{2}, "
			"componentID:{3}, globalorderID={9}, grouporderID={10}, intaddr:{4}, intport:{5}, extaddr:{6}, extport:{7},  from {8}.\n",
			uid,
			username.c_str(),
			COMPONENT_NAME_EX((COMPONENT_TYPE)componentType), 
			componentID,
			inet_ntoa((struct in_addr&)intaddr),
			ntohs(intport),
			(extaddr != 0 ? inet_ntoa((struct in_addr&)extaddr) : "nonsupport"),
			ntohs(extport),
			pChannel->c_str(),
			((int32)globalorderID),
			((int32)grouporderID)));

	Components& components = Components::getSingleton();
	Components::ComponentInfos* idOwner = components.findComponent(componentID);
	if (idOwner != NULL && idOwner->componentType != componentType)
	{
		WARNING_MSG(fmt::format("ServerApp::registerNewApp: rejected componentID conflict, requestedType={}, existingType={}, componentID={}, addr={}.\n",
			componentType, idOwner->componentType, componentID, pChannel->c_str()));
		return false;
	}

	Components::ComponentInfos* cinfos = components.findComponent(componentType, uid, componentID);

	if(cinfos == NULL)
	{
		components.addComponent(uid, username.c_str(),
			(KBEngine::COMPONENT_TYPE)componentType, componentID, globalorderID, grouporderID, 0, intaddr, intport, extaddr, extport, extaddrEx, 0,
			0.f, 0.f, 0, 0, 0, 0, 0, pChannel);
	}
	else
	{
		if (!(cinfos->pIntAddr->ip == intaddr && cinfos->pIntAddr->port == intport))
		{
			ERROR_MSG(fmt::format("ServerApp::onRegisterNewApp: error component(uid:{}, username:{}, componentType:{}, componentID:{}, from {})!\n",
				uid,
				username.c_str(),
				COMPONENT_NAME_EX((COMPONENT_TYPE)componentType), componentID, pChannel->c_str()));

			return false;
		}

		if (cinfos->pChannel != NULL && cinfos->pChannel != pChannel &&
			!cinfos->pChannel->isDestroyed())
		{
			// KBE 的组件连接是历史双向连接：本进程可能先通过主动连接建立出站 Channel，
			// 随后再收到对端的入站注册。只有尚未绑定的内部 Channel 才能完成这次合法
			// 反向绑定，并提升为主路由；已经绑定过组件的 Channel 仍然拒绝抢占。
			// KBE keeps a historical bidirectional connection: an outbound Channel may
			// exist before the peer's inbound registration arrives. Only an unbound
			// internal Channel may complete that reverse binding and become authoritative;
			// a Channel already bound to a component cannot replace the live route.
			if (pChannel->componentID() != UNKNOWN_COMPONENT_TYPE)
			{
				WARNING_MSG(fmt::format("ServerApp::registerNewApp: rejected live binding replacement, componentType={}, componentID={}, addr={}.\n",
					componentType, componentID, pChannel->c_str()));
				return false;
			}

			// 反向 Channel 接管主路由后，旧主动 Channel 已不再有任何组件用途；
			// 若继续留在 NetworkInterface，它不会再收到心跳并会被误报为超时退出。
			// Once the reverse Channel owns the route, the old outbound Channel has no
			// remaining component role. Keeping it would stop heartbeats and report a
			// false timeout later, so retire it through the normal maintenance path.
			cinfos->pChannel->destroy();
		}

		cinfos->pChannel = pChannel;
	}

	cinfos = components.findComponent(componentType, uid, componentID);
	if (cinfos == NULL || cinfos->pChannel != pChannel)
	{
		WARNING_MSG(fmt::format("ServerApp::registerNewApp: rejected unbound registration, componentType={}, componentID={}, addr={}.\n",
			componentType, componentID, pChannel->c_str()));
		return false;
	}

	pChannel->componentID(componentID);
	return true;
}

//-------------------------------------------------------------------------------------
void ServerApp::reqKillServer(Network::Channel* pChannel, MemoryStream& s)
{
	if(pChannel->isExternal())
	{
		s.done();
		return;
	}

	COMPONENT_ID componentID;
	COMPONENT_TYPE componentType;
	std::string username;
	int32 uid;
	std::string reason;

	s >> componentID >> componentType >> username >> uid >> reason;

	Components::ComponentInfos* sourceInfos = NULL;
	if (componentType == BASEAPP_TYPE || componentType == CELLAPP_TYPE)
	{
		sourceInfos = Components::getSingleton().findComponent(
			componentType, uid, componentID);
	}

	if (!Security::isBoundComponentSource(componentID, sourceInfos, pChannel))
	{
		WARNING_MSG(fmt::format("ServerApp::reqKillServer: rejected componentType={}, componentID={}, uid={}, usernameSize={}, reasonSize={}, addr={}.\n",
			componentType, componentID, uid, username.size(), reason.size(), pChannel->c_str()));
		s.done();
		return;
	}

	INFO_MSG(fmt::format("ServerApp::reqKillServer: requester(uid:{}, usernameSize:{}, componentType:{}, "
				"componentID:{}, reasonSize:{}, from {})\n",
				uid, 
				username.size(),
				COMPONENT_NAME_EX((COMPONENT_TYPE)componentType),
				componentID,
				reason.size(),
				pChannel->c_str()));

	CRITICAL_MSG("The application was killed!\n");
}

//-------------------------------------------------------------------------------------
void ServerApp::onAppActiveTick(Network::Channel* pChannel, COMPONENT_TYPE componentType, COMPONENT_ID componentID)
{
	if (componentType == CLIENT_TYPE)
	{
		pChannel->updateLastReceivedTime();
		return;
	}

	if (pChannel->isExternal())
		return;

	if (componentType == CONSOLE_TYPE)
	{
		pChannel->updateLastReceivedTime();
		return;
	}

	// A packet may only refresh the component represented by its own Channel.
	// 心跳封包只能刷新其实际 Channel 所绑定的组件，不能替其他组件续命。
	Components::ComponentInfos* sourceInfos = Components::getSingleton().findComponent(
		componentType, KBEngine::getUserUID(), componentID);
	if (!Security::isBoundBidirectionalComponentSource(componentID, sourceInfos, pChannel,
		pChannel->componentID()))
	{
		WARNING_MSG(fmt::format("ServerApp::onAppActiveTick: rejected componentType={}, componentID={}, addr={}.\n",
			componentType, componentID, pChannel->c_str()));
		return;
	}

	pChannel->updateLastReceivedTime();

	//DEBUG_MSG(fmt::format("ServerApp::onAppActiveTick[{:p}]: {}:{} lastReceivedTime:{} at {}.\n",
	//	(void*)pChannel, COMPONENT_NAME_EX(componentType), componentID, pChannel->lastReceivedTime(), pChannel->c_str()));
}

//-------------------------------------------------------------------------------------
void ServerApp::reqClose(Network::Channel* pChannel)
{
	if(pChannel->isExternal())
		return;
	
	DEBUG_MSG(fmt::format("ServerApp::reqClose: {}\n", pChannel->c_str()));
	// this->networkInterface().deregisterChannel(pChannel);
	// pChannel->destroy();
}

//-------------------------------------------------------------------------------------
void ServerApp::lookApp(Network::Channel* pChannel)
{
	if(pChannel->isExternal())
		return;

	//DEBUG_MSG(fmt::format("ServerApp::lookApp: {}, componentID={}\n", pChannel->c_str(), g_componentID));

	Network::Bundle* pBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
	
	(*pBundle) << g_componentType;
	(*pBundle) << componentID_;

	ShutdownHandler::SHUTDOWN_STATE state = shuttingdown();
	int8 istate = int8(state);
	(*pBundle) << istate;

	pChannel->send(pBundle);
	//DEBUG_MSG(fmt::format("ServerApp::lookApp: response! componentID={}\n", g_componentID));
}

//-------------------------------------------------------------------------------------
void ServerApp::reqCloseServer(Network::Channel* pChannel, MemoryStream& s)
{
	if(pChannel->isExternal())
		return;
	
	DEBUG_MSG(fmt::format("ServerApp::reqCloseServer: {}\n", pChannel->c_str()));

	Network::Bundle* pBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
	
	bool success = true;
	(*pBundle) << success;
	pChannel->send(pBundle);
	this->shutDown();
}

//-------------------------------------------------------------------------------------
void ServerApp::queryLoad(Network::Channel* pChannel)
{
	if(pChannel->isExternal())
		return;
}

//-------------------------------------------------------------------------------------
void ServerApp::hello(Network::Channel* pChannel, MemoryStream& s)
{
	std::string verInfo, scriptVerInfo, encryptedKey;

	s >> verInfo >> scriptVerInfo;
	s.readBlob(encryptedKey);

	const size_t encryptedKeyBytes = encryptedKey.size();
	if (encryptedKeyBytes <= 3 || encryptedKeyBytes > 65535)
	{
		encryptedKey.clear();
	}

	INFO_MSG(fmt::format("ServerApp::onHello: verInfo={}, scriptVerInfo={}, encryptedKeyBytes={}, addr:{}\n",
		verInfo, scriptVerInfo, encryptedKeyBytes, pChannel->c_str()));

	if(verInfo != KBEVersion::versionString())
		onVersionNotMatch(pChannel);
	else if(scriptVerInfo != KBEVersion::scriptVersionString())
		onScriptVersionNotMatch(pChannel);
	else
		onHello(pChannel, verInfo, scriptVerInfo, encryptedKey);
}

//-------------------------------------------------------------------------------------
void ServerApp::onHello(Network::Channel* pChannel, 
						const std::string& verInfo, 
						const std::string& scriptVerInfo, 
						const std::string& encryptedKey)
{
}

//-------------------------------------------------------------------------------------
void ServerApp::onVersionNotMatch(Network::Channel* pChannel)
{
}

//-------------------------------------------------------------------------------------
void ServerApp::onScriptVersionNotMatch(Network::Channel* pChannel)
{
}

//-------------------------------------------------------------------------------------
void ServerApp::startProfile(Network::Channel* pChannel, KBEngine::MemoryStream& s)
{
	if(pChannel->isExternal())
		return;
	
	std::string profileName;
	int8 profileType;
	uint32 timelen;

	s >> profileName >> profileType >> timelen;

	startProfile_(pChannel, profileName, profileType, timelen);
}

//-------------------------------------------------------------------------------------
void ServerApp::startProfile_(Network::Channel* pChannel, std::string profileName, int8 profileType, uint32 timelen)
{
	if(pChannel->isExternal())
		return;
	
	switch(profileType)
	{
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
		ERROR_MSG(fmt::format("ServerApp::startProfile_: type({}:{}) not support!\n", 
			profileType, profileName));

		break;
	};
}

//-------------------------------------------------------------------------------------		
}
