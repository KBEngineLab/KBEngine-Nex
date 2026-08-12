#ifndef KBE_COMPLETION_PROCESSING_BUDGET_H
#define KBE_COMPLETION_PROCESSING_BUDGET_H

#include "common/common.h"
#include "network/common.h"

namespace KBEngine {
namespace Network
{

// Completion callbacks synchronously enter packet parsing and application handlers. The completion
// already obtained by the blocking poll is guaranteed; subsequent completions obey the time budget.
// Fast callbacks still batch naturally until that budget expires, while one slow callback yields
// before a network burst can further starve timers, KCP updates, and the game tick.
// Completion 回调会同步进入封包解析和应用处理。阻塞 poll 已取得的当前 completion 必须处理，
// 后续 completion 则服从时间预算。快速回调仍会自然聚合到预算耗尽；单个慢回调结束后立即让出，
// 避免网络突发继续饿死 Timer、KCP 更新和游戏 Tick。
static const uint32 COMPLETION_MIN_COMPLETIONS_PER_TICK = 1;
static const uint32 COMPLETION_MAX_PROCESSING_TIME_MS = 2;
static const uint64 COMPLETION_ADAPTIVE_EXHAUSTION_STEP = 8;

inline uint32 completionProcessingTimeBudgetMs(uint64 consecutiveBudgetExhaustions)
{
	if(g_maxCompletionProcessingTimeMS == 0)
		return 0;

	// A permanently non-empty completion queue needs more throughput than the 2 ms
	// fairness slice can provide. Grow only after repeated exhaustion and keep the
	// slice bounded so timers and game ticks still run at least once per 8 ms of IO.
	// completion 队列持续非空时，2ms 公平切片可能不足以追上到达速率。
	// 仅在连续耗尽后逐级扩容，并以 8ms 封顶，确保 Timer 与游戏 Tick 仍能及时运行。
	const uint64 growthSteps = consecutiveBudgetExhaustions / COMPLETION_ADAPTIVE_EXHAUSTION_STEP;
	const uint64 multiplier = 1 + (growthSteps > 3 ? 3 : growthSteps);
	const uint32 initialBudgetMs = KBE_MIN(COMPLETION_MAX_PROCESSING_TIME_MS,
		g_maxCompletionProcessingTimeMS);
	const uint64 budgetMs = static_cast<uint64>(initialBudgetMs) * multiplier;
	return static_cast<uint32>(KBE_MIN(budgetMs,
		static_cast<uint64>(g_maxCompletionProcessingTimeMS)));
}

inline bool shouldProcessAnotherCompletion(uint32 processedCount,
	uint64 elapsedStamps, uint64 processingBudgetStamps)
{
	if (processedCount >= g_maxCompletionsPerTick)
	{
		return false;
	}

	return processedCount < COMPLETION_MIN_COMPLETIONS_PER_TICK ||
		processingBudgetStamps == 0 || elapsedStamps < processingBudgetStamps;
}

inline bool completionTimeBudgetExhausted(uint32 processedCount,
	uint64 elapsedStamps, uint64 processingBudgetStamps)
{
	return processedCount >= COMPLETION_MIN_COMPLETIONS_PER_TICK &&
		processingBudgetStamps > 0 && elapsedStamps >= processingBudgetStamps;
}

}
}

#endif // KBE_COMPLETION_PROCESSING_BUDGET_H
