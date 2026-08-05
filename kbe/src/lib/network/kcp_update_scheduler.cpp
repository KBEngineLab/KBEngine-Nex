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
// 积压时单个 ikcp_update 可能因大量重传而从微秒级放大到毫秒级；小批量后即检查时间预算，
// 避免“至少 256 次”把一次 2ms 调度轮次放大成数百毫秒并继续制造超时重传。
// A backlogged ikcp_update can grow from microseconds to milliseconds due to retransmissions.
// Per-channel output and payload are bounded. A four-channel floor amortizes the
// coarse Windows timer quantum for both ACK and data queues; the time check after
// this small floor still returns control to the BaseApp dispatcher promptly.
// 单通道输出和 payload 已受限；ACK 与数据队列使用四通道下限摊薄 Windows 粗粒度
// timer 成本，并在该小批次后检查时间预算，及时归还 BaseApp dispatcher。
const size_t KCP_MIN_UPDATES_PER_WAKEUP = 1;
const size_t KCP_MIN_ACK_FLUSHES_PER_WAKEUP = 4;
const size_t KCP_MAX_UPDATES_PER_WAKEUP = 2048;
const uint64 KCP_PROCESSING_TIME_BUDGET_MICROS = 2000;
const uint64 KCP_ACK_PROCESSING_TIME_BUDGET_MICROS = 2000;
const int64 KCP_BACKLOG_RETRY_DELAY_MICROS = 1000;
}

KcpUpdateScheduler::KcpUpdateScheduler(EventDispatcher& dispatcher) :
	dispatcher_(dispatcher),
	queue_(),
	ackQueue_(),
	timerHandle_(),
	timerDueTime_(0),
	processing_(false),
	updateCallCount_(0),
	timerWakeupCount_(0),
	timerRearmCount_(0),
	deadlineMissCount_(0),
	protocolTickMissCount_(0),
	maxScheduleDelayMicros_(0),
	budgetExhaustionCount_(0),
	consecutiveBudgetExhaustions_(0),
	maxConsecutiveBudgetExhaustions_(0),
	timeBudgetExhaustionCount_(0),
	totalProcessingMicros_(0),
	maxProcessingMicros_(0),
	ackFlushCallCount_(0),
	ackBudgetExhaustionCount_(0),
	ackTotalProcessingMicros_(0),
	ackMaxProcessingMicros_(0),
	dataTotalProcessingMicros_(0),
	dataMaxProcessingMicros_(0)
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
void KcpUpdateScheduler::scheduleAck(Channel& channel)
{
	const uint64 dueTime = timestamp() + delayToStamps(1);
	if (ackQueue_.schedule(channelKey(channel), dueTime) && !processing_)
		armNextTimer();
}

