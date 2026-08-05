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

#ifndef KBE_NETWORK_INTERFACE_H
#define KBE_NETWORK_INTERFACE_H

#include "common/memorystream.h"
#include "network/common.h"
#include "common/common.h"
#include "common/timer.h"
#include "helper/debug_helper.h"
#include "network/endpoint.h"
#include "network/kcp_update_scheduler.h"

namespace KBEngine { 
namespace Network
{
class Address;
class Bundle;
class Channel;
class ChannelTimeOutHandler;
class ChannelDeregisterHandler;
class DelayedChannels;
class ListenerReceiver;
class Packet;
class EventDispatcher;
class MessageHandlers;

// Channel 使用对象池后，裸指针地址会被后续会话复用。索引必须同时保存会话代次，
// 否则旧地址条目可能把新对象误判成原连接并在 Watcher 扫描时解引用错误的传输状态。
// Channel addresses are reused by the object pool, so the index must snapshot the
// session generation or a stale entry may treat a later object as the old connection.
struct ChannelIndexEntry
{
	ChannelIndexEntry(Channel* channel = NULL, uint64 epoch = 0) :
		pChannel(channel), sessionEpoch(epoch) {}

	bool matches(const Channel* channel, uint64 epoch) const
	{
		return pChannel == channel && sessionEpoch != 0 && sessionEpoch == epoch;
	}

	operator Channel*() const { return pChannel; }

	Channel* pChannel;
	uint64 sessionEpoch;
};

class NetworkInterface : public TimerHandler
{
public:
	typedef std::map<Address, ChannelIndexEntry>	ChannelMap;
	typedef std::set<Address>			ChannelMaintenanceSet;
	
	NetworkInterface(EventDispatcher * pDispatcher,
		int32 extlisteningPort_min = -1, int32 extlisteningPort_max = -1, const char * extlisteningInterface = "",
		uint32 extrbuffer = 0, uint32 extwbuffer = 0,
		int32 intlisteningPort_min = 0, int32 intlisteningPort_max = 0, const char * intlisteningInterface = "",
		uint32 intrbuffer = 0, uint32 intwbuffer = 0,
		int32 extlisteningUdpPort_min = -1, int32 extlisteningUdpPort_max = -1);

	~NetworkInterface();

	INLINE const Address & extaddr() const;
	INLINE const Address & extUdpAddr() const;
	INLINE const Address & intaddr() const;

	bool initialize(const char* pEndPointName, uint16 listeningPort_min, uint16 listeningPort_max,
		const char * listeningInterface, EndPoint* pEP, ListenerReceiver* pLR, uint32 rbuffer = 0,
		uint32 wbuffer = 0, ProtocolType protocolType = PROTOCOL_TCP);

	bool registerChannel(Channel* pChannel);
	// listener 接受的新 TCP 连接可以替换同一对端地址的旧 Channel，用于处理内核先复用四元组、主线稍后消费断开 completion 的时序。
	// A newly accepted TCP connection may replace an old Channel for the same peer when the kernel reuses the tuple before the main thread consumes the terminal completion.
	bool registerAcceptedChannel(Channel* pChannel);
	bool deregisterChannel(Channel* pChannel);
	bool deregisterAllChannels();
	uint32 purgeStaleChannelIndexEntries();
	Channel * findChannel(const Address & addr);
	Channel * findChannel(KBESOCKET fd);

	ChannelTimeOutHandler * pChannelTimeOutHandler() const
		{ return pChannelTimeOutHandler_; }
	void pChannelTimeOutHandler(ChannelTimeOutHandler * pHandler)
		{ pChannelTimeOutHandler_ = pHandler; }
		
	ChannelDeregisterHandler * pChannelDeregisterHandler() const
		{ return pChannelDeregisterHandler_; }
	void pChannelDeregisterHandler(ChannelDeregisterHandler * pHandler)
		{ pChannelDeregisterHandler_ = pHandler; }

	EventDispatcher & dispatcher()		{ return *pDispatcher_; }

	/* 外部网点和内部网点 */
	EndPoint & extEndpoint()				{ return extEndpoint_; }
	EndPoint & extUdpEndpoint()			{ return extUdpEndpoint_; }
	EndPoint & intEndpoint()				{ return intEndpoint_; }

