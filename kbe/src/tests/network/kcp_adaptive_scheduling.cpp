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

	// 协议数据切片在持续耗尽时按 2/4/8/16/32ms 增长；ACK 独立封顶 4ms，
	// 避免确认包与协议追赶合计长期独占 dispatcher。
	// Sustained protocol exhaustion grows through 2/4/8/16/32 ms. ACK work caps
	// independently at 4 ms so acknowledgements and catch-up cannot jointly own the dispatcher.
	if (!require(adaptiveKcpBudgetMicros(2000, 0) == 2000,
		"idle budget was not the 2 ms baseline") ||
		!require(adaptiveKcpBudgetMicros(2000, 7) == 2000,
			"budget grew before the exhaustion threshold") ||
		!require(adaptiveKcpBudgetMicros(2000, 8) == 4000,
			"budget did not grow after persistent exhaustion") ||
		!require(adaptiveKcpBudgetMicros(2000, 16) == 8000,
			"budget did not reach the 8 ms tier") ||
		!require(adaptiveKcpBudgetMicros(2000, 24) == 16000,
			"budget did not reach the 16 ms tier") ||
		!require(adaptiveKcpBudgetMicros(2000, 32) == 32000,
			"budget did not reach its bounded maximum") ||
		!require(adaptiveKcpBudgetMicros(2000, 1000) == 32000,
			"budget exceeded its bounded maximum") ||
		!require(adaptiveKcpAckBudgetMicros(2000, 0) == 2000,
			"idle ACK budget was not the 2 ms baseline") ||
		!require(adaptiveKcpAckBudgetMicros(2000, 1000) == 4000,
			"ACK budget exceeded its 4 ms maximum"))
	{
		return EXIT_FAILURE;
	}

	std::cout << "KCP_ADAPTIVE_SCHEDULING_TEST_PASS" << std::endl;
	return EXIT_SUCCESS;
}