//-------------------------------------------------------------------------------------
void KcpUpdateScheduler::cancel(Channel& channel)
{
	const KcpUpdateQueue::Key key = channelKey(channel);
	const bool dataChanged = queue_.cancel(key);
	const bool ackChanged = ackQueue_.cancel(key);
	const bool changed = dataChanged || ackChanged;
	if (changed && !processing_)
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
	uint64 dataDueTime = 0;
	uint64 ackDueTime = 0;
	const bool hasData = queue_.nextDue(dataDueTime);
	const bool hasAck = ackQueue_.nextDue(ackDueTime);
	if (!hasData && !hasAck)
	{
		if (timerHandle_.isSet())
		{
			timerHandle_.cancel();
		}
		timerDueTime_ = 0;
		return;
	}
	const uint64 dueTime = hasData && hasAck
		? std::min(dataDueTime, ackDueTime) : (hasData ? dataDueTime : ackDueTime);

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
	const int64 remainingMicros = dueTime > now
		? stampsToDelay(dueTime - now)
		: KCP_BACKLOG_RETRY_DELAY_MICROS;
	timerHandle_ = dispatcher_.addTimer(remainingMicros, this);
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
	const uint64 ackBudget = delayToStamps(KCP_ACK_PROCESSING_TIME_BUDGET_MICROS);
	KcpUpdateQueue::Key key = 0;
	KcpUpdateQueue::Time dueTime = 0;
	size_t ackProcessed = 0;
	for (; ackProcessed < KCP_MAX_UPDATES_PER_WAKEUP; ++ackProcessed)
	{
		if (ackProcessed >= KCP_MIN_ACK_FLUSHES_PER_WAKEUP &&
			timestamp() - processingStart >= ackBudget)
			break;
		if (!ackQueue_.takeDue(processingStart, key, &dueTime))
			break;
		Channel* channel = reinterpret_cast<Channel*>(key);
		++ackFlushCallCount_;
		channel->flushKcpAcks();
		ackQueue_.cancel(key);
	}
	if (ackProcessed >= KCP_MAX_UPDATES_PER_WAKEUP ||
		(ackProcessed >= KCP_MIN_ACK_FLUSHES_PER_WAKEUP && timestamp() - processingStart >= ackBudget))
	{
		KcpUpdateQueue::Time nextAckDue = 0;
		if (ackQueue_.nextDue(nextAckDue) && nextAckDue <= processingStart)
			++ackBudgetExhaustionCount_;
	}
	const uint64 ackProcessingMicros = static_cast<uint64>(stampsToDelay(timestamp() - processingStart));
	ackTotalProcessingMicros_ += ackProcessingMicros;
	ackMaxProcessingMicros_ = std::max(ackMaxProcessingMicros_, ackProcessingMicros);

	const uint64 dataProcessingStart = timestamp();
	const uint64 processingBudget = delayToStamps(KCP_PROCESSING_TIME_BUDGET_MICROS);
	const uint64 now = dataProcessingStart;
	size_t processed = 0;
	for (; processed < KCP_MAX_UPDATES_PER_WAKEUP; ++processed)
	{
		if (processed >= KCP_MIN_UPDATES_PER_WAKEUP &&
			timestamp() - dataProcessingStart >= processingBudget)
		{
			break;
		}
		if (!queue_.takeDue(now, key, &dueTime))
			break;
		if (now > dueTime)
		{
			++deadlineMissCount_;
			const uint64 scheduleDelayMicros = static_cast<uint64>(stampsToDelay(now - dueTime));
			maxScheduleDelayMicros_ = std::max<uint64>(maxScheduleDelayMicros_, scheduleDelayMicros);
			// deadlineMisses 包含正常的微秒级定时器抖动；只有超过一个完整 KCP tick
			// 才表示协议调度已真正落后一轮。tickInterval=0 对应 KCP 默认 100ms。
			// deadlineMisses includes ordinary microsecond timer jitter. Only a delay beyond
			// one full KCP tick means protocol scheduling has actually fallen a cycle behind.
			const uint64 protocolTickMicros = static_cast<uint64>(
				g_rudp_tickInterval > 0 ? g_rudp_tickInterval : 100) * 1000ULL;
			if (scheduleDelayMicros > protocolTickMicros)
				++protocolTickMissCount_;
		}
		Channel* channel = reinterpret_cast<Channel*>(key);
		ackQueue_.cancel(key);
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
	const uint64 dataProcessingElapsed = timestamp() - dataProcessingStart;
	const uint64 processingMicros = static_cast<uint64>(stampsToDelay(processingElapsed));
	const uint64 dataProcessingMicros = static_cast<uint64>(stampsToDelay(dataProcessingElapsed));
	totalProcessingMicros_ += processingMicros;
	maxProcessingMicros_ = std::max(maxProcessingMicros_, processingMicros);
	dataTotalProcessingMicros_ += dataProcessingMicros;
	dataMaxProcessingMicros_ = std::max(dataMaxProcessingMicros_, dataProcessingMicros);
	const bool timeBudgetExhausted = processed >= KCP_MIN_UPDATES_PER_WAKEUP &&
		dataProcessingElapsed >= processingBudget;
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
