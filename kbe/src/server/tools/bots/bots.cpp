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
#include "client_lib/moveto_point_handler.h"
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
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "../../../server/baseapp/baseapp_interface.h"
#include "../../../server/loginapp/loginapp_interface.h"

namespace KBEngine{

namespace
{
uint64 pythonLatencyWindowNanoseconds()
{
	const double defaultSeconds = 10.0;
	const char* configured = std::getenv("KBE_PERF_PYTHON_LATENCY_WINDOW_SECONDS");
	if (configured == NULL || configured[0] == '\0')
		return static_cast<uint64>(defaultSeconds * 1000000000.0);

	char* end = NULL;
	errno = 0;
	const double seconds = std::strtod(configured, &end);
	if (errno != 0 || end == configured || *end != '\0' || !std::isfinite(seconds) ||
		seconds < 1.0 || seconds > 1800.0)
	{
		WARNING_MSG(fmt::format(
			"Bots: ignoring invalid KBE_PERF_PYTHON_LATENCY_WINDOW_SECONDS='{}'\n",
			configured));
		return static_cast<uint64>(defaultSeconds * 1000000000.0);
	}

	return static_cast<uint64>(seconds * 1000000000.0);
}
}

bool g_botsDevMode = false;
bool g_botsReuseAccounts = false;
int8 g_botsTransportOverride = -1;
int8 g_botsAllowTcpFallbackOverride = -1;

bool botsUseTcpTransport()
{
	if (g_botsTransportOverride >= 0)
		return g_botsTransportOverride == 1;

	return g_kbeSrvConfig.getBots().bots_transport == "tcp";
}

bool botsAllowTcpFallback()
{
	if (g_botsAllowTcpFallbackOverride >= 0)
		return g_botsAllowTcpFallbackOverride == 1;

	return g_kbeSrvConfig.getBots().bots_allow_tcp_fallback;
}

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
pTelnetServer_(NULL),
pActiveReportHandler_(NULL),
totalKcpHandshakeSuccesses_(0),
totalKcpHandshakeInvalidPackets_(0),
totalTcpConnections_(0),
totalTcpFallbacks_(0),
totalTransportFailures_(0),
totalNetworkErrors_(0),
totalRemovedClients_(0),
totalDetachedEntities_(0),
totalClearedEntityGarbages_(0),
lastBotsTickMicros_(0),
maxBotsTickMicros_(0),
clientTickBatches_(0),
clientTickMaxBatchMicros_(0),
clientTickBudgetExhaustions_(0),
clientTickCompletedRounds_(0),
clientTickOverdueGameTicks_(0),
clientTickIter_(clients_.end()),
clientTickStartStamps_(0),
clientTickActive_(false),
pythonLatencyEnabled_(false),
pythonLatencySuccesses_(0),
pythonLatencyTimeouts_(0),
pythonLatencyInvalidTimestamps_(0),
pythonLatencyInvalidResponses_(0)
{
	const char* pythonChain = std::getenv("KBE_PERF_PYTHON_CHAIN");
	const char* pythonRtt = std::getenv("KBE_PERF_PYTHON_RTT");
	pythonLatencyEnabled_ = (pythonChain != NULL && std::strcmp(pythonChain, "1") == 0) ||
		(pythonRtt != NULL && std::strcmp(pythonRtt, "1") == 0);
	for (size_t index = 0; index < PYTHON_LATENCY_OPERATION_COUNT; ++index)
	{
		pythonLatencyWindows_[index] = pythonLatencyEnabled_ ? new ProfileLatencyWindow(
			PYTHON_LATENCY_WINDOW_CAPACITY, pythonLatencyWindowNanoseconds()) : NULL;
	}

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
	for (size_t index = 0; index < PYTHON_LATENCY_OPERATION_COUNT; ++index)
		SAFE_RELEASE(pythonLatencyWindows_[index]);

	SAFE_RELEASE(pActiveReportHandler_);
	Components::getSingleton().finalise();
}

//-------------------------------------------------------------------------------------
bool Bots::initialize()
{
	// --dev 是 IDE 集中日志的显式开关。默认不连接 Logger，确保正式
	// Bots 与压测只承担本地日志 IO。
	// --dev explicitly enables centralized IDE logging. Normal Bots and load
	// tests do not connect to Logger and incur local logging cost only.
	if (g_botsDevMode)
	{
		// ClientApp's generic bootstrap does not install a DebugHelper network
		// interface because normal clients never forward logs. Development Bots do.
		// ClientApp 通用启动流程不会设置 DebugHelper 网络接口，因为普通客户端
		// 不转发日志；开发模式 Bots 必须在注册 Logger 前补齐该依赖。
		DebugHelper::getSingleton().pNetworkInterface(&networkInterface());
		if (!Components::getSingleton().findLogger(true))
		{
			WARNING_MSG("Bots::initialize: --dev requested but Logger is unavailable; continuing with local logs.\n");
		}
	}

	// 广播自己的地址给网上上的所有kbemachine
	this->dispatcher().addTask(&Components::getSingleton());
	return ClientApp::initialize() && initializeWatcher();
}

//-------------------------------------------------------------------------------------
bool Bots::initializeWatcher()
{
	WATCH_OBJECT("bots/devMode", g_botsDevMode);
	WATCH_OBJECT("bots/reuseAccounts", g_botsReuseAccounts);
	WATCH_OBJECT("bots/transport/configured", this, &Bots::configuredTransport);
	WATCH_OBJECT("bots/transport/allowTcpFallback", this, &Bots::configuredAllowTcpFallback);
	WATCH_OBJECT("bots/clients/total", this, &Bots::numClients);
	WATCH_OBJECT("bots/clients/kcp", this, &Bots::numKcpClients);
	WATCH_OBJECT("bots/clients/tcp", this, &Bots::numTcpClients);
	WATCH_OBJECT("bots/clients/kcpHandshaking", this, &Bots::numKcpHandshakes);
	WATCH_OBJECT("bots/clients/destroyed", this, &Bots::numDestroyedClients);
	WATCH_OBJECT("bots/totals/kcpHandshakeSuccesses", this, &Bots::totalKcpHandshakeSuccesses);
	WATCH_OBJECT("bots/totals/kcpHandshakeInvalidPackets", this, &Bots::totalKcpHandshakeInvalidPackets);
	WATCH_OBJECT("bots/totals/tcpConnections", this, &Bots::totalTcpConnections);
	WATCH_OBJECT("bots/totals/tcpFallbacks", this, &Bots::totalTcpFallbacks);
	WATCH_OBJECT("bots/totals/transportFailures", this, &Bots::totalTransportFailures);
	WATCH_OBJECT("bots/totals/networkErrors", this, &Bots::totalNetworkErrors);
	WATCH_OBJECT("bots/totals/removedClients", this, &Bots::totalRemovedClients);
	WATCH_OBJECT("bots/totals/detachedEntities", this, &Bots::totalDetachedEntities);
	WATCH_OBJECT("bots/totals/clearedEntityGarbages", this, &Bots::totalClearedEntityGarbages);
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
	WATCH_OBJECT("bots/performance/configuredTransport", this, &Bots::configuredTransport);
	WATCH_OBJECT("bots/performance/allowTcpFallback", this, &Bots::configuredAllowTcpFallback);
	WATCH_OBJECT("bots/performance/clientsDestroyed", this, &Bots::numDestroyedClients);
	// 状态分布直接定位登录流水线积压阶段；Watcher 每五秒查询一次，单次线性扫描不进入游戏 Tick 热路径。
	// State distribution identifies login-pipeline backlog directly; the five-second Watcher query keeps this linear scan off the game-tick hot path.
	WATCH_OBJECT("bots/performance/clientsStateInit", this, &Bots::numClientsInit);
	WATCH_OBJECT("bots/performance/clientsStateCreate", this, &Bots::numClientsCreate);
	WATCH_OBJECT("bots/performance/clientsStateLogin", this, &Bots::numClientsLogin);
	WATCH_OBJECT("bots/performance/clientsStateBaseappCreate", this, &Bots::numClientsBaseappCreate);
	WATCH_OBJECT("bots/performance/clientsStateKcpHandshaking", this, &Bots::numClientsKcpHandshaking);
	WATCH_OBJECT("bots/performance/clientsStateBaseappHello", this, &Bots::numClientsBaseappHello);
	WATCH_OBJECT("bots/performance/clientsStateBaseappLogin", this, &Bots::numClientsBaseappLogin);
	WATCH_OBJECT("bots/performance/clientsStatePlay", this, &Bots::numClientsPlay);
	WATCH_OBJECT("bots/performance/kcpHandshakeSuccesses", this, &Bots::totalKcpHandshakeSuccesses);
	WATCH_OBJECT("bots/performance/kcpHandshakeInvalidPackets", this, &Bots::totalKcpHandshakeInvalidPackets);
	WATCH_OBJECT("bots/performance/tcpFallbacks", this, &Bots::totalTcpFallbacks);
	WATCH_OBJECT("bots/performance/transportFailures", this, &Bots::totalTransportFailures);
	WATCH_OBJECT("bots/performance/networkErrors", this, &Bots::totalNetworkErrors);
	WATCH_OBJECT("bots/performance/removedClients", this, &Bots::totalRemovedClients);
	WATCH_OBJECT("bots/performance/detachedEntities", this, &Bots::totalDetachedEntities);
	WATCH_OBJECT("bots/performance/clearedEntityGarbages", this, &Bots::totalClearedEntityGarbages);
	WATCH_OBJECT("bots/performance/tickLastMicros", this, &Bots::lastBotsTickMicros);
	WATCH_OBJECT("bots/performance/tickMaxMicros", this, &Bots::maxBotsTickMicros);
	WATCH_OBJECT("bots/performance/clientTickBatchSize", this, &Bots::clientTickBatchSize);
	WATCH_OBJECT("bots/performance/clientTickBudgetMicros", this, &Bots::clientTickBudgetMicros);
	WATCH_OBJECT("bots/performance/clientTickBatches", this, &Bots::clientTickBatches);
	WATCH_OBJECT("bots/performance/clientTickMaxBatchMicros", this, &Bots::clientTickMaxBatchMicros);
	WATCH_OBJECT("bots/performance/clientTickBudgetExhaustions", this, &Bots::clientTickBudgetExhaustions);
	WATCH_OBJECT("bots/performance/clientTickCompletedRounds", this, &Bots::clientTickCompletedRounds);
	WATCH_OBJECT("bots/performance/clientTickOverdueGameTicks", this, &Bots::clientTickOverdueGameTicks);
	ClientTickStageMetrics& timerMetrics = clientTickTimerMetrics();
	WATCH_OBJECT("bots/performance/clientTimerCalls", &timerMetrics, &ClientTickStageMetrics::calls);
	WATCH_OBJECT("bots/performance/clientTimerSampledCalls", &timerMetrics, &ClientTickStageMetrics::sampledCalls);
	WATCH_OBJECT("bots/performance/clientTimerSampleRate", &timerMetrics, &ClientTickStageMetrics::sampleRate);
	WATCH_OBJECT("bots/performance/clientTimerSampledAverageNanos", &timerMetrics, &ClientTickStageMetrics::sampledAverageNanos);
	WATCH_OBJECT("bots/performance/clientTimerSampledMaxNanos", &timerMetrics, &ClientTickStageMetrics::sampledMaxNanos);
	WATCH_OBJECT("bots/performance/clientTimerSlowSamplesOver1ms", &timerMetrics, &ClientTickStageMetrics::slowSamplesOver1ms);
	ClientTickStageMetrics& movementMetrics = clientTickMovementSyncMetrics();
	WATCH_OBJECT("bots/performance/clientMovementSyncCalls", &movementMetrics, &ClientTickStageMetrics::calls);
	WATCH_OBJECT("bots/performance/clientMovementSyncSampledCalls", &movementMetrics, &ClientTickStageMetrics::sampledCalls);
	WATCH_OBJECT("bots/performance/clientMovementSyncSampleRate", &movementMetrics, &ClientTickStageMetrics::sampleRate);
	WATCH_OBJECT("bots/performance/clientMovementSyncSampledAverageNanos", &movementMetrics, &ClientTickStageMetrics::sampledAverageNanos);
	WATCH_OBJECT("bots/performance/clientMovementSyncSampledMaxNanos", &movementMetrics, &ClientTickStageMetrics::sampledMaxNanos);
	WATCH_OBJECT("bots/performance/clientMovementSyncSlowSamplesOver1ms", &movementMetrics, &ClientTickStageMetrics::slowSamplesOver1ms);
	// 复用网络层无锁累计计数；压测控制器在进程外计算速率，热路径不增加统计开销。
	// Reuse lock-free network totals; the controller derives rates out of process with no hot-path cost.
	WATCH_OBJECT("bots/performance/numPacketsSent", Network::g_numPacketsSent);
	WATCH_OBJECT("bots/performance/numPacketsReceived", Network::g_numPacketsReceived);
	WATCH_OBJECT("bots/performance/numBytesSent", Network::g_numBytesSent);
	WATCH_OBJECT("bots/performance/numBytesReceived", Network::g_numBytesReceived);
	WATCH_OBJECT("bots/performance/udpSendBacklogBytes", &networkInterface(), &Network::NetworkInterface::pollerUdpSendBacklogBytes);
	WATCH_OBJECT("bots/performance/udpSendBacklogPeakBytes", &networkInterface(), &Network::NetworkInterface::pollerUdpSendBacklogPeakBytes);
	WATCH_OBJECT("bots/performance/udpSendBackpressure", &networkInterface(), &Network::NetworkInterface::pollerUdpSendBackpressureCount);
	// 少量 UDP 通道永久失联时，需要区分内核没有可读数据与 completion read 未能重新投递；这些 getter 只读取聚合计数。
	// When a few UDP channels stop receiving permanently, these aggregate counters distinguish absent kernel data from a failed completion-read rearm.
	WATCH_OBJECT("bots/performance/pendingPollerRearms", &networkInterface(), &Network::NetworkInterface::pendingPollerRearms);
	WATCH_OBJECT("bots/performance/pollerRearmAttempts", &networkInterface(), &Network::NetworkInterface::pollerRearmAttempts);
	WATCH_OBJECT("bots/performance/pollerRearmRetries", &networkInterface(), &Network::NetworkInterface::pollerRearmRetries);
	WATCH_OBJECT("bots/performance/completionProcessed", &networkInterface(), &Network::NetworkInterface::pollerCompletionProcessedCount);
	WATCH_OBJECT("bots/performance/completionLastBatch", &networkInterface(), &Network::NetworkInterface::pollerCompletionLastBatchCount);
	WATCH_OBJECT("bots/performance/completionMaxBatch", &networkInterface(), &Network::NetworkInterface::pollerCompletionMaxBatchCount);
	WATCH_OBJECT("bots/performance/completionBudgetExhaustions", &networkInterface(), &Network::NetworkInterface::pollerCompletionBudgetExhaustionCount);
	WATCH_OBJECT("bots/performance/completionConsecutiveBudgetExhaustions", &networkInterface(), &Network::NetworkInterface::pollerCompletionConsecutiveBudgetExhaustions);
	WATCH_OBJECT("bots/performance/completionMaxConsecutiveBudgetExhaustions", &networkInterface(), &Network::NetworkInterface::pollerCompletionMaxConsecutiveBudgetExhaustions);
	WATCH_OBJECT("bots/performance/completionTimeBudgetExhaustions", &networkInterface(), &Network::NetworkInterface::pollerCompletionTimeBudgetExhaustionCount);
	WATCH_OBJECT("bots/performance/discardedPacketsAfterClose", &networkInterface(), &Network::NetworkInterface::discardedPacketsAfterCloseCount);
	WATCH_OBJECT("bots/performance/contextsOutstandingBytes", &networkInterface(), &Network::NetworkInterface::pollerContextsOutstandingBytes);
	WATCH_OBJECT("bots/performance/contextsCachedBytes", &networkInterface(), &Network::NetworkInterface::pollerContextsCachedBytes);
	// Bots 与服务端共用消息处理分类计数。只注册现有采样统计，Watcher 查询不会扫描
	// ClientObject 或 Channel，可直接定位单个 KCP 重组包中的应用 handler 长尾。
	// Bots shares the server message-category counters. Registering the existing samples
	// does not scan ClientObjects or Channels and exposes handler latency inside KCP packets.
	Network::MessageProcessingMetrics& messageMetrics = Network::MessageHandlers::processingMetrics();
	for (int categoryValue = 0;
		categoryValue < Network::MESSAGE_PROCESSING_CATEGORY_COUNT; ++categoryValue)
	{
		const Network::MessageProcessingCategory category =
			static_cast<Network::MessageProcessingCategory>(categoryValue);
		Network::MessageProcessingCategoryStats* pStats = &messageMetrics.stats(category);
		// Bots performance 由 runner 读取单层目录，分类名必须保持为直属叶子。
		// The runner reads one Bots performance directory level, so categories stay flat leaves.
		const std::string prefix = std::string("bots/performance/messageProcessing_") +
			Network::MessageProcessingMetrics::categoryName(category);
		WATCH_OBJECT(prefix + "Calls", pStats, &Network::MessageProcessingCategoryStats::calls);
		WATCH_OBJECT(prefix + "SampledCalls", pStats, &Network::MessageProcessingCategoryStats::sampledCalls);
		WATCH_OBJECT(prefix + "SampleRate", pStats, &Network::MessageProcessingCategoryStats::sampleRate);
		WATCH_OBJECT(prefix + "SampledTotalNanos", pStats, &Network::MessageProcessingCategoryStats::sampledTotalNanos);
		WATCH_OBJECT(prefix + "SampledAverageNanos", pStats, &Network::MessageProcessingCategoryStats::sampledAverageNanos);
		WATCH_OBJECT(prefix + "SampledMaxNanos", pStats, &Network::MessageProcessingCategoryStats::sampledMaxNanos);
		WATCH_OBJECT(prefix + "SlowSamplesOver1ms", pStats, &Network::MessageProcessingCategoryStats::slowSamplesOver1ms);
		WATCH_OBJECT(prefix + "SlowestHandlerID", pStats, &Network::MessageProcessingCategoryStats::slowestHandlerID);
		WATCH_OBJECT(prefix + "SlowestHandlerName", pStats, &Network::MessageProcessingCategoryStats::slowestHandlerName);
	}
	// KCP 调度与队列指标用于区分空闲定时维护开销和真实业务流量，避免仅凭进程 CPU 猜测热点。
	// KCP scheduler and queue metrics distinguish idle maintenance cost from real traffic instead of inferring hotspots from process CPU alone.
	WATCH_OBJECT("bots/performance/kcpScheduledChannels", &networkInterface(), &Network::NetworkInterface::kcpScheduledChannelCount);
	WATCH_OBJECT("bots/performance/kcpUpdateCalls", &networkInterface(), &Network::NetworkInterface::kcpUpdateCallCount);
	WATCH_OBJECT("bots/performance/kcpTimerWakeups", &networkInterface(), &Network::NetworkInterface::kcpTimerWakeupCount);
	WATCH_OBJECT("bots/performance/kcpTimerRearms", &networkInterface(), &Network::NetworkInterface::kcpTimerRearmCount);
	WATCH_OBJECT("bots/performance/kcpDueChannels", &networkInterface(), &Network::NetworkInterface::kcpDueChannelCount);
	WATCH_OBJECT("bots/performance/kcpOverdueChannels", &networkInterface(), &Network::NetworkInterface::kcpOverdueChannelCount);
	WATCH_OBJECT("bots/performance/kcpDeadlineMisses", &networkInterface(), &Network::NetworkInterface::kcpDeadlineMissCount);
	WATCH_OBJECT("bots/performance/kcpProtocolTickMisses", &networkInterface(), &Network::NetworkInterface::kcpProtocolTickMissCount);
	WATCH_OBJECT("bots/performance/kcpConfiguredTickIntervalMs", &networkInterface(), &Network::NetworkInterface::rudpTickIntervalMs);
	WATCH_OBJECT("bots/performance/kcpConfiguredMinRtoMs", &networkInterface(), &Network::NetworkInterface::rudpMinRtoMs);
	WATCH_OBJECT("bots/performance/kcpMaxScheduleDelayMicros", &networkInterface(), &Network::NetworkInterface::kcpMaxScheduleDelayMicros);
	WATCH_OBJECT("bots/performance/kcpBudgetExhaustions", &networkInterface(), &Network::NetworkInterface::kcpBudgetExhaustionCount);
	WATCH_OBJECT("bots/performance/kcpConsecutiveBudgetExhaustions", &networkInterface(), &Network::NetworkInterface::kcpConsecutiveBudgetExhaustions);
	WATCH_OBJECT("bots/performance/kcpMaxConsecutiveBudgetExhaustions", &networkInterface(), &Network::NetworkInterface::kcpMaxConsecutiveBudgetExhaustions);
	WATCH_OBJECT("bots/performance/kcpTimeBudgetExhaustions", &networkInterface(), &Network::NetworkInterface::kcpTimeBudgetExhaustionCount);
	WATCH_OBJECT("bots/performance/kcpTotalProcessingMicros", &networkInterface(), &Network::NetworkInterface::kcpTotalProcessingMicros);
	WATCH_OBJECT("bots/performance/kcpMaxProcessingMicros", &networkInterface(), &Network::NetworkInterface::kcpMaxProcessingMicros);
	WATCH_OBJECT("bots/performance/kcpPendingSegments", &networkInterface(), &Network::NetworkInterface::kcpPendingSegmentCount);
	WATCH_OBJECT("bots/performance/kcpQueuedSegments", &networkInterface(), &Network::NetworkInterface::kcpQueuedSegmentCount);
	WATCH_OBJECT("bots/performance/kcpUnackedSegments", &networkInterface(), &Network::NetworkInterface::kcpUnackedSegmentCount);
	WATCH_OBJECT("bots/performance/kcpRetransmissions", &networkInterface(), &Network::NetworkInterface::kcpRetransmissionCount);
	WATCH_OBJECT("bots/performance/kcpTimeoutRetransmissions", &networkInterface(), &Network::NetworkInterface::kcpTimeoutRetransmissionCount);
	WATCH_OBJECT("bots/performance/kcpFastRetransmissions", &networkInterface(), &Network::NetworkInterface::kcpFastRetransmissionCount);
	WATCH_OBJECT("bots/performance/kcpAcksSent", &networkInterface(), &Network::NetworkInterface::kcpAckSentCount);
	WATCH_OBJECT("bots/performance/kcpAcksReceived", &networkInterface(), &Network::NetworkInterface::kcpAckReceivedCount);
	WATCH_OBJECT("bots/performance/kcpSendWindowBlockedChannels", &networkInterface(), &Network::NetworkInterface::kcpSendWindowBlockedChannelCount);
	WATCH_OBJECT("bots/performance/kcpAdmissionLimitedChannels", &networkInterface(), &Network::NetworkInterface::kcpAdmissionLimitedChannelCount);
	WATCH_OBJECT("bots/performance/kcpRemoteWindowZeroChannels", &networkInterface(), &Network::NetworkInterface::kcpRemoteWindowZeroChannelCount);
	WATCH_OBJECT("bots/performance/kcpInputErrors", &networkInterface(), &Network::NetworkInterface::kcpInputErrorCount);
	WATCH_OBJECT("bots/performance/kcpInputTooShort", &networkInterface(), &Network::NetworkInterface::kcpInputTooShortCount);
	WATCH_OBJECT("bots/performance/kcpInputConversationMismatches", &networkInterface(), &Network::NetworkInterface::kcpInputConversationMismatchCount);
	WATCH_OBJECT("bots/performance/kcpInputTruncatedSegments", &networkInterface(), &Network::NetworkInterface::kcpInputTruncatedSegmentCount);
	WATCH_OBJECT("bots/performance/kcpInputInvalidCommands", &networkInterface(), &Network::NetworkInterface::kcpInputInvalidCommandCount);
	WATCH_OBJECT("bots/performance/kcpInputOtherErrors", &networkInterface(), &Network::NetworkInterface::kcpInputOtherErrorCount);
	WATCH_OBJECT("bots/performance/kcpReceiveDrainCalls", &networkInterface(), &Network::NetworkInterface::kcpReceiveDrainCallCount);
	WATCH_OBJECT("bots/performance/kcpReceiveDrainedPackets", &networkInterface(), &Network::NetworkInterface::kcpReceiveDrainedPacketCount);
	WATCH_OBJECT("bots/performance/kcpReceiveBudgetYields", &networkInterface(), &Network::NetworkInterface::kcpReceiveBudgetYieldCount);
	WATCH_OBJECT("bots/performance/kcpReceivePendingSegmentsPeak", &networkInterface(), &Network::NetworkInterface::kcpReceivePendingSegmentsPeak);
	WATCH_OBJECT("bots/performance/kcpFixedAllocatedBytes", this, &Bots::kcpFixedAllocatedBytes);
	WATCH_OBJECT("bots/performance/kcpDynamicAllocatedBytes", this, &Bots::kcpDynamicAllocatedBytes);
	WATCH_OBJECT("bots/performance/clientEntities", this, &Bots::numClientEntities);
	WATCH_OBJECT("bots/performance/staleViewMessageDrops", this, &Bots::staleViewMessageDrops);
	// Movement counters distinguish service/network delay from Bots-local timer starvation and
	// excessive script retargeting without adding per-Avatar sampling work.
	// 移动计数用于区分服务端/网络延迟、Bots 本地 Timer 饥饿和脚本频繁换目标，且不增加逐 Avatar 采样。
	WATCH_OBJECT("bots/performance/moveControllerStarts", client::g_moveControllerStarts);
	WATCH_OBJECT("bots/performance/moveControllerReplacements", client::g_moveControllerReplacements);
	WATCH_OBJECT("bots/performance/moveControllerCompletions", client::g_moveControllerCompletions);
	WATCH_OBJECT("bots/performance/moveUpdateCalls", client::g_moveUpdateCalls);
	WATCH_OBJECT("bots/performance/moveDelayedUpdates", client::g_moveDelayedUpdates);
	WATCH_OBJECT("bots/performance/moveSkippedTicks", client::g_moveSkippedTicks);
	WATCH_OBJECT("bots/performance/moveCatchupClamps", client::g_moveCatchupClamps);
	WATCH_OBJECT("bots/performance/moveMaxElapsedMicros", client::g_moveMaxElapsedMicros);
	WATCH_OBJECT("bots/performance/pythonLatencyCount", this, &Bots::pythonPerformanceLatencyCount);
	WATCH_OBJECT("bots/performance/pythonLatencyP99Micros", this, &Bots::pythonPerformanceLatencyP99Micros);
	WATCH_OBJECT("bots/performance/pythonLatencyWindowCount", this, &Bots::pythonPerformanceLatencyWindowCount);
	WATCH_OBJECT("bots/performance/pythonLatencyWindowP99Micros", this, &Bots::pythonPerformanceLatencyWindowP99Micros);
	WATCH_OBJECT("bots/pythonLatency/control/enabled", this, &Bots::pythonLatencyEnabled);
	WATCH_OBJECT("bots/pythonLatency/control/successes", this, &Bots::pythonLatencySuccesses);
	WATCH_OBJECT("bots/pythonLatency/control/timeouts", this, &Bots::pythonLatencyTimeouts);
	WATCH_OBJECT("bots/pythonLatency/control/invalidTimestamps", this, &Bots::pythonLatencyInvalidTimestamps);
	WATCH_OBJECT("bots/pythonLatency/control/invalidResponses", this, &Bots::pythonLatencyInvalidResponses);
	WATCH_OBJECT("bots/pythonLatency/control/successRatePercent", this, &Bots::pythonLatencySuccessRatePercent);
	WATCH_OBJECT("bots/pythonLatency/control/allocatedBytes", this, &Bots::pythonLatencyAllocatedBytes);

#define KBE_WATCH_PYTHON_LATENCY(PATH, SUFFIX) \
	WATCH_OBJECT("bots/pythonLatency/" PATH "/count", this, &Bots::pythonLatency##SUFFIX##Count); \
	WATCH_OBJECT("bots/pythonLatency/" PATH "/meanMicros", this, &Bots::pythonLatency##SUFFIX##MeanMicros); \
	WATCH_OBJECT("bots/pythonLatency/" PATH "/p50Micros", this, &Bots::pythonLatency##SUFFIX##P50Micros); \
	WATCH_OBJECT("bots/pythonLatency/" PATH "/p95Micros", this, &Bots::pythonLatency##SUFFIX##P95Micros); \
	WATCH_OBJECT("bots/pythonLatency/" PATH "/p99Micros", this, &Bots::pythonLatency##SUFFIX##P99Micros); \
	WATCH_OBJECT("bots/pythonLatency/" PATH "/p999Micros", this, &Bots::pythonLatency##SUFFIX##P999Micros); \
	WATCH_OBJECT("bots/pythonLatency/" PATH "/maxMicros", this, &Bots::pythonLatency##SUFFIX##MaxMicros); \
	WATCH_OBJECT("bots/pythonLatency/" PATH "/p999Available", this, &Bots::pythonLatency##SUFFIX##P999Available);

	KBE_WATCH_PYTHON_LATENCY("roundTrip", RoundTrip)
	KBE_WATCH_PYTHON_LATENCY("clientToBase", ClientToBase)
	KBE_WATCH_PYTHON_LATENCY("baseToCell", BaseToCell)
	KBE_WATCH_PYTHON_LATENCY("cellToBase", CellToBase)
	KBE_WATCH_PYTHON_LATENCY("baseToClient", BaseToClient)
#undef KBE_WATCH_PYTHON_LATENCY
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
	// 常驻 Task 每个 dispatcher 周期只运行一个有界批次。与高频 Timer 相比，它不会在
	// IOCP 拥塞时因 Timer 迟到而丢失吞吐，同时每批之后仍必然推进系统 Timer 和网络轮询。
	// A persistent task runs one bounded batch per dispatcher cycle. Unlike a high-frequency
	// timer it does not lose throughput when IOCP delays timers, while timers and network IO
	// still receive an execution opportunity after every batch.
	this->dispatcher().addTask(this);

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
	clientTickActive_ = false;
	this->dispatcher().cancelTask(this);

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
	// time_t t = ::time(NULL);
	// static int kbeTime = 0;
	// DEBUG_MSG(fmt::format("Bots::handleGameTick[{}]:{}\n", t, ++kbeTime));

	ClientApp::handleGameTick();

	// 上一轮尚未完成时不重置游标；这保留公平顺序并显式暴露容量不足，
	// 而不是反复从 map 头部开始导致尾部客户端永久饥饿。
	// Keep the cursor when the previous round is incomplete so tail clients cannot starve.
	if (!clientTickActive_)
	{
		clientTickIter_ = clients_.begin();
		clientTickStartStamps_ = timestamp();
		clientTickActive_ = clientTickIter_ != clients_.end();
	}
	else
	{
		++clientTickOverdueGameTicks_;
	}
}

//-------------------------------------------------------------------------------------
bool Bots::process()
{
	processClientTickBatch();
	return true;
}

//-------------------------------------------------------------------------------------
void Bots::processClientTickBatch()
{
	if (!clientTickActive_)
		return;

	AUTO_SCOPED_PROFILE("updateBotsBatch");
	const uint64 batchStart = timestamp();
	uint32 processed = 0;
	const uint64 budgetStamps = static_cast<uint64>(
		static_cast<double>(clientTickBudgetMicros()) * stampsPerSecondD() / 1000000.0);
	while (clientTickIter_ != clients_.end() && processed < clientTickBatchSize())
	{
		CLIENTS::iterator current = clientTickIter_++;
		Network::Channel* pChannel = current->first;
		ClientObject* pClientObject = current->second;
		if (pClientObject->isDestroyed())
		{
			delClient(pChannel);
			continue;
		}

		pClientObject->gameTick();
		++processed;
		if (timestamp() - batchStart >= budgetStamps)
		{
			++clientTickBudgetExhaustions_;
			break;
		}
	}

	const uint64 batchElapsed = timestamp() - batchStart;
	const uint64 batchMicros = static_cast<uint64>(
		static_cast<double>(batchElapsed) * 1000000.0 / static_cast<double>(stampsPerSecond()));
	clientTickMaxBatchMicros_ = KBE_MAX(clientTickMaxBatchMicros_, batchMicros);
	++clientTickBatches_;

	if (clientTickIter_ == clients_.end())
	{
		clientTickActive_ = false;
		++clientTickCompletedRounds_;
		const uint64 elapsedStamps = timestamp() - clientTickStartStamps_;
		lastBotsTickMicros_ = static_cast<uint64>(
			static_cast<double>(elapsedStamps) * 1000000.0 / static_cast<double>(stampsPerSecond()));
		maxBotsTickMicros_ = KBE_MAX(maxBotsTickMicros_, lastBotsTickMicros_);
	}
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
std::string Bots::configuredTransport() const
{
	return botsUseTcpTransport() ? "tcp" : "kcp";
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
uint32 Bots::numClientsInState(int state) const
{
	uint32 count = 0;
	for (CLIENTS::const_iterator iter = clients_.begin(); iter != clients_.end(); ++iter)
		count += iter->second != NULL && static_cast<int>(iter->second->state()) == state ? 1 : 0;
	return count;
}

//-------------------------------------------------------------------------------------
uint32 Bots::numClientsInit() const { return numClientsInState(ClientObject::C_STATE_INIT); }
uint32 Bots::numClientsCreate() const { return numClientsInState(ClientObject::C_STATE_CREATE); }
uint32 Bots::numClientsLogin() const { return numClientsInState(ClientObject::C_STATE_LOGIN); }
uint32 Bots::numClientsBaseappCreate() const { return numClientsInState(ClientObject::C_STATE_LOGIN_BASEAPP_CREATE); }
uint32 Bots::numClientsKcpHandshaking() const { return numClientsInState(ClientObject::C_STATE_LOGIN_BASEAPP_KCP_HANDSHAKE); }
uint32 Bots::numClientsBaseappHello() const { return numClientsInState(ClientObject::C_STATE_LOGIN_BASEAPP_HELLO); }
uint32 Bots::numClientsBaseappLogin() const { return numClientsInState(ClientObject::C_STATE_LOGIN_BASEAPP); }
uint32 Bots::numClientsPlay() const { return numClientsInState(ClientObject::C_STATE_PLAY); }

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
uint64 Bots::staleViewMessageDrops() const
{
	uint64 count = 0;
	for (CLIENTS::const_iterator iter = clients_.begin(); iter != clients_.end(); ++iter)
	{
		const ClientObject* pClient = iter->second;
		if (pClient != NULL)
			count += pClient->staleViewMessageDrops();
	}
	return count;
}

void Bots::recordPythonPerformanceLatency(uint64 startNs, uint64 endNs)
{
	if (!pythonLatencyEnabled_ || endNs < startNs)
		return;
	pythonLatencyWindows_[PYTHON_LATENCY_ROUND_TRIP]->record(
		endNs - startNs, monotonicNanoseconds());
	++pythonLatencySuccesses_;
}

void Bots::recordPythonPerformanceTransaction(
	uint64 requestID,
	uint64 clientStartedNs,
	uint64 baseReceivedNs,
	uint64 cellReceivedNs,
	uint64 baseReturnedNs,
	uint64 clientCompletedNs)
{
	if (!pythonLatencyEnabled_)
		return;
	if (requestID == 0 || clientStartedNs > baseReceivedNs || baseReceivedNs > cellReceivedNs ||
		cellReceivedNs > baseReturnedNs || baseReturnedNs > clientCompletedNs)
	{
		++pythonLatencyInvalidTimestamps_;
		return;
	}

	// Python perf_counter_ns() supplies comparable segment durations, while the window age must use
	// the engine clock so expiry never depends on platform-specific clock origins.
	// Python perf_counter_ns() 用于计算可比的分段时长；窗口年龄统一使用引擎时钟，避免依赖平台时钟原点。
	const uint64 completedAtNs = monotonicNanoseconds();
	pythonLatencyWindows_[PYTHON_LATENCY_ROUND_TRIP]->record(
		clientCompletedNs - clientStartedNs, completedAtNs);
	pythonLatencyWindows_[PYTHON_LATENCY_CLIENT_TO_BASE]->record(
		baseReceivedNs - clientStartedNs, completedAtNs);
	pythonLatencyWindows_[PYTHON_LATENCY_BASE_TO_CELL]->record(
		cellReceivedNs - baseReceivedNs, completedAtNs);
	pythonLatencyWindows_[PYTHON_LATENCY_CELL_TO_BASE]->record(
		baseReturnedNs - cellReceivedNs, completedAtNs);
	pythonLatencyWindows_[PYTHON_LATENCY_BASE_TO_CLIENT]->record(
		clientCompletedNs - baseReturnedNs, completedAtNs);
	++pythonLatencySuccesses_;
}

void Bots::recordPythonPerformanceProbeTimeout(uint64 requestID)
{
	(void)requestID;
	if (pythonLatencyEnabled_)
		++pythonLatencyTimeouts_;
}

void Bots::recordPythonPerformanceProbeInvalidResponse(uint64 requestID)
{
	(void)requestID;
	if (pythonLatencyEnabled_)
		++pythonLatencyInvalidResponses_;
}

uint64 Bots::pythonPerformanceLatencyCount() const
{
	return pythonLatencySuccesses_;
}

uint64 Bots::pythonPerformanceLatencyP99Micros() const
{
	return static_cast<uint64>(pythonLatencyRoundTripP99Micros());
}

uint64 Bots::pythonPerformanceLatencyWindowCount() const
{
	return pythonLatencyRoundTripCount();
}

uint64 Bots::pythonPerformanceLatencyWindowP99Micros() const
{
	return static_cast<uint64>(pythonLatencyRoundTripP99Micros());
}

uint64 Bots::monotonicNanoseconds()
{
	return static_cast<uint64>(static_cast<long double>(timestamp()) * 1000000000.0L /
		static_cast<long double>(stampsPerSecond()));
}

const ProfileLatencyWindow::Snapshot& Bots::pythonLatencySnapshot(PythonLatencyOperation operation) const
{
	static const ProfileLatencyWindow::Snapshot empty;
	ProfileLatencyWindow* window = pythonLatencyWindows_[operation];
	return window != NULL ? window->snapshot(monotonicNanoseconds()) : empty;
}

uint64 Bots::pythonLatencyAllocatedBytes() const
{
	uint64 bytes = 0;
	for (size_t index = 0; index < PYTHON_LATENCY_OPERATION_COUNT; ++index)
	{
		if (pythonLatencyWindows_[index] != NULL)
			bytes += pythonLatencyWindows_[index]->allocatedBytes();
	}
	return bytes;
}

double Bots::pythonLatencySuccessRatePercent() const
{
	const uint64 completed = pythonLatencySuccesses_ + pythonLatencyTimeouts_;
	return completed > 0 ? static_cast<double>(pythonLatencySuccesses_) * 100.0 /
		static_cast<double>(completed) : 0.0;
}

#define KBE_DEFINE_PYTHON_LATENCY_GETTERS(SUFFIX, OPERATION) \
	uint64 Bots::pythonLatency##SUFFIX##Count() const \
	{ return pythonLatencySnapshot(OPERATION).count; } \
	double Bots::pythonLatency##SUFFIX##MeanMicros() const \
	{ return pythonLatencySnapshot(OPERATION).meanStamps / 1000.0; } \
	double Bots::pythonLatency##SUFFIX##P50Micros() const \
	{ return static_cast<double>(pythonLatencySnapshot(OPERATION).p50Stamps) / 1000.0; } \
	double Bots::pythonLatency##SUFFIX##P95Micros() const \
	{ return static_cast<double>(pythonLatencySnapshot(OPERATION).p95Stamps) / 1000.0; } \
	double Bots::pythonLatency##SUFFIX##P99Micros() const \
	{ return static_cast<double>(pythonLatencySnapshot(OPERATION).p99Stamps) / 1000.0; } \
	double Bots::pythonLatency##SUFFIX##P999Micros() const \
	{ return static_cast<double>(pythonLatencySnapshot(OPERATION).p999Stamps) / 1000.0; } \
	double Bots::pythonLatency##SUFFIX##MaxMicros() const \
	{ return static_cast<double>(pythonLatencySnapshot(OPERATION).maxStamps) / 1000.0; } \
	bool Bots::pythonLatency##SUFFIX##P999Available() const \
	{ return pythonLatencySnapshot(OPERATION).p999Available; }

KBE_DEFINE_PYTHON_LATENCY_GETTERS(RoundTrip, PYTHON_LATENCY_ROUND_TRIP)
KBE_DEFINE_PYTHON_LATENCY_GETTERS(ClientToBase, PYTHON_LATENCY_CLIENT_TO_BASE)
KBE_DEFINE_PYTHON_LATENCY_GETTERS(BaseToCell, PYTHON_LATENCY_BASE_TO_CELL)
KBE_DEFINE_PYTHON_LATENCY_GETTERS(CellToBase, PYTHON_LATENCY_CELL_TO_BASE)
KBE_DEFINE_PYTHON_LATENCY_GETTERS(BaseToClient, PYTHON_LATENCY_BASE_TO_CLIENT)
#undef KBE_DEFINE_PYTHON_LATENCY_GETTERS

//-------------------------------------------------------------------------------------
uint64 Bots::kcpFixedAllocatedBytes() const
{
	return networkInterface().kcpFixedAllocatedBytes();
}

//-------------------------------------------------------------------------------------
uint64 Bots::kcpDynamicAllocatedBytes() const
{
	return networkInterface().kcpDynamicAllocatedBytes();
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
	totalDetachedEntities_ += pClient->detachedEntityCount();
	totalClearedEntityGarbages_ += pClient->clearedEntityGarbageCount();
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
	// Bots 的连接创建/重试会高频复用 Channel 对象；统计扫描前清除失效地址索引。
	// Bots creation and retry reuse Channel objects aggressively; remove stale address entries before metric scans.
	networkInterface().purgeStaleChannelIndexEntries();

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