	const char * c_str() const { return extEndpoint_.c_str(); }
	
	const ChannelMap& channels(void) const { return channelMap_; }
		
	/** 发送相关 */
	void sendIfDelayed(Channel & channel);
	void delayedSend(Channel & channel);
	
	bool good() const
	{
		return (!pExtListenerReceiver_ || extEndpoint_.good()) &&
			(!pExtUdpListenerReceiver_ || extUdpEndpoint_.good()) && intEndpoint_.good();
	}

	void onChannelTimeOut(Channel * pChannel);
	
	/* 
		处理所有channels  
	*/
	void processChannels(KBEngine::Network::MessageHandlers* pMsgHandlers);

	INLINE int32 numExtChannels() const;
	// 这些只读统计仅在 watcher 查询时遍历 ChannelMap，不给网络 Tick 增加持续计数开销，也不会复制 Channel 生命周期状态。
	// These read-only statistics scan ChannelMap only for watcher queries, adding no continuous accounting cost to the network tick and duplicating no Channel lifecycle state.
	uint32 numExternalTcpChannels() const;
	uint32 numExternalWebSocketChannels() const;
	uint32 numExternalKcpChannels() const;
	uint32 numExternalUdpChannels() const;
	uint32 numExternalKcpControlBlocks() const;
	uint32 numExternalKcpUpdateTimers() const;
	uint32 pendingChannelMaintenanceCount() const;
	uint32 pendingPollerRearms() const;
	uint64 pollerRearmAttempts() const;
	uint64 pollerRearmRetries() const;
	uint64 pollerContextAllocations() const;
	uint64 pollerContextReuses() const;
	uint64 pollerContextsOutstanding() const;
	uint64 pollerContextsCached() const;
	uint64 pollerContextsPeakOutstanding() const;
	uint64 pollerContextsOutstandingBytes() const;
	uint64 pollerContextsCachedBytes() const;
	uint64 pollerTcpSendOwnershipTransfers() const;
	uint64 pollerTcpSendBatchCopies() const;
	uint64 pollerTcpSendBatchCopiedBytes() const;
	uint64 pollerTcpSendBacklogBytes() const;
	uint64 pollerTcpSendBacklogPeakBytes() const;
	uint64 pollerTcpSendBackpressureCount() const;
	uint64 pollerTcpSendOversizedRejectCount() const;
	uint64 pollerTcpPartialSendCount() const;
	uint64 pollerReceiveOwnershipTransfers() const;
	uint64 pollerReceiveTransferredBytes() const;
	uint64 pollerUdpSendBacklogBytes() const;
	uint64 pollerUdpSendBacklogPeakBytes() const;
	uint64 pollerUdpSendBackpressureCount() const;
	uint64 pollerCompletionProcessRounds() const;
	uint64 pollerCompletionProcessedCount() const;
	uint64 pollerCompletionLastBatchCount() const;
	uint64 pollerCompletionMaxBatchCount() const;
	uint64 pollerCompletionBudgetExhaustionCount() const;
	uint64 pollerCompletionConsecutiveBudgetExhaustions() const;
	uint64 pollerCompletionMaxConsecutiveBudgetExhaustions() const;
	uint64 pollerCompletionTimeBudgetExhaustionCount() const;
	uint64 pollerCompletionDequeueCallCount() const;
	uint64 pollerCompletionDequeuedCount() const;
	uint64 pollerCompletionMaxDequeuedBatchCount() const;
	uint64 pollerCompletionPendingLocalCount() const;
	uint64 discardedPacketsAfterCloseCount() const { return discardedPacketsAfterCloseCount_; }
	uint64 receiveWindowOverflowBurstCount() const { return receiveWindowOverflowBurstCount_; }
	uint64 receiveWindowCriticalBurstCount() const { return receiveWindowCriticalBurstCount_; }
	uint64 receiveWindowOverflowDeferredCount() const { return receiveWindowOverflowDeferredCount_; }
	uint64 receiveWindowOverflowCondemnedCount() const { return receiveWindowOverflowCondemnedCount_; }
	uint64 receiveWindowMaxMessagesPerTick() const { return receiveWindowMaxMessagesPerTick_; }
	uint64 receiveWindowMaxBytesPerTick() const { return receiveWindowMaxBytesPerTick_; }
	uint64 kcpReceiveDrainCallCount() const { return kcpReceiveDrainCallCount_; }
	uint64 kcpReceiveDrainedPacketCount() const { return kcpReceiveDrainedPacketCount_; }
	uint64 kcpReceiveBudgetYieldCount() const { return kcpReceiveBudgetYieldCount_; }
	uint64 kcpReceivePendingSegmentsPeak() const { return kcpReceivePendingSegmentsPeak_; }
	void recordKcpReceiveDrain(uint32 processedPackets, uint32 pendingSegments, bool budgetYield);
	uint64 channelIndexMismatchCount() const { return channelIndexMismatchCount_; }
	uint64 kcpScheduledChannelCount() const;
	uint64 kcpSchedulerHeapEntryCount() const;
	uint64 kcpScheduleRequestCount() const;
	uint64 kcpEarlierReplacementCount() const;
	uint64 kcpStaleDiscardCount() const;
	uint64 kcpSchedulerCompactionCount() const;
	uint64 kcpUpdateCallCount() const;
	uint64 kcpTimerWakeupCount() const;
	uint64 kcpTimerRearmCount() const;
	uint64 kcpDueChannelCount() const;
	uint64 kcpOverdueChannelCount() const;
	uint64 kcpDeadlineMissCount() const;
	uint64 kcpProtocolTickMissCount() const;
	uint32 rudpTickIntervalMs() const;
	uint32 rudpMinRtoMs() const;
	uint32 rudpExternalFlushSegmentsBudget() const;
	uint32 rudpExternalWriteQueueMaxBytes() const;
	uint64 kcpMaxScheduleDelayMicros() const;
	uint64 kcpBudgetExhaustionCount() const;
	uint64 kcpConsecutiveBudgetExhaustions() const;
	uint64 kcpMaxConsecutiveBudgetExhaustions() const;
	uint64 kcpTimeBudgetExhaustionCount() const;
	uint64 kcpTotalProcessingMicros() const;
	uint64 kcpMaxProcessingMicros() const;
	uint64 kcpAckScheduledChannelCount() const;
	uint64 kcpAckScheduleRequestCount() const;
	uint64 kcpAckFlushCallCount() const;
	uint64 kcpAckBudgetExhaustionCount() const;
	uint64 kcpAckTotalProcessingMicros() const;
	uint64 kcpAckMaxProcessingMicros() const;
	uint64 kcpDataTotalProcessingMicros() const;
	uint64 kcpDataMaxProcessingMicros() const;
	uint64 kcpPendingSegmentCount() const;
	uint64 kcpQueuedSegmentCount() const;
	uint64 kcpUnackedSegmentCount() const;
	uint64 kcpPendingPayloadBytes() const;
	uint64 kcpQueuedPayloadBytes() const;
	uint64 kcpUnackedPayloadBytes() const;
	uint64 kcpSendBufferMemoryBytes() const;
	uint64 kcpAverageQueuedPayloadBytes() const;
	uint64 kcpStreamCoalesceCount() const;
	uint64 kcpStreamCoalescedBytes() const;
	uint64 kcpAcknowledgedSegmentCount() const;
	uint64 kcpRetransmissionCount() const;
	uint64 kcpTimeoutRetransmissionCount() const;
	uint64 kcpFastRetransmissionCount() const;
	uint64 kcpAckSentCount() const;
	uint64 kcpAckReceivedCount() const;
	uint64 kcpFlushCallCount() const;
	uint64 kcpFlushScannedSegmentCount() const;
	uint64 kcpFlushDataSegmentCount() const;
	uint64 kcpFlushEmptyDataCallCount() const;
	uint64 kcpAckOutputCallCount() const;
	uint64 kcpAckOutputByteCount() const;
	uint64 kcpDataOutputCallCount() const;
	uint64 kcpDataOutputByteCount() const;
	uint64 kcpSendtoSampleCallCount() const;
	uint64 kcpSendtoSampleTotalMicros() const;
	uint64 kcpSendtoSampleMaxMicros() const;
	uint64 kcpMaxPendingSegmentsPerChannel() const;
	uint64 kcpSendWindowBlockedChannelCount() const;
	uint64 kcpAdmissionLimitedChannelCount() const;
	uint64 kcpRemoteWindowZeroChannelCount() const;
	uint64 kcpFixedAllocatedBytes() const;
	uint64 kcpDynamicAllocatedBytes() const;
	uint64 kcpInputErrorCount() const { return kcpInputErrorCount_; }
	uint64 kcpInputTooShortCount() const { return kcpInputTooShortCount_; }
	uint64 kcpInputConversationMismatchCount() const { return kcpInputConversationMismatchCount_; }
	uint64 kcpInputTruncatedSegmentCount() const { return kcpInputTruncatedSegmentCount_; }
	uint64 kcpInputInvalidCommandCount() const { return kcpInputInvalidCommandCount_; }
	uint64 kcpInputOtherErrorCount() const { return kcpInputOtherErrorCount_; }
	// KCP 输入校验和网络 dispatcher 在同一线程执行，使用普通整数即可避免原子操作污染收包热路径。
	// KCP input validation runs on the network dispatcher thread, so ordinary integers avoid atomics on the receive hot path.
	uint64 recordKcpInputError(int result, size_t packetLength);

private:
	friend class Channel;

