#include "network/completion_processing_budget.h"
#include "network/iocp_udp_receive_depth.h"

#include <cstdlib>
#include <iostream>

namespace
{
bool require(bool condition, const char* message)
{
	if (!condition)
	{
		std::cerr << message << std::endl;
	}

	return condition;
}
}

int main()
{
	using namespace KBEngine::Network;
	const KBEngine::uint64 budget = 2000;

	// 客户端与共享 listener 必须使用不同接收深度：前者控制每连接内存，后者吸收聚合突发。
	// Client and shared-listener depths differ to bound per-connection memory while absorbing aggregate bursts.
	if (!require(iocpUdpReceiveDepth(true) == 4,
		"connected UDP receive depth changed unexpectedly") ||
		!require(iocpUdpReceiveDepth(false) == 64,
			"shared UDP listener receive depth changed unexpectedly"))
	{
		return EXIT_FAILURE;
	}

	// 自适应预算必须只在持续积压后增长，并以 16ms 封顶，防止一次网络轮询饿死 Timer。
	// The adaptive budget grows only after persistent backlog and remains capped at 16 ms.
	if (!require(completionProcessingTimeBudgetMs(0) == 2,
		"idle completion budget was not the 2 ms baseline") ||
		!require(completionProcessingTimeBudgetMs(7) == 2,
			"completion budget grew before the exhaustion threshold") ||
		!require(completionProcessingTimeBudgetMs(8) == 4,
			"completion budget did not grow after persistent exhaustion") ||
		!require(completionProcessingTimeBudgetMs(16) == 6,
			"completion budget did not grow progressively") ||
		!require(completionProcessingTimeBudgetMs(24) == 8,
			"completion budget did not reach the 8 ms intermediate level") ||
		!require(completionProcessingTimeBudgetMs(56) == 16,
			"completion budget did not reach its bounded maximum") ||
		!require(completionProcessingTimeBudgetMs(1000) == 16,
			"completion budget exceeded its bounded maximum"))
	{
		return EXIT_FAILURE;
	}

	// 阻塞 poll 已经取出的第一个 completion 必须处理；其回调结束后再决定是否继续 drain。
	// The first completion returned by the blocking poll must run before deciding whether to continue draining.
	if (!require(shouldProcessAnotherCompletion(0, budget * 2, budget),
		"the first completion was not guaranteed"))
	{
		return EXIT_FAILURE;
	}

	// 第一个回调完成后必须立即服从时间预算，避免任何固定批量把 Timer 和游戏 Tick 饿住。
	// After the first callback, the time budget must yield immediately so no fixed batch can starve timers and game ticks.
	if (!require(!shouldProcessAnotherCompletion(COMPLETION_MIN_COMPLETIONS_PER_TICK, budget, budget),
		"time budget did not yield after the guaranteed batch") ||
		!require(completionTimeBudgetExhausted(COMPLETION_MIN_COMPLETIONS_PER_TICK, budget, budget),
			"time budget exhaustion was not reported"))
	{
		return EXIT_FAILURE;
	}

	// 快回调仍可继续聚合到硬上限；禁用时间预算时也必须保留数量上限，防止无限 drain。
	// Fast callbacks may continue to the hard bound; disabling the time budget must still retain that count bound.
	if (!require(shouldProcessAnotherCompletion(COMPLETION_MIN_COMPLETIONS_PER_TICK, budget - 1, budget),
		"fast completion batch stopped before its budget") ||
		!require(shouldProcessAnotherCompletion(COMPLETION_MAX_COMPLETIONS_PER_TICK - 1, budget * 2, 0),
			"disabled time budget stopped before the hard bound") ||
		!require(!shouldProcessAnotherCompletion(COMPLETION_MAX_COMPLETIONS_PER_TICK, 0, 0),
			"hard completion bound was exceeded") ||
		!require(!completionTimeBudgetExhausted(COMPLETION_MAX_COMPLETIONS_PER_TICK, budget * 2, 0),
			"disabled time budget was reported as exhausted"))
	{
		return EXIT_FAILURE;
	}

	std::cout << "COMPLETION_PROCESSING_BUDGET_TEST_PASS" << std::endl;
	return EXIT_SUCCESS;
}
