// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

#include "kcp_update_scheduler.h"

#include "channel.h"
#include "event_dispatcher.h"
#include "kcp_adaptive_scheduling.h"

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
// 数据队列保持单通道下限，慢回调后立即归还 dispatcher，避免挤占内部 TCP 消费；
// ACK 队列使用固定四通道下限摊薄 Windows 粗粒度 timer 成本。
// A backlogged ikcp_update can grow from microseconds to milliseconds due to retransmissions.
// The data queue keeps a one-channel floor so a slow callback yields promptly and cannot
// starve internal TCP consumption; the ACK queue uses a fixed four-channel floor.
const size_t KCP_MIN_UPDATES_PER_WAKEUP = 1;
const size_t KCP_MIN_ACK_FLUSHES_PER_WAKEUP = 4;
const size_t KCP_MAX_UPDATES_PER_WAKEUP = 2048;
const uint64 KCP_PROCESSING_TIME_BUDGET_MICROS = 2000;
const uint64 KCP_ACK_PROCESSING_TIME_BUDGET_MICROS = 2000;
const uint64 KCP_RECEIVE_PROCESSING_TIME_BUDGET_MICROS = 2000;
const int64 KCP_BACKLOG_RETRY_DELAY_MICROS = 1000;
}

KcpUpdateScheduler::KcpUpdateScheduler(EventDispatcher& dispatcher) :
	dispatcher_(dispatcher),
	queue_(),
	ackQueue_(),
	receiveQueue_(),
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
void KcpUpdateScheduler::scheduleReceive(Channel& channel)
{
	const uint64 dueTime = timestamp() + delayToStamps(1);
	if (receiveQueue_.schedule(channelKey(channel), dueTime) && !processing_)
		armNextTimer();
}

//-------------------------------------------------------------------------------------
void KcpUpdateScheduler::cancel(Channel& channel)
{
	const KcpUpdateQueue::Key key = channelKey(channel);
	const bool dataChanged = queue_.cancel(key);
	const bool ackChanged = ackQueue_.cancel(key);
	const bool receiveChanged = receiveQueue_.cancel(key);
	const bool changed = dataChanged || ackChanged || receiveChanged;
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
	uint64 receiveDueTime = 0;
	const bool hasData = queue_.nextDue(dataDueTime);
	const bool hasAck = ackQueue_.nextDue(ackDueTime);
	const bool hasReceive = receiveQueue_.nextDue(receiveDueTime);
	if (!hasData && !hasAck && !hasReceive)
	{
		if (timerHandle_.isSet())
		{
			timerHandle_.cancel();
		}
		timerDueTime_ = 0;
		return;
	}
	uint64 dueTime = hasData ? dataDueTime : (hasAck ? ackDueTime : receiveDueTime);
	if (hasAck)
		dueTime = std::min(dueTime, ackDueTime);
	if (hasReceive)
		dueTime = std::min(dueTime, receiveDueTime);

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
	const uint64 ackBudgetMicros = adaptiveKcpAckBudgetMicros(
		KCP_ACK_PROCESSING_TIME_BUDGET_MICROS, consecutiveBudgetExhaustions_);
	const uint64 ackBudget = delayToStamps(ackBudgetMicros);
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
	const uint64 processingBudgetMicros = adaptiveKcpBudgetMicros(
		KCP_PROCESSING_TIME_BUDGET_MICROS, consecutiveBudgetExhaustions_);
	const uint64 processingBudget = delayToStamps(processingBudgetMicros);
	size_t processed = 0;
	for (; processed < KCP_MAX_UPDATES_PER_WAKEUP; ++processed)
	{
		if (processed >= KCP_MIN_UPDATES_PER_WAKEUP &&
			timestamp() - dataProcessingStart >= processingBudget)
		{
			break;
		}

		// A busy ikcp_update slice can span several milliseconds. Re-read the monotonic
		// clock before selecting the next entry so Channels that become due during this
		// slice consume the remaining budget instead of waiting for another global timer.
		// 一轮繁忙的 ikcp_update 可能持续数毫秒。选择下一项前重新读取单调时钟，使本轮
		// 期间新到期的 Channel 直接使用剩余预算，而不是额外等待下一次全局 Timer。
		const uint64 selectionTime = timestamp();
		if (!queue_.takeDue(selectionTime, key, &dueTime))
			break;
		if (selectionTime > dueTime)
		{
			++deadlineMissCount_;
			const uint64 scheduleDelayMicros = static_cast<uint64>(
				stampsToDelay(selectionTime - dueTime));
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
	const uint64 backlogCheckTime = timestamp();
	const bool budgetExhausted = (processed == KCP_MAX_UPDATES_PER_WAKEUP || timeBudgetExhausted) &&
		queue_.nextDue(nextDueTime) && nextDueTime <= backlogCheckTime;
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

	// Reassembled application messages have their own slice after protocol work. This keeps
	// retransmission deadlines moving even when one message synchronously enters a slow script
	// handler, while still guaranteeing quiet Channels a continuation without another datagram.
	// 已重组应用消息在协议工作之后使用独立切片。即使单条消息同步进入慢脚本处理器，重传
	// 截止时间仍能继续推进；安静 Channel 也无需等待下一份数据报即可续处理尾包。
	const uint64 receiveProcessingStart = timestamp();
	const uint64 receiveBudget = delayToStamps(KCP_RECEIVE_PROCESSING_TIME_BUDGET_MICROS);
	size_t receiveProcessed = 0;
	const uint64 receiveNow = receiveProcessingStart;
	for (; receiveProcessed < KCP_MAX_UPDATES_PER_WAKEUP; ++receiveProcessed)
	{
		if (receiveProcessed >= KCP_MIN_UPDATES_PER_WAKEUP &&
			timestamp() - receiveProcessingStart >= receiveBudget)
		{
			break;
		}
		if (!receiveQueue_.takeDue(receiveNow, key, &dueTime))
			break;
		Channel* channel = reinterpret_cast<Channel*>(key);
		channel->drainKcpReceive();
		if (!receiveQueue_.isScheduled(key))
			receiveQueue_.cancel(key);
	}
	processing_ = false;
	armNextTimer();
}

}
}