	virtual void handleTimeout(TimerHandle handle, void * arg);

	void closeSocket();
	uint64 kcpSendtoSampleTotalStamps() const;

	struct KcpWatcherSnapshot
	{
		uint64 pendingSegments;
		uint64 queuedSegments;
		uint64 unackedSegments;
		uint64 queuedPayloadBytes;
		uint64 unackedPayloadBytes;
		uint64 sendBufferMemoryBytes;
		uint64 acknowledgedSegments;
		uint64 timeoutRetransmissions;
		uint64 fastRetransmissions;
		uint64 ackSent;
		uint64 ackReceived;
		uint64 streamCoalesces;
		uint64 streamCoalescedBytes;
		uint64 flushCalls;
		uint64 flushScannedSegments;
		uint64 flushDataSegments;
		uint64 flushEmptyDataCalls;
		uint64 ackOutputCalls;
		uint64 ackOutputBytes;
		uint64 dataOutputCalls;
		uint64 dataOutputBytes;
		uint64 sendtoSampleCalls;
		uint64 sendtoSampleStamps;
		uint64 sendtoMaxSampleStamps;
		uint64 maxPendingSegmentsPerChannel;
		uint64 sendWindowBlockedChannels;
		uint64 admissionLimitedChannels;
		uint64 remoteWindowZeroChannels;
		uint64 fixedAllocatedBytes;
		uint64 dynamicAllocatedBytes;
	};

