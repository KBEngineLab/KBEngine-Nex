// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

#ifndef KBE_KCP_UPDATE_SCHEDULER_H
#define KBE_KCP_UPDATE_SCHEDULER_H

#include "common/timer.h"
#include "kcp_update_queue.h"

namespace KBEngine {
namespace Network
{

class Channel;
class EventDispatcher;

class KcpUpdateScheduler : public TimerHandler
{
public:
	explicit KcpUpdateScheduler(EventDispatcher& dispatcher);
	~KcpUpdateScheduler() override;

	void schedule(Channel& channel, int64 microseconds);
	void cancel(Channel& channel);
	bool isScheduled(const Channel& channel) const;

	uint64 scheduledChannelCount() const { return static_cast<uint64>(queue_.scheduledCount()); }
	uint64 heapEntryCount() const { return static_cast<uint64>(queue_.heapEntryCount()); }
	uint64 scheduleRequestCount() const { return queue_.scheduleRequestCount(); }
	uint64 earlierReplacementCount() const { return queue_.earlierReplacementCount(); }
	uint64 staleDiscardCount() const { return queue_.staleDiscardCount(); }
	uint64 compactionCount() const { return queue_.compactionCount(); }
	uint64 updateCallCount() const { return updateCallCount_; }
	uint64 timerWakeupCount() const { return timerWakeupCount_; }
	uint64 timerRearmCount() const { return timerRearmCount_; }
	uint64 dueChannelCount() const;
	uint64 overdueChannelCount() const;
	uint64 deadlineMissCount() const { return deadlineMissCount_; }
	uint64 protocolTickMissCount() const { return protocolTickMissCount_; }
	uint64 maxScheduleDelayMicros() const { return maxScheduleDelayMicros_; }
	uint64 budgetExhaustionCount() const { return budgetExhaustionCount_; }
	uint64 consecutiveBudgetExhaustions() const { return consecutiveBudgetExhaustions_; }
	uint64 maxConsecutiveBudgetExhaustions() const { return maxConsecutiveBudgetExhaustions_; }
	uint64 timeBudgetExhaustionCount() const { return timeBudgetExhaustionCount_; }
	uint64 totalProcessingMicros() const { return totalProcessingMicros_; }
	uint64 maxProcessingMicros() const { return maxProcessingMicros_; }

private:
	void handleTimeout(TimerHandle handle, void* pUser) override;
	void armNextTimer();
	static uint64 delayToStamps(int64 microseconds);
	static int64 stampsToDelay(uint64 stamps);
	static KcpUpdateQueue::Key channelKey(const Channel& channel);

	EventDispatcher& dispatcher_;
	KcpUpdateQueue queue_;
	TimerHandle timerHandle_;
	uint64 timerDueTime_;
	bool processing_;
	uint64 updateCallCount_;
	uint64 timerWakeupCount_;
	uint64 timerRearmCount_;
	uint64 deadlineMissCount_;
	uint64 protocolTickMissCount_;
	uint64 maxScheduleDelayMicros_;
	uint64 budgetExhaustionCount_;
	uint64 consecutiveBudgetExhaustions_;
	uint64 maxConsecutiveBudgetExhaustions_;
	uint64 timeBudgetExhaustionCount_;
	uint64 totalProcessingMicros_;
	uint64 maxProcessingMicros_;
};

}
}

#endif // KBE_KCP_UPDATE_SCHEDULER_H
