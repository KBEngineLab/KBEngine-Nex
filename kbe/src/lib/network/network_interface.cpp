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


#include "network_interface.h"
#include "kcp_send_state.h"
#ifndef CODE_INLINE
#include "network_interface.inl"
#endif

#include "network/address.h"
#include "network/event_dispatcher.h"
#include "network/event_poller.h"
#include "network/packet_receiver.h"
#include "network/listener_receiver.h"
#include "network/listener_udp_receiver.h"
#include "network/channel.h"
#include "network/packet.h"
#include "network/delayed_channels.h"
#include "network/interfaces.h"
#include "network/message_handler.h"

#include <algorithm>

namespace KBEngine {
namespace Network
{
namespace
{
// ikcp.c 将该协议常量保持为私有宏；诊断分类只需要辨别 -1 是短头还是 conv 不匹配。
// ikcp.c keeps this protocol constant private; diagnostics only need it to split a short header from a conv mismatch.
const size_t KCP_INPUT_HEADER_BYTES = 24;
}

//-------------------------------------------------------------------------------------
NetworkInterface::NetworkInterface(Network::EventDispatcher * pDispatcher,
		int32 extlisteningPort_min, int32 extlisteningPort_max, const char * extlisteningInterface,
		uint32 extrbuffer, uint32 extwbuffer,
		int32 intlisteningPort_min, int32 intlisteningPort_max, const char * intlisteningInterface,
		uint32 intrbuffer, uint32 intwbuffer,
		int32 extlisteningUdpPort_min, int32 extlisteningUdpPort_max):
	extEndpoint_(),
	extUdpEndpoint_(),
	intEndpoint_(),
	channelMap_(),
	channelMaintenance_(),
	channelTickEpoch_(0),
	finalizedKcpAckSentCount_(0),
	finalizedKcpAckReceivedCount_(0),
	finalizedKcpTimeoutRetransmissionCount_(0),
	finalizedKcpFastRetransmissionCount_(0),
	finalizedKcpStreamCoalesceCount_(0),
	finalizedKcpStreamCoalescedBytes_(0),
	discardedPacketsAfterCloseCount_(0),
	kcpInputErrorCount_(0),
	kcpInputTooShortCount_(0),
	kcpInputConversationMismatchCount_(0),
	kcpInputTruncatedSegmentCount_(0),
	kcpInputInvalidCommandCount_(0),
	kcpInputOtherErrorCount_(0),
	pDispatcher_(pDispatcher),
	kcpUpdateScheduler_(*pDispatcher),
	pExtListenerReceiver_(NULL),
	pExtUdpListenerReceiver_(NULL),
	pIntListenerReceiver_(NULL),
	pDelayedChannels_(new DelayedChannels()),
	pChannelTimeOutHandler_(NULL),
	pChannelDeregisterHandler_(NULL),
	numExtChannels_(0)
{
	if(extlisteningPort_min != -1)
	{
		pExtListenerReceiver_ = new ListenerReceiver(extEndpoint_, Channel::EXTERNAL, *this);

		this->initialize("EXTERNAL", htons(extlisteningPort_min), htons(extlisteningPort_max),
			extlisteningInterface, &extEndpoint_, pExtListenerReceiver_, extrbuffer, extwbuffer);

		// 如果配置了对外端口范围， 如果范围过小这里extEndpoint_可能没有端口可用了
		if(extlisteningPort_min != -1)
		{
			KBE_ASSERT(extEndpoint_.good() && "Channel::EXTERNAL: no available port, "
				"please check for kbengine[_defs].xml!\n");
		}
	}

	if (extlisteningUdpPort_min != -1)
	{
		pExtUdpListenerReceiver_ = new ListenerUdpReceiver(extUdpEndpoint_, Channel::EXTERNAL, *this);

		this->initialize("EXTERNAL-UDP", htons(extlisteningUdpPort_min), htons(extlisteningUdpPort_max),
			extlisteningInterface, &extUdpEndpoint_, pExtUdpListenerReceiver_, extrbuffer, extwbuffer,
			PROTOCOL_UDP);

		KBE_ASSERT(extUdpEndpoint_.good() && "Channel::EXTERNAL-UDP: no available UDP port, "
			"please check for kbengine[_defs].xml!\n");
	}

	if (intlisteningPort_min != -1)
	{
		pIntListenerReceiver_ = new ListenerReceiver(intEndpoint_, Channel::INTERNAL, *this);

		this->initialize("INTERNAL", htons(intlisteningPort_min), htons(intlisteningPort_max),
			intlisteningInterface, &intEndpoint_, pIntListenerReceiver_, intrbuffer, intwbuffer);
	}

	KBE_ASSERT(good() && "NetworkInterface::NetworkInterface: no available port, "
		"please check for kbengine[_defs].xml!\n");

	pDelayedChannels_->init(this->dispatcher(), this);
}

//-------------------------------------------------------------------------------------
NetworkInterface::~NetworkInterface()
{
	ChannelMap::iterator iter = channelMap_.begin();
	while (iter != channelMap_.end())
	{
		ChannelMap::iterator oldIter = iter++;
		Channel * pChannel = oldIter->second;
		pChannel->destroy();
		delete pChannel;
	}

	channelMap_.clear();
	channelMaintenance_.clear();

	this->closeSocket();

	if (pDispatcher_ != NULL)
	{
		pDelayedChannels_->fini(this->dispatcher());
		pDispatcher_ = NULL;
	}

	SAFE_RELEASE(pDelayedChannels_);
	SAFE_RELEASE(pExtListenerReceiver_);
	SAFE_RELEASE(pExtUdpListenerReceiver_);
	SAFE_RELEASE(pIntListenerReceiver_);
}

//-------------------------------------------------------------------------------------
void NetworkInterface::closeSocket()
{
	if (extEndpoint_.good())
	{
		this->dispatcher().deregisterReadFileDescriptor(extEndpoint_);
		extEndpoint_.close();
	}

	if (extUdpEndpoint_.good())
	{
		this->dispatcher().deregisterReadFileDescriptor(extUdpEndpoint_);
		extUdpEndpoint_.close();
	}

	if (intEndpoint_.good())
	{
		this->dispatcher().deregisterReadFileDescriptor(intEndpoint_);
		intEndpoint_.close();
	}
}

//-------------------------------------------------------------------------------------
void NetworkInterface::cleanupChannel(ChannelMap::iterator iter)
{
	cancelChannelMaintenance(iter->first);
	Channel* pChannel = iter->second;
	channelMap_.erase(iter);

	if (pChannel == NULL)
		return;

	if (pChannel->isExternal())
	{
		KBE_ASSERT(numExtChannels_ > 0);
		--numExtChannels_;
	}

	if (pChannelDeregisterHandler_)
		pChannelDeregisterHandler_->onChannelDeregister(pChannel);

	// map 和上层观察者必须先与 Channel 解耦，再销毁其 socket、poller completion 和 KCP timer，防止回调重新查到半销毁对象。
	// Detach the map and upper-layer observer before releasing the socket, poller completions, and KCP timer so callbacks cannot rediscover a half-destroyed Channel.
	if (!pChannel->isDestroyed())
		pChannel->destroy();

	Network::Channel::reclaimPoolObject(pChannel);
}

//-------------------------------------------------------------------------------------
bool NetworkInterface::initialize(const char* pEndPointName, uint16 listeningPort_min, uint16 listeningPort_max,
										const char * listeningInterface, EndPoint* pEP, ListenerReceiver* pLR, uint32 rbuffer, 
										uint32 wbuffer, ProtocolType protocolType)
{
	KBE_ASSERT(listeningInterface && pEP && pLR);

	if (pEP->good())
	{
		this->dispatcher().deregisterReadFileDescriptor(*pEP);
		pEP->close();
	}

	Address address;
	address.ip = 0;
	address.port = 0;

	pEP->socket(protocolType == PROTOCOL_TCP ? SOCK_STREAM : SOCK_DGRAM);
	if (!pEP->good())
	{
		ERROR_MSG(fmt::format("NetworkInterface::initialize({}): couldn't create a socket\n",
			pEndPointName));

		return false;
	}
	
	if (listeningPort_min > 0 && listeningPort_min == listeningPort_max)
		pEP->setreuseaddr(true);
	
	u_int32_t ifIPAddr = INADDR_ANY;

	bool listeningInterfaceEmpty =
		(listeningInterface == NULL || listeningInterface[0] == 0);

	// 查找指定接口名 NIP、MAC、IP是否可用
	if(pEP->findIndicatedInterface(listeningInterface, ifIPAddr) == 0)
	{
		char szIp[MAX_IP] = {0};
		Address::ip2string(ifIPAddr, szIp);

		INFO_MSG(fmt::format("NetworkInterface::initialize({}): Creating on interface '{}' (= {})\n",
			pEndPointName, listeningInterface, szIp));
	}

	// 如果不为空又找不到那么警告用户错误的设置，同时我们采用默认的方式(绑定到INADDR_ANY)
	else if (!listeningInterfaceEmpty)
	{
		WARNING_MSG(fmt::format("NetworkInterface::initialize({}): Couldn't parse interface spec '{}' so using all interfaces\n",
			pEndPointName, listeningInterface));
	}
	
	// 尝试绑定到端口，如果被占用向后递增
	bool foundport = false;
	uint32 listeningPort = listeningPort_min;
	if(listeningPort_min != listeningPort_max)
	{
		for(int lpIdx=ntohs(listeningPort_min); lpIdx<=ntohs(listeningPort_max); ++lpIdx)
		{
			listeningPort = htons(lpIdx);
			if (pEP->bind(listeningPort, ifIPAddr) != 0)
			{
				continue;
			}
			else
			{
				foundport = true;
				break;
			}
		}
	}
	else
	{
		if (pEP->bind(listeningPort, ifIPAddr) == 0)
		{
			foundport = true;
		}
	}

	// 如果无法绑定到合适的端口那么报错返回，进程将退出
	if(!foundport)
	{
		ERROR_MSG(fmt::format("NetworkInterface::initialize({}): Couldn't bind the socket to {}:{} ({})\n",
			pEndPointName, inet_ntoa((struct in_addr&)ifIPAddr), ntohs(listeningPort), kbe_strerror()));
		
		pEP->close();
		return false;
	}

	// 获得当前绑定的地址，如果是INADDR_ANY这里获得的IP是0
	pEP->getlocaladdress( (u_int16_t*)&address.port,
		(u_int32_t*)&address.ip );

	if (0 == address.ip)
	{
		u_int32_t addr;
		if(0 == pEP->getDefaultInterfaceAddress(addr))
		{
			address.ip = addr;

			char szIp[MAX_IP] = {0};
			Address::ip2string(address.ip, szIp);
			INFO_MSG(fmt::format("NetworkInterface::initialize({}): bound to all interfaces with default route interface on {} ( {} )\n",
				pEndPointName, szIp, address.c_str()));
		}
		else
		{
			ERROR_MSG(fmt::format("NetworkInterface::initialize({}): Couldn't determine ip addr of default interface\n", pEndPointName));

			pEP->close();
			return false;
		}
	}
	
	pEP->setnonblocking(true);
	if (protocolType == PROTOCOL_TCP)
		pEP->setnodelay(true);
	pEP->addr(address);
	
	if(rbuffer > 0)
	{
		if (!pEP->setBufferSize(SO_RCVBUF, rbuffer))
		{
			WARNING_MSG(fmt::format("NetworkInterface::initialize({}): Operating with a receive buffer of only {} bytes (instead of {})\n",
				pEndPointName, pEP->getBufferSize(SO_RCVBUF), rbuffer));
		}
	}
	if(wbuffer > 0)
	{
		if (!pEP->setBufferSize(SO_SNDBUF, wbuffer))
		{
			WARNING_MSG(fmt::format("NetworkInterface::initialize({}): Operating with a send buffer of only {} bytes (instead of {})\n",
				pEndPointName, pEP->getBufferSize(SO_SNDBUF), wbuffer));
		}
	}

	int backlog = 0;
	if (protocolType == PROTOCOL_TCP)
	{
		backlog = Network::g_SOMAXCONN;
		if (backlog < 5)
			backlog = 5;

		if (pEP->listen(backlog) == -1)
		{
			ERROR_MSG(fmt::format("NetworkInterface::initialize({}): listen to {} ({})\n",
				pEndPointName, address.c_str(), kbe_strerror()));

			pEP->close();
			return false;
		}
	}

	// SO_ACCEPTCONN 只有在 listen() 成功后才会把流式 socket 标识为 listener；完成式后端必须在此后识别并投递 accept。
	// SO_ACCEPTCONN identifies a stream socket as a listener only after listen() succeeds; completion backends must detect and arm accept after that point.
	// 延后注册同时避免 io_uring/kqueue 把未监听 socket 缓存为普通 TCP，并保持 IOCP 的 AcceptEx 投递语义一致。
	// Delayed registration also prevents io_uring/kqueue from caching an unbound socket as ordinary TCP and keeps IOCP AcceptEx arming consistent.
	if (!this->dispatcher().registerReadFileDescriptor(*pEP, pLR))
	{
		ERROR_MSG(fmt::format("NetworkInterface::initialize({}): couldn't register the listening socket\n",
			pEndPointName));
		pEP->close();
		return false;
	}

	if (protocolType == PROTOCOL_TCP)
	{
		INFO_MSG(fmt::format("NetworkInterface::initialize({}): address {}, SOMAXCONN={}.\n",
			pEndPointName, address.c_str(), backlog));
	}
	else
	{
		INFO_MSG(fmt::format("NetworkInterface::initialize({}): address {}.\n",
			pEndPointName, address.c_str()));
	}

	return true;
}

//-------------------------------------------------------------------------------------
void NetworkInterface::delayedSend(Channel & channel)
{
	pDelayedChannels_->add(channel);
}

//-------------------------------------------------------------------------------------
void NetworkInterface::sendIfDelayed(Channel & channel)
{
	pDelayedChannels_->sendIfDelayed(channel);
}

//-------------------------------------------------------------------------------------
void NetworkInterface::handleTimeout(TimerHandle handle, void * arg)
{
	INFO_MSG(fmt::format("NetworkInterface::handleTimeout: EXTERNAL({}), INTERNAL({}).\n", 
		extaddr().c_str(), intaddr().c_str()));
}

//-------------------------------------------------------------------------------------
Channel * NetworkInterface::findChannel(const Address & addr)
{
	if (addr.ip == 0)
		return NULL;

	ChannelMap::iterator iter = channelMap_.find(addr);
	if (iter == channelMap_.end())
		return NULL;

	Channel* pChannel = iter->second;
	if (pChannel != NULL && pChannel->isDestroyed())
	{
		cleanupChannel(iter);
		return NULL;
	}

	return pChannel;
}

//-------------------------------------------------------------------------------------
Channel * NetworkInterface::findChannel(KBESOCKET fd)
{
	ChannelMap::iterator iter = channelMap_.begin();
	while (iter != channelMap_.end())
	{
		Channel* pChannel = iter->second;
		if (pChannel != NULL && pChannel->isDestroyed())
		{
			ChannelMap::iterator current = iter++;
			cleanupChannel(current);
			continue;
		}

		if (pChannel != NULL && pChannel->pEndPoint() && *pChannel->pEndPoint() == fd)
			return pChannel;

		++iter;
	}

	return NULL;
}

//-------------------------------------------------------------------------------------
bool NetworkInterface::registerChannel(Channel* pChannel)
{
	return registerChannel(pChannel, false);
}

//-------------------------------------------------------------------------------------
bool NetworkInterface::registerAcceptedChannel(Channel* pChannel)
{
	return registerChannel(pChannel, true);
}

//-------------------------------------------------------------------------------------
bool NetworkInterface::registerChannel(Channel* pChannel, bool replaceExistingAcceptedChannel)
{
	const Address & addr = pChannel->addr();
	KBE_ASSERT(addr.ip != 0);
	KBE_ASSERT(&pChannel->networkInterface() == this);
	ChannelMap::iterator iter = channelMap_.find(addr);
	Channel * pExisting = iter != channelMap_.end() ? iter->second : NULL;

	if(pExisting)
	{
		if (pExisting == pChannel)
		{
			CRITICAL_MSG(fmt::format("NetworkInterface::registerChannel: channel {} is already registered.\n",
				pChannel->c_str()));
			return false;
		}

		const bool endpointInvalid = pExisting->pEndPoint() == NULL || !pExisting->pEndPoint()->good();
		const bool existingClosing = pExisting->isDestroyed() || pExisting->condemn() > 0 || endpointInvalid;
		const bool acceptedTcpReplacement = replaceExistingAcceptedChannel &&
			pExisting->protocoltype() == PROTOCOL_TCP;

		if (!existingClosing && !acceptedTcpReplacement)
		{
			CRITICAL_MSG(fmt::format("NetworkInterface::registerChannel: channel {} is exist.\n",
				pChannel->c_str()));
			return false;
		}

		// 同地址的新连接只替换已关闭状态，或由 accept 证明已经被内核复用四元组的旧 TCP 连接。
		// A same-address connection replaces only a closing entry, or an old TCP connection whose tuple reuse is proven by a successful accept.
		cleanupChannel(iter);
	}

	channelMap_[addr] = pChannel;
	if (pChannel->isDestroyed() || pChannel->condemn() > 0)
		requestChannelMaintenance(pChannel);

	if(pChannel->isExternal())
		numExtChannels_++;

	//INFO_MSG(fmt::format("NetworkInterface::registerChannel: new channel: {}.\n", pChannel->c_str()));
	return true;
}

//-------------------------------------------------------------------------------------
bool NetworkInterface::deregisterAllChannels()
{
	ChannelMap::iterator iter = channelMap_.begin();
	while (iter != channelMap_.end())
	{
		ChannelMap::iterator oldIter = iter++;
		Channel * pChannel = oldIter->second;
		pChannel->destroy();
		Network::Channel::reclaimPoolObject(pChannel);
	}

	channelMap_.clear();
	channelMaintenance_.clear();
	numExtChannels_ = 0;

	return true;
}

//-------------------------------------------------------------------------------------
bool NetworkInterface::deregisterChannel(Channel* pChannel)
{
	const Address & addr = pChannel->addr();
	KBE_ASSERT(pChannel->pEndPoint() != NULL);
	cancelChannelMaintenance(addr);

	if(pChannel->isExternal())
		numExtChannels_--;

	//INFO_MSG(fmt::format("NetworkInterface::deregisterChannel: del channel: {}\n",
	//	pChannel->c_str()));

	if (!channelMap_.erase(addr))
	{
		CRITICAL_MSG(fmt::format("NetworkInterface::deregisterChannel: "
				"Channel not found {}!\n",
			pChannel->c_str()));

		return false;
	}

	if(pChannelDeregisterHandler_)
	{
		pChannelDeregisterHandler_->onChannelDeregister(pChannel);
	}	

	return true;
}

//-------------------------------------------------------------------------------------
void NetworkInterface::onChannelTimeOut(Channel * pChannel)
{
	if (pChannelTimeOutHandler_)
	{
		pChannelTimeOutHandler_->onChannelTimeOut(pChannel);
	}
	else
	{
		ERROR_MSG(fmt::format("NetworkInterface::onChannelTimeOut: "
					"Channel {} timed out but no handler is registered.\n",
				pChannel->c_str()));
	}
}

//-------------------------------------------------------------------------------------
void NetworkInterface::processChannels(KBEngine::Network::MessageHandlers* pMsgHandlers)
{
	(void)pMsgHandlers;

	// epoch 在旧 Tick 结束处前进；之后首次活动的 Channel 会自行清零计数器。
	// Advance the epoch at the old tick boundary; each Channel resets its counters on its first later activity.
	if (++channelTickEpoch_ == 0)
		++channelTickEpoch_;

	ChannelMaintenanceSet pending;
	pending.swap(channelMaintenance_);
	ChannelMaintenanceSet::const_iterator pendingIter = pending.begin();
	for (; pendingIter != pending.end(); ++pendingIter)
	{
		ChannelMap::iterator iter = channelMap_.find(*pendingIter);
		if (iter == channelMap_.end())
			continue;

		Network::Channel* pChannel = iter->second;

		if(pChannel == NULL)
		{
			channelMap_.erase(iter);
		}
		else if(pChannel->isDestroyed())
		{
			cleanupChannel(iter);
		}
		else if(pChannel->condemn() > 0)
		{
			if (pChannel->condemn() == Network::Channel::FLAG_CONDEMN_AND_WAIT_DESTROY && !pChannel->processGracefulClose())
			{
				// 优雅关闭需要在后续 Tick 继续推进，但不能恢复全 Channel 扫描。
				// Graceful close must progress on later ticks without restoring a scan of every Channel.
				channelMaintenance_.insert(*pendingIter);
			}
			else
			{
				cleanupChannel(iter);
			}
		}
	}
}

//-------------------------------------------------------------------------------------
void NetworkInterface::requestChannelMaintenance(Channel* pChannel)
{
	if (pChannel == NULL || pChannel->pEndPoint() == NULL)
		return;

	ChannelMap::iterator iter = channelMap_.find(pChannel->addr());
	if (iter != channelMap_.end() && iter->second == pChannel)
		channelMaintenance_.insert(iter->first);
}

//-------------------------------------------------------------------------------------
void NetworkInterface::cancelChannelMaintenance(const Address& address)
{
	channelMaintenance_.erase(address);
}

//-------------------------------------------------------------------------------------
uint32 NetworkInterface::pendingChannelMaintenanceCount() const
{
	return static_cast<uint32>(channelMaintenance_.size());
}

//-------------------------------------------------------------------------------------
uint32 NetworkInterface::pendingPollerRearms() const
{
	EventPoller* pPoller = pDispatcher_ != NULL ? pDispatcher_->pPoller() : NULL;
	return pPoller != NULL ? pPoller->pendingRearmCount() : 0;
}

//-------------------------------------------------------------------------------------
uint64 NetworkInterface::pollerRearmAttempts() const
{
	EventPoller* pPoller = pDispatcher_ != NULL ? pDispatcher_->pPoller() : NULL;
	return pPoller != NULL ? pPoller->rearmAttemptCount() : 0;
}

//-------------------------------------------------------------------------------------
uint64 NetworkInterface::pollerRearmRetries() const
{
	EventPoller* pPoller = pDispatcher_ != NULL ? pDispatcher_->pPoller() : NULL;
	return pPoller != NULL ? pPoller->rearmRetryCount() : 0;
}

//-------------------------------------------------------------------------------------
#define KBE_POLLER_METRIC(methodName, pollerMethod) \
	uint64 NetworkInterface::methodName() const \
	{ \
		EventPoller* pPoller = pDispatcher_ != NULL ? pDispatcher_->pPoller() : NULL; \
		return pPoller != NULL ? pPoller->pollerMethod() : 0; \
	}

// These accessors keep watcher registration independent from concrete poller types and never scan socket state.
// 这些访问器让 watcher 注册不依赖具体 poller 类型，并且查询时绝不扫描 socket 状态。
KBE_POLLER_METRIC(pollerContextAllocations, contextAllocationCount)
KBE_POLLER_METRIC(pollerContextReuses, contextReuseCount)
KBE_POLLER_METRIC(pollerContextsOutstanding, contextOutstandingCount)
KBE_POLLER_METRIC(pollerContextsCached, contextCachedCount)
KBE_POLLER_METRIC(pollerContextsPeakOutstanding, contextPeakOutstandingCount)
KBE_POLLER_METRIC(pollerContextsOutstandingBytes, contextOutstandingBytes)
KBE_POLLER_METRIC(pollerContextsCachedBytes, contextCachedBytes)
KBE_POLLER_METRIC(pollerTcpSendOwnershipTransfers, tcpSendOwnershipTransferCount)
KBE_POLLER_METRIC(pollerTcpSendBatchCopies, tcpSendBatchCopyCount)
KBE_POLLER_METRIC(pollerTcpSendBatchCopiedBytes, tcpSendBatchCopiedBytes)
KBE_POLLER_METRIC(pollerTcpSendBacklogBytes, tcpSendBacklogBytes)
KBE_POLLER_METRIC(pollerTcpSendBacklogPeakBytes, tcpSendBacklogPeakBytes)
KBE_POLLER_METRIC(pollerTcpSendBackpressureCount, tcpSendBackpressureCount)
KBE_POLLER_METRIC(pollerTcpSendOversizedRejectCount, tcpSendOversizedRejectCount)
KBE_POLLER_METRIC(pollerTcpPartialSendCount, tcpPartialSendCount)
KBE_POLLER_METRIC(pollerReceiveOwnershipTransfers, receiveOwnershipTransferCount)
KBE_POLLER_METRIC(pollerReceiveTransferredBytes, receiveOwnershipTransferredBytes)
KBE_POLLER_METRIC(pollerUdpSendBacklogBytes, udpSendBacklogBytes)
KBE_POLLER_METRIC(pollerUdpSendBacklogPeakBytes, udpSendBacklogPeakBytes)
KBE_POLLER_METRIC(pollerUdpSendBackpressureCount, udpSendBackpressureCount)
KBE_POLLER_METRIC(pollerCompletionProcessRounds, completionProcessRounds)
KBE_POLLER_METRIC(pollerCompletionProcessedCount, completionProcessedCount)
KBE_POLLER_METRIC(pollerCompletionLastBatchCount, completionLastBatchCount)
KBE_POLLER_METRIC(pollerCompletionMaxBatchCount, completionMaxBatchCount)
KBE_POLLER_METRIC(pollerCompletionBudgetExhaustionCount, completionBudgetExhaustionCount)
KBE_POLLER_METRIC(pollerCompletionConsecutiveBudgetExhaustions, completionConsecutiveBudgetExhaustions)
KBE_POLLER_METRIC(pollerCompletionMaxConsecutiveBudgetExhaustions, completionMaxConsecutiveBudgetExhaustions)
KBE_POLLER_METRIC(pollerCompletionTimeBudgetExhaustionCount, completionTimeBudgetExhaustionCount)

#undef KBE_POLLER_METRIC

//-------------------------------------------------------------------------------------
uint64 NetworkInterface::kcpScheduledChannelCount() const { return kcpUpdateScheduler_.scheduledChannelCount(); }
uint64 NetworkInterface::kcpSchedulerHeapEntryCount() const { return kcpUpdateScheduler_.heapEntryCount(); }
uint64 NetworkInterface::kcpScheduleRequestCount() const { return kcpUpdateScheduler_.scheduleRequestCount(); }
uint64 NetworkInterface::kcpEarlierReplacementCount() const { return kcpUpdateScheduler_.earlierReplacementCount(); }
uint64 NetworkInterface::kcpStaleDiscardCount() const { return kcpUpdateScheduler_.staleDiscardCount(); }
uint64 NetworkInterface::kcpSchedulerCompactionCount() const { return kcpUpdateScheduler_.compactionCount(); }
uint64 NetworkInterface::kcpUpdateCallCount() const { return kcpUpdateScheduler_.updateCallCount(); }
uint64 NetworkInterface::kcpTimerWakeupCount() const { return kcpUpdateScheduler_.timerWakeupCount(); }
uint64 NetworkInterface::kcpTimerRearmCount() const { return kcpUpdateScheduler_.timerRearmCount(); }
uint64 NetworkInterface::kcpDueChannelCount() const { return kcpUpdateScheduler_.dueChannelCount(); }
uint64 NetworkInterface::kcpOverdueChannelCount() const { return kcpUpdateScheduler_.overdueChannelCount(); }
uint64 NetworkInterface::kcpDeadlineMissCount() const { return kcpUpdateScheduler_.deadlineMissCount(); }
uint64 NetworkInterface::kcpProtocolTickMissCount() const { return kcpUpdateScheduler_.protocolTickMissCount(); }
uint32 NetworkInterface::rudpTickIntervalMs() const { return g_rudp_tickInterval; }
uint32 NetworkInterface::rudpMinRtoMs() const { return g_rudp_minRTO; }
uint32 NetworkInterface::rudpExternalFlushSegmentsBudget() const { return g_rudp_extFlushSegmentsBudget; }
uint32 NetworkInterface::rudpExternalWriteQueueMaxBytes() const { return g_rudp_extWriteQueueMaxBytes; }
uint64 NetworkInterface::kcpMaxScheduleDelayMicros() const { return kcpUpdateScheduler_.maxScheduleDelayMicros(); }
uint64 NetworkInterface::kcpBudgetExhaustionCount() const { return kcpUpdateScheduler_.budgetExhaustionCount(); }
uint64 NetworkInterface::kcpConsecutiveBudgetExhaustions() const { return kcpUpdateScheduler_.consecutiveBudgetExhaustions(); }
uint64 NetworkInterface::kcpMaxConsecutiveBudgetExhaustions() const { return kcpUpdateScheduler_.maxConsecutiveBudgetExhaustions(); }
uint64 NetworkInterface::kcpTimeBudgetExhaustionCount() const { return kcpUpdateScheduler_.timeBudgetExhaustionCount(); }
uint64 NetworkInterface::kcpTotalProcessingMicros() const { return kcpUpdateScheduler_.totalProcessingMicros(); }
uint64 NetworkInterface::kcpMaxProcessingMicros() const { return kcpUpdateScheduler_.maxProcessingMicros(); }
uint64 NetworkInterface::kcpAckScheduledChannelCount() const { return kcpUpdateScheduler_.ackScheduledChannelCount(); }
uint64 NetworkInterface::kcpAckFlushCallCount() const { return kcpUpdateScheduler_.ackFlushCallCount(); }
uint64 NetworkInterface::kcpAckBudgetExhaustionCount() const { return kcpUpdateScheduler_.ackBudgetExhaustionCount(); }

//-------------------------------------------------------------------------------------
uint64 NetworkInterface::recordKcpInputError(int result, size_t packetLength)
{
	++kcpInputErrorCount_;
	if (result == -1)
	{
		if (packetLength < KCP_INPUT_HEADER_BYTES)
			++kcpInputTooShortCount_;
		else
			++kcpInputConversationMismatchCount_;
	}
	else if (result == -2)
		++kcpInputTruncatedSegmentCount_;
	else if (result == -3)
		++kcpInputInvalidCommandCount_;
	else
		++kcpInputOtherErrorCount_;

	return kcpInputErrorCount_;
}

//-------------------------------------------------------------------------------------
uint64 NetworkInterface::kcpPendingSegmentCount() const
{
	uint64 pendingSegments = 0;
	for (ChannelMap::const_iterator iter = channelMap_.begin(); iter != channelMap_.end(); ++iter)
	{
		const Channel* pChannel = iter->second;
		if (pChannel != NULL && pChannel->pKCP() != NULL)
			pendingSegments += static_cast<uint64>(ikcp_waitsnd(pChannel->pKCP()));
	}
	return pendingSegments;
}

//-------------------------------------------------------------------------------------
uint64 NetworkInterface::kcpQueuedSegmentCount() const
{
	uint64 queuedSegments = 0;
	for (ChannelMap::const_iterator iter = channelMap_.begin(); iter != channelMap_.end(); ++iter)
	{
		const Channel* pChannel = iter->second;
		if (pChannel != NULL && pChannel->pKCP() != NULL)
			queuedSegments += static_cast<uint64>(pChannel->pKCP()->nsnd_que);
	}
	return queuedSegments;
}

//-------------------------------------------------------------------------------------
uint64 NetworkInterface::kcpUnackedSegmentCount() const
{
	uint64 unackedSegments = 0;
	for (ChannelMap::const_iterator iter = channelMap_.begin(); iter != channelMap_.end(); ++iter)
	{
		const Channel* pChannel = iter->second;
		if (pChannel != NULL && pChannel->pKCP() != NULL)
			unackedSegments += static_cast<uint64>(pChannel->pKCP()->nsnd_buf);
	}
	return unackedSegments;
}

//-------------------------------------------------------------------------------------
uint64 NetworkInterface::kcpQueuedPayloadBytes() const
{
	uint64 bytes = 0;
	for (ChannelMap::const_iterator iter = channelMap_.begin(); iter != channelMap_.end(); ++iter)
	{
		const Channel* pChannel = iter->second;
		if (pChannel != NULL && pChannel->pKCP() != NULL)
			bytes += static_cast<uint64>(pChannel->pKCP()->snd_queue_bytes);
	}
	return bytes;
}

//-------------------------------------------------------------------------------------
uint64 NetworkInterface::kcpUnackedPayloadBytes() const
{
	uint64 bytes = 0;
	for (ChannelMap::const_iterator iter = channelMap_.begin(); iter != channelMap_.end(); ++iter)
	{
		const Channel* pChannel = iter->second;
		if (pChannel != NULL && pChannel->pKCP() != NULL)
			bytes += static_cast<uint64>(pChannel->pKCP()->snd_buf_bytes);
	}
	return bytes;
}

//-------------------------------------------------------------------------------------
uint64 NetworkInterface::kcpPendingPayloadBytes() const
{
	return kcpQueuedPayloadBytes() + kcpUnackedPayloadBytes();
}

//-------------------------------------------------------------------------------------
uint64 NetworkInterface::kcpSendBufferMemoryBytes() const
{
	uint64 bytes = 0;
	for (ChannelMap::const_iterator iter = channelMap_.begin(); iter != channelMap_.end(); ++iter)
	{
		const Channel* pChannel = iter->second;
		const ikcpcb* pKcp = pChannel != NULL ? pChannel->pKCP() : NULL;
		if (pKcp != NULL)
		{
			// ikcp_segment_new allocates sizeof(IKCPSEG) plus payload for every
			// queued or unacknowledged send segment. Keep this estimate O(channels).
			// ikcp_segment_new 为每个排队或待确认发送段分配结构体和 payload；
			// 使用增量字节计数，使该估算保持 O(Channel)。
			const uint64 segments = static_cast<uint64>(pKcp->nsnd_que) + pKcp->nsnd_buf;
			bytes += pKcp->snd_queue_bytes + pKcp->snd_buf_bytes +
				segments * static_cast<uint64>(sizeof(IKCPSEG));
		}
	}
	return bytes;
}

//-------------------------------------------------------------------------------------
uint64 NetworkInterface::kcpAverageQueuedPayloadBytes() const
{
	const uint64 segments = kcpQueuedSegmentCount();
	return segments > 0 ? kcpQueuedPayloadBytes() / segments : 0;
}

//-------------------------------------------------------------------------------------
uint64 NetworkInterface::kcpAcknowledgedSegmentCount() const
{
	uint64 acknowledgedSegments = 0;
	for (ChannelMap::const_iterator iter = channelMap_.begin(); iter != channelMap_.end(); ++iter)
	{
		const Channel* pChannel = iter->second;
		if (pChannel != NULL && pChannel->pKCP() != NULL)
			acknowledgedSegments += static_cast<uint64>(pChannel->pKCP()->snd_una);
	}
	return acknowledgedSegments;
}

//-------------------------------------------------------------------------------------
uint64 NetworkInterface::kcpRetransmissionCount() const
{
	return kcpTimeoutRetransmissionCount() + kcpFastRetransmissionCount();
}

//-------------------------------------------------------------------------------------
#define KBE_KCP_CUMULATIVE_METRIC(methodName, fieldName, finalizedFieldName) \
	uint64 NetworkInterface::methodName() const \
	{ \
		uint64 total = finalizedFieldName; \
		for (ChannelMap::const_iterator iter = channelMap_.begin(); iter != channelMap_.end(); ++iter) \
		{ \
			const Channel* pChannel = iter->second; \
			if (pChannel != NULL && pChannel->pKCP() != NULL) \
				total += static_cast<uint64>(pChannel->pKCP()->fieldName); \
		} \
		return total; \
	}

KBE_KCP_CUMULATIVE_METRIC(kcpTimeoutRetransmissionCount, timeout_retransmissions, finalizedKcpTimeoutRetransmissionCount_)
KBE_KCP_CUMULATIVE_METRIC(kcpFastRetransmissionCount, fast_retransmissions, finalizedKcpFastRetransmissionCount_)
KBE_KCP_CUMULATIVE_METRIC(kcpAckSentCount, ack_sent, finalizedKcpAckSentCount_)
KBE_KCP_CUMULATIVE_METRIC(kcpAckReceivedCount, ack_received, finalizedKcpAckReceivedCount_)
KBE_KCP_CUMULATIVE_METRIC(kcpStreamCoalesceCount, stream_coalesces, finalizedKcpStreamCoalesceCount_)
KBE_KCP_CUMULATIVE_METRIC(kcpStreamCoalescedBytes, stream_coalesced_bytes, finalizedKcpStreamCoalescedBytes_)

#undef KBE_KCP_CUMULATIVE_METRIC

//-------------------------------------------------------------------------------------
void NetworkInterface::accumulateFinalizedKcpDiagnostics(uint64 ackSent, uint64 ackReceived,
	uint64 timeoutRetransmissions, uint64 fastRetransmissions,
	uint64 streamCoalesces, uint64 streamCoalescedBytes)
{
	// Channel 和 NetworkInterface 都属于同一 dispatcher 线程；归档销毁连接的计数不需要锁或原子操作。
	// Channel and NetworkInterface share one dispatcher thread, so archiving a closing connection needs no lock or atomic operation.
	finalizedKcpAckSentCount_ += ackSent;
	finalizedKcpAckReceivedCount_ += ackReceived;
	finalizedKcpTimeoutRetransmissionCount_ += timeoutRetransmissions;
	finalizedKcpFastRetransmissionCount_ += fastRetransmissions;
	finalizedKcpStreamCoalesceCount_ += streamCoalesces;
	finalizedKcpStreamCoalescedBytes_ += streamCoalescedBytes;
}

//-------------------------------------------------------------------------------------
uint64 NetworkInterface::kcpMaxPendingSegmentsPerChannel() const
{
	uint64 maxPendingSegments = 0;
	for (ChannelMap::const_iterator iter = channelMap_.begin(); iter != channelMap_.end(); ++iter)
	{
		const Channel* pChannel = iter->second;
		if (pChannel != NULL && pChannel->pKCP() != NULL)
		{
			maxPendingSegments = std::max<uint64>(maxPendingSegments,
				static_cast<uint64>(ikcp_waitsnd(pChannel->pKCP())));
		}
	}
	return maxPendingSegments;
}

//-------------------------------------------------------------------------------------
uint64 NetworkInterface::kcpSendWindowBlockedChannelCount() const
{
	uint64 blockedChannels = 0;
	for (ChannelMap::const_iterator iter = channelMap_.begin(); iter != channelMap_.end(); ++iter)
	{
		const Channel* pChannel = iter->second;
		const ikcpcb* pKcp = pChannel != NULL ? pChannel->pKCP() : NULL;
		if (pKcp != NULL && KcpSendState(pKcp->nsnd_que, pKcp->nsnd_buf,
			pKcp->snd_wnd, pKcp->rmt_wnd, pKcp->cwnd, pKcp->nocwnd == 0).isWindowBlocked())
			++blockedChannels;
	}
	return blockedChannels;
}

//-------------------------------------------------------------------------------------
uint64 NetworkInterface::kcpAdmissionLimitedChannelCount() const
{
	uint64 limitedChannels = 0;
	for (ChannelMap::const_iterator iter = channelMap_.begin(); iter != channelMap_.end(); ++iter)
	{
		const Channel* pChannel = iter->second;
		const ikcpcb* pKcp = pChannel != NULL ? pChannel->pKCP() : NULL;
		if (pKcp != NULL && KcpSendState(pKcp->nsnd_que, pKcp->nsnd_buf,
			pKcp->snd_wnd, pKcp->rmt_wnd, pKcp->cwnd, pKcp->nocwnd == 0,
			pKcp->snd_queue_bytes + pKcp->snd_buf_bytes,
			pChannel->isExternal() ? g_rudp_extWriteQueueMaxBytes : 0).isAdmissionLimited())
		{
			++limitedChannels;
		}
	}
	return limitedChannels;
}

//-------------------------------------------------------------------------------------
uint64 NetworkInterface::kcpRemoteWindowZeroChannelCount() const
{
	uint64 zeroWindowChannels = 0;
	for (ChannelMap::const_iterator iter = channelMap_.begin(); iter != channelMap_.end(); ++iter)
	{
		const Channel* pChannel = iter->second;
		if (pChannel != NULL && pChannel->pKCP() != NULL && pChannel->pKCP()->rmt_wnd == 0)
			++zeroWindowChannels;
	}
	return zeroWindowChannels;
}

//-------------------------------------------------------------------------------------
uint32 NetworkInterface::numExternalTcpChannels() const
{
	uint32 count = 0;
	for (ChannelMap::const_iterator iter = channelMap_.begin(); iter != channelMap_.end(); ++iter)
	{
		const Channel* pChannel = iter->second;
		if (pChannel && pChannel->isExternal() && pChannel->protocoltype() == PROTOCOL_TCP &&
			pChannel->type() != Channel::CHANNEL_WEB)
		{
			++count;
		}
	}

	return count;
}

//-------------------------------------------------------------------------------------
uint32 NetworkInterface::numExternalWebSocketChannels() const
{
	uint32 count = 0;
	for (ChannelMap::const_iterator iter = channelMap_.begin(); iter != channelMap_.end(); ++iter)
	{
		const Channel* pChannel = iter->second;
		if (pChannel && pChannel->isExternal() && pChannel->type() == Channel::CHANNEL_WEB)
			++count;
	}

	return count;
}

//-------------------------------------------------------------------------------------
uint32 NetworkInterface::numExternalKcpChannels() const
{
	uint32 count = 0;
	for (ChannelMap::const_iterator iter = channelMap_.begin(); iter != channelMap_.end(); ++iter)
	{
		const Channel* pChannel = iter->second;
		if (pChannel && pChannel->isExternal() && pChannel->protocolSubtype() == SUB_PROTOCOL_KCP)
			++count;
	}

	return count;
}

//-------------------------------------------------------------------------------------
uint32 NetworkInterface::numExternalUdpChannels() const
{
	uint32 count = 0;
	for (ChannelMap::const_iterator iter = channelMap_.begin(); iter != channelMap_.end(); ++iter)
	{
		const Channel* pChannel = iter->second;
		if (pChannel && pChannel->isExternal() && pChannel->protocoltype() == PROTOCOL_UDP &&
			pChannel->protocolSubtype() != SUB_PROTOCOL_KCP)
		{
			++count;
		}
	}

	return count;
}

//-------------------------------------------------------------------------------------
uint32 NetworkInterface::numExternalKcpControlBlocks() const
{
	uint32 count = 0;
	for (ChannelMap::const_iterator iter = channelMap_.begin(); iter != channelMap_.end(); ++iter)
	{
		const Channel* pChannel = iter->second;
		if (pChannel && pChannel->isExternal() && pChannel->pKCP())
			++count;
	}

	return count;
}

//-------------------------------------------------------------------------------------
uint32 NetworkInterface::numExternalKcpUpdateTimers() const
{
	uint32 count = 0;
	for (ChannelMap::const_iterator iter = channelMap_.begin(); iter != channelMap_.end(); ++iter)
	{
		const Channel* pChannel = iter->second;
		if (pChannel && pChannel->isExternal() && pChannel->hasKcpUpdateTimer())
			++count;
	}

	return count;
}
//-------------------------------------------------------------------------------------
}
}