	const KcpWatcherSnapshot& kcpWatcherSnapshot() const;
	void cleanupChannel(ChannelMap::iterator iter);
	const Channel* currentRegisteredChannel(ChannelMap::const_iterator iter) const;
	bool registerChannel(Channel* pChannel, bool replaceExistingAcceptedChannel);
	void requestChannelMaintenance(Channel* pChannel);
	void cancelChannelMaintenance(const Address& address);
	void accumulateFinalizedKcpDiagnostics(uint64 ackSent, uint64 ackReceived,
		uint64 timeoutRetransmissions, uint64 fastRetransmissions,
		uint64 streamCoalesces, uint64 streamCoalescedBytes,
		uint64 flushCalls, uint64 flushScannedSegments, uint64 flushDataSegments,
		uint64 flushEmptyDataCalls, uint64 ackOutputCalls, uint64 ackOutputBytes,
		uint64 dataOutputCalls, uint64 dataOutputBytes, uint64 sendtoSampleCalls,
		uint64 sendtoSampleStamps, uint64 sendtoMaxSampleStamps);
	void recordDiscardedPacketAfterClose() { ++discardedPacketsAfterCloseCount_; }
	void recordReceiveWindowOverflow(bool deferred, bool condemned)
	{
		++receiveWindowOverflowBurstCount_;
		if (deferred)
			++receiveWindowOverflowDeferredCount_;
		if (condemned)
			++receiveWindowOverflowCondemnedCount_;
	}
	void recordReceiveWindowActivity(uint32 messages, uint32 bytes)
	{
		receiveWindowMaxMessagesPerTick_ = std::max<uint64>(receiveWindowMaxMessagesPerTick_, messages);
		receiveWindowMaxBytesPerTick_ = std::max<uint64>(receiveWindowMaxBytesPerTick_, bytes);
	}
	void recordReceiveWindowCriticalBurst() { ++receiveWindowCriticalBurstCount_; }
	uint64 channelTickEpoch() const { return channelTickEpoch_; }

private:
	EndPoint								extEndpoint_, extUdpEndpoint_, intEndpoint_;

