#include "network/kcp_receive_budget.h"

#include <cstdlib>
#include <iostream>

namespace
{
bool require(bool condition, const char* message)
{
	if (!condition)
		std::cerr << message << std::endl;
	return condition;
}
}

int main()
{
	using namespace KBEngine::Network;
	const KBEngine::uint64 budget = 500;

	if (!require(shouldDrainAnotherKcpPacket(0, budget * 2, budget),
		"the first reassembled packet was not guaranteed") ||
		!require(!shouldDrainAnotherKcpPacket(KCP_RECEIVE_MIN_PACKETS_PER_SLICE, budget, budget),
			"a slow packet did not yield at the time budget") ||
		!require(shouldDrainAnotherKcpPacket(KCP_RECEIVE_MIN_PACKETS_PER_SLICE, budget - 1, budget),
			"fast packets stopped before the time budget") ||
		!require(!shouldDrainAnotherKcpPacket(KCP_RECEIVE_MAX_PACKETS_PER_SLICE, 0, 0),
			"the hard packet bound was exceeded"))
	{
		return EXIT_FAILURE;
	}

	std::cout << "KCP_RECEIVE_BUDGET_TEST_PASS" << std::endl;
	return EXIT_SUCCESS;
}
