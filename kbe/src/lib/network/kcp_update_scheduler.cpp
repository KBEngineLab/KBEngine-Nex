// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

#include "kcp_update_scheduler.h"

#include "channel.h"
#include "event_dispatcher.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace KBEngine {
namespace Network
{
namespace
{
// Bound one wakeup so a KCP burst cannot starve packet dispatch, timers, or the game tick.
// 限制单次唤醒的处理量，避免 KCP 突发饿死报文分发、其他定时器和游戏 Tick。
const size_t KCP_MIN_UPDATES_PER_WAKEUP = 256;
const size_t KCP_MAX_UPDATES_PER_WAKEUP = 2048;
const uint64 KCP_PROCESSING_TIME_BUDGET_MICROS = 2000;
}

KcpUpdateScheduler::KcpUpdateScheduler(EventDispatcher& dispatcher) :
	dispatcher_(dispatcher),
	queue_(),
	timerHandle_(),
	timerDueTime_(0),
	processing_(false),
	updateCallCount_(0),
	timerWakeupCount_(0),
	timerRearmCount_(0),
	deadlineMissCount_(0),
	maxScheduleDelayMicros_(0),
	budgetExhaustionCount_(0),
	consecutiveBudgetExhaustions_(0),
	maxConsecutiveBudgetExhaustions_(0),
	timeBudgetExhaustionCount_(0),
	totalProcessingMicros_(0),
	maxProcessingMicros_(0)
{
}

//-------------------------------------------------------------------------------------
KcpUpdateScheduler::~KcpUpdateScheduler()
{
	if (timerHandle_.isSet())
	{
		timerHandle_.cancel();
	}
}

//-------------------------------------------------------------------------------------
uint64 KcpUpdateScheduler::dueChannelCount() const
{
	return static_cast<uint64>(queue_.dueCount(timestamp()));
}

//-------------------------------------------------------------------------------------
uint64 KcpUpdateScheduler::overdueChannelCount() const
{
	return static_cast<uint64>(queue_.overdueCount(timestamp()));
}

//-------------------------------------------------------------------------------------
KcpUpdateQueue::Key KcpUpdateScheduler::channelKey(const Channel& channel)
{
	return reinterpret_cast<KcpUpdateQueue::Key>(&channel);
}

//-------------------------------------------------------------------------------------
uint64 KcpUpdateScheduler::delayToStamps(int64 microseconds)
{
	if (microseconds <= 0)
	{
		microseconds = 1;
	}

	const double stamps = std::ceil(static_cast<double>(microseconds) * stampsPerSecondD() / 1000000.0);
	return std::max<uint64>(1, static_cast<uint64>(stamps));
}

//-------------------------------------------------------------------------------------
int64 KcpUpdateScheduler::stampsToDelay(uint64 stamps)
{
	const double microseconds = std::ceil(static_cast<double>(stamps) * 1000000.0 / stampsPerSecondD());
	if (microseconds >= static_cast<double>(std::numeric_limits<int64>::max()))
	{
		return std::numeric_limits<int64>::max();
	}
	return std::max<int64>(1, static_cast<int64>(microseconds));
}

//-------------------------------------------------------------------------------------
void KcpUpdateScheduler::schedule(Channel& channel, int64 microseconds)
{
	const uint64 dueTime = timestamp() + delayToStamps(microseconds);
	if (queue_.schedule(channelKey(channel), dueTime) && !processing_)
	{
		armNextTimer();
	}
}

//-------------------------------------------------------------------------------------
void KcpUpdateScheduler::cancel(Channel& channel)
{
	if (queue_.cancel(channelKey(channel)) && !processing_)
	{
		armNextTimer();
	}
}

//-------------------------------------------------------------------------------------
bool KcpUpdateScheduler::isScheduled(const Channel& channel) const
{
	return queue_.isScheduled(channelKey(channel));
}

//-------------------------------------------------------------------------------------
void KcpUpdateScheduler::armNextTimer()
{
	uint64 dueTime = 0;
	if (!queue_.nextDue(dueTime))
	{
		if (timerHandle_.isSet())
		{
			timerHandle_.cancel();
		}
		timerDueTime_ = 0;
		return;
	}

	// An already armed earlier timer is a harmless early wakeup and avoids another global timer cancellation/allocation.
	// 已投递的更早 timer 只会产生一次无害的提前唤醒，保留它可避免再次取消和分配全局 timer。
	if (timerHandle_.isSet() && timerDueTime_ <= dueTime)
	{
		return;
	}

	if (timerHandle_.isSet())
	{
		timerHandle_.cancel();
	}

	const uint64 now = timestamp();
	const uint64 remaining = dueTime > now ? dueTime - now : 1;
	timerHandle_ = dispatcher_.addTimer(stampsToDelay(remaining), this);
	timerDueTime_ = dueTime;
	++timerRearmCount_;
}

//-------------------------------------------------------------------------------------
void KcpUpdateScheduler::handleTimeout(TimerHandle handle, void*)
{
	// EventDispatcher timers repeat by default; cancel the executing handle before arming the next queue deadline.
	// EventDispatcher timer 默认重复执行，因此必须先取消正在执行的 handle，再投递队列的下一个截止时间。
	if (!timerHandle_.isSet() || !(handle == timerHandle_))
		return;

	timerHandle_.cancel();
	timerDueTime_ = 0;
	++timerWakeupCount_;

	processing_ = true;
	const uint64 processingStart = timestamp();
	const uint64 processingBudget = delayToStamps(KCP_PROCESSING_TIME_BUDGET_MICROS);
	const uint64 now = processingStart;
	KcpUpdateQueue::Key key = 0;
	KcpUpdateQueue::Time dueTime = 0;
	size_t processed = 0;
	for (; processed < KCP_MAX_UPDATES_PER_WAKEUP; ++processed)
	{
		if (processed >= KCP_MIN_UPDATES_PER_WAKEUP &&
			timestamp() - processingStart >= processingBudget)
		{
			break;
		}
		if (!queue_.takeDue(now, key, &dueTime))
			break;
		if (now > dueTime)
		{
			++deadlineMissCount_;
			maxScheduleDelayMicros_ = std::max<uint64>(maxScheduleDelayMicros_,
				static_cast<uint64>(stampsToDelay(now - dueTime)));
		}
		Channel* channel = reinterpret_cast<Channel*>(key);
		++updateCallCount_;
		channel->updateKcp();
		if (!queue_.isScheduled(key))
		{
			// A normal callback reschedules in place; an inactive entry means the Channel ended and its retained map node can be removed.
			// 正常回调会原地续约；inactive 项表示 Channel 已结束，此时可删除为复用保留的 map 节点。
			queue_.cancel(key);
		}
	}

	const uint64 processingElapsed = timestamp() - processingStart;
	const uint64 processingMicros = static_cast<uint64>(stampsToDelay(processingElapsed));
	totalProcessingMicros_ += processingMicros;
	maxProcessingMicros_ = std::max(maxProcessingMicros_, processingMicros);
	const bool timeBudgetExhausted = processed >= KCP_MIN_UPDATES_PER_WAKEUP &&
		processingElapsed >= processingBudget;
	if (timeBudgetExhausted)
		++timeBudgetExhaustionCount_;

	KcpUpdateQueue::Time nextDueTime = 0;
	const bool budgetExhausted = (processed == KCP_MAX_UPDATES_PER_WAKEUP || timeBudgetExhausted) &&
		queue_.nextDue(nextDueTime) && nextDueTime <= now;
	if (budgetExhausted)
	{
		++budgetExhaustionCount_;
		++consecutiveBudgetExhaustions_;
		maxConsecutiveBudgetExhaustions_ = std::max(maxConsecutiveBudgetExhaustions_,
			consecutiveBudgetExhaustions_);
	}
	else
	{
		consecutiveBudgetExhaustions_ = 0;
	}
	processing_ = false;
	armNextTimer();
}

}
}