	ChannelMap								channelMap_;
	// 只有进入关闭生命周期的 Channel 才需要每 Tick 推进，正常空闲连接不参与该集合。
	// Only channels in their closing lifecycle require per-tick progress; ordinary idle connections never enter this set.
	ChannelMaintenanceSet					channelMaintenance_;
	// Tick epoch 让 Channel 在首次收发时懒清零窗口计数，避免为全部连接执行无效写入。
	// The tick epoch lets a Channel lazily reset window counters on first activity, avoiding writes to every connection.
	uint64									channelTickEpoch_;
	// A Watcher directory reads many KCP leaves consecutively. Cache one coherent O(Channel)
	// snapshot per network Tick so diagnostics cannot multiply into dozens of full scans.
	// 一个 Watcher 目录会连续读取多个 KCP 叶子；每个网络 Tick 只生成一次一致的 O(Channel)
	// 快照，避免诊断查询放大为数十次全量扫描。
	mutable uint64							kcpWatcherSnapshotEpoch_;
	mutable bool								kcpWatcherSnapshotValid_;
	mutable KcpWatcherSnapshot				kcpWatcherSnapshot_;
	uint64									finalizedKcpAckSentCount_;
	uint64									finalizedKcpAckReceivedCount_;
	uint64									finalizedKcpTimeoutRetransmissionCount_;
	uint64									finalizedKcpFastRetransmissionCount_;
	uint64									finalizedKcpStreamCoalesceCount_;
	uint64									finalizedKcpStreamCoalescedBytes_;
	uint64									finalizedKcpFlushCallCount_;
	uint64									finalizedKcpFlushScannedSegmentCount_;
	uint64									finalizedKcpFlushDataSegmentCount_;
	uint64									finalizedKcpFlushEmptyDataCallCount_;
	uint64									finalizedKcpAckOutputCallCount_;
	uint64									finalizedKcpAckOutputByteCount_;
	uint64									finalizedKcpDataOutputCallCount_;
	uint64									finalizedKcpDataOutputByteCount_;
	uint64									finalizedKcpSendtoSampleCallCount_;
	uint64									finalizedKcpSendtoSampleStamps_;
	uint64									finalizedKcpSendtoMaxSampleStamps_;
	uint64									discardedPacketsAfterCloseCount_;
	uint64									receiveWindowOverflowBurstCount_;
	uint64									receiveWindowCriticalBurstCount_;
	uint64									receiveWindowOverflowDeferredCount_;
	uint64									receiveWindowOverflowCondemnedCount_;
	uint64									receiveWindowMaxMessagesPerTick_;
	uint64									receiveWindowMaxBytesPerTick_;
	uint64									kcpReceiveDrainCallCount_;
	uint64									kcpReceiveDrainedPacketCount_;
	uint64									kcpReceiveBudgetYieldCount_;
	uint64									kcpReceivePendingSegmentsPeak_;
	uint64									channelIndexMismatchCount_;
	uint64									kcpInputErrorCount_;
	uint64									kcpInputTooShortCount_;
	uint64									kcpInputConversationMismatchCount_;
	uint64									kcpInputTruncatedSegmentCount_;
	uint64									kcpInputInvalidCommandCount_;
	uint64									kcpInputOtherErrorCount_;

	EventDispatcher *						pDispatcher_;
	KcpUpdateScheduler					kcpUpdateScheduler_;
	
	ListenerReceiver *						pExtListenerReceiver_;
	ListenerReceiver *						pExtUdpListenerReceiver_;
	ListenerReceiver *						pIntListenerReceiver_;
	
	DelayedChannels * 						pDelayedChannels_;
	
	ChannelTimeOutHandler *					pChannelTimeOutHandler_;	// 超时的通道可被这个句柄捕捉， 例如告知上层client断开
	ChannelDeregisterHandler *				pChannelDeregisterHandler_;

	int32									numExtChannels_;
};

}
}

#ifdef CODE_INLINE
#include "network_interface.inl"
#endif
#endif // KBE_NETWORK_INTERFACE_H
