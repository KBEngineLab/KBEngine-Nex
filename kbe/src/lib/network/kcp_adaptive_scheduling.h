#ifndef KBE_KCP_ADAPTIVE_SCHEDULING_H
#define KBE_KCP_ADAPTIVE_SCHEDULING_H

#include "common/common.h"

#include <algorithm>

namespace KBEngine {
namespace Network
{

// Protocol updates no longer dispatch reassembled application messages, so sustained backlog
// may safely receive a larger slice without running Entity/Python callbacks inside it. ACK work
// retains a separate small cap because prompt acknowledgements need latency, not long ownership
// of the dispatcher thread.
// 协议更新已不再派发重组后的应用消息，因此持续积压时可以安全扩大切片，而不会在其中执行
// Entity/Python 回调。ACK 仍使用独立小上限，因为确认包需要的是低延迟，而不是长时间占用
// dispatcher 线程。
static const uint64 KCP_ADAPTIVE_EXHAUSTION_STEP = 8;
static const uint64 KCP_MAX_ADAPTIVE_PROCESSING_TIME_BUDGET_MICROS = 32000;
static const uint64 KCP_MAX_ADAPTIVE_ACK_PROCESSING_TIME_BUDGET_MICROS = 4000;

inline uint64 boundedAdaptiveKcpBudgetMicros(uint64 baseBudgetMicros,
	uint64 maximumBudgetMicros, uint64 consecutiveExhaustions)
{
	// Exponential tiers reach useful throughput quickly after a few consecutive exhausted
	// wakeups, while the explicit maximum keeps each dispatcher handoff bounded.
	// 指数档位可在连续数次耗尽后快速达到有效吞吐；显式上限仍保证每次 dispatcher 交接有界。
	const uint64 growthSteps = std::min<uint64>(
		consecutiveExhaustions / KCP_ADAPTIVE_EXHAUSTION_STEP, 4);
	const uint64 multiplier = 1ULL << growthSteps;
	if (baseBudgetMicros >= maximumBudgetMicros / multiplier)
		return maximumBudgetMicros;

	return std::min<uint64>(baseBudgetMicros * multiplier, maximumBudgetMicros);
}

inline uint64 adaptiveKcpBudgetMicros(uint64 baseBudgetMicros, uint64 consecutiveExhaustions)
{
	return boundedAdaptiveKcpBudgetMicros(baseBudgetMicros,
		KCP_MAX_ADAPTIVE_PROCESSING_TIME_BUDGET_MICROS, consecutiveExhaustions);
}

inline uint64 adaptiveKcpAckBudgetMicros(uint64 baseBudgetMicros, uint64 consecutiveExhaustions)
{
	return boundedAdaptiveKcpBudgetMicros(baseBudgetMicros,
		KCP_MAX_ADAPTIVE_ACK_PROCESSING_TIME_BUDGET_MICROS, consecutiveExhaustions);
}

}
}

#endif // KBE_KCP_ADAPTIVE_SCHEDULING_H
