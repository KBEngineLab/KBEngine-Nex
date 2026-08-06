#ifndef KBE_KCP_ADAPTIVE_SCHEDULING_H
#define KBE_KCP_ADAPTIVE_SCHEDULING_H

#include "common/common.h"

namespace KBEngine {
namespace Network
{

// The scheduler wakes for each earliest KCP deadline. A slow ikcp_update must yield promptly,
// so the data loop keeps its one-channel floor and only the time budget grows under persistent
// exhaustion. Raising the per-wakeup floor was tried and reverted: it starved internal TCP
// consumption on the BaseApp and produced send-window overflows between BaseApp/CellApp.
// 调度器为最早的 KCP 截止时间唤醒。慢 ikcp_update 必须及时让出，因此数据循环保持单通道下限，
// 仅在持续积压时扩大时间预算。曾尝试提高每轮下限，但会挤占 BaseApp 内部 TCP 消费，
// 导致 BaseApp/CellApp 发送窗口溢出，已回退。
static const uint64 KCP_ADAPTIVE_EXHAUSTION_STEP = 8;
static const uint64 KCP_MAX_ADAPTIVE_PROCESSING_TIME_BUDGET_MICROS = 8000;

inline uint64 adaptiveKcpBudgetMicros(uint64 baseBudgetMicros, uint64 consecutiveExhaustions)
{
	// Persistent backlog means the fixed fairness slice is below the arrival rate.
	// Increase throughput gradually, but retain an 8 ms upper bound for dispatcher fairness.
	// 持续积压表示固定公平切片低于数据到达速率；逐级提高吞吐，并保留 8ms 上限。
	const uint64 growthSteps = consecutiveExhaustions / KCP_ADAPTIVE_EXHAUSTION_STEP;
	const uint64 multiplier = 1 + (growthSteps > 3 ? 3 : growthSteps);
	const uint64 budgetMicros = baseBudgetMicros * multiplier;
	return budgetMicros > KCP_MAX_ADAPTIVE_PROCESSING_TIME_BUDGET_MICROS ?
		KCP_MAX_ADAPTIVE_PROCESSING_TIME_BUDGET_MICROS : budgetMicros;
}

}
}

#endif // KBE_KCP_ADAPTIVE_SCHEDULING_H
