#include "network/kcp_adaptive_scheduling.h"

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

	// 时间预算以连续耗尽为信号：正常 2ms，持续积压逐级提升并封顶 8ms。
	// The time budget uses sustained exhaustion as its signal: 2 ms baseline, tiered growth capped at 8 ms.
	if (!require(adaptiveKcpBudgetMicros(2000, 0) == 2000,
		"idle budget was not the 2 ms baseline") ||
		!require(adaptiveKcpBudgetMicros(2000, 7) == 2000,
			"budget grew before the exhaustion threshold") ||
		!require(adaptiveKcpBudgetMicros(2000, 8) == 4000,
			"budget did not grow after persistent exhaustion") ||
		!require(adaptiveKcpBudgetMicros(2000, 16) == 6000,
			"budget did not grow progressively") ||
		!require(adaptiveKcpBudgetMicros(2000, 24) == 8000,
			"budget did not reach its bounded maximum") ||
		!require(adaptiveKcpBudgetMicros(2000, 1000) == 8000,
			"budget exceeded its bounded maximum"))
	{
		return EXIT_FAILURE;
	}

	std::cout << "KCP_ADAPTIVE_SCHEDULING_TEST_PASS" << std::endl;
	return EXIT_SUCCESS;
}
