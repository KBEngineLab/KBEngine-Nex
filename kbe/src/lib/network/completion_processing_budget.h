#ifndef KBE_COMPLETION_PROCESSING_BUDGET_H
#define KBE_COMPLETION_PROCESSING_BUDGET_H

#include "common/common.h"

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
static const uint32 COMPLETION_MAX_COMPLETIONS_PER_TICK = 1024;
static const uint32 COMPLETION_MAX_PROCESSING_TIME_MS = 2;

inline bool shouldProcessAnotherCompletion(uint32 processedCount,
	uint64 elapsedStamps, uint64 processingBudgetStamps)
{
	if (processedCount >= COMPLETION_MAX_COMPLETIONS_PER_TICK)
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
