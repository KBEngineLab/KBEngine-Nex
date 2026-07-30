#include "network/completion_udp_send_budget.h"

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
	KBEngine::Network::CompletionUdpSendBudget budget;
	const KBEngine::uint64 first = 1;
	const KBEngine::uint64 second = 2;
	const size_t packetBytes = 1024;
	const size_t destinationLimit = 128 * 1024;

	// 单一目标只能占用 128 KiB，避免一个 KCP Channel 填满共享 UDP Socket 的全部队列。
	// One destination may occupy only 128 KiB so one KCP Channel cannot fill the shared UDP socket queue.
	for (int i = 0; i < 128; ++i)
	{
		if (!require(budget.tryReserve(first, packetBytes, destinationLimit), "first destination rejected before its limit"))
			return EXIT_FAILURE;
	}

	if (!require(!budget.tryReserve(first, packetBytes, destinationLimit), "first destination exceeded its limit") ||
		!require(budget.tryReserve(second, packetBytes, destinationLimit), "second destination was starved by the first"))
	{
		return EXIT_FAILURE;
	}

	// 排出一个数据报后必须同步释放该目标的配额，否则目标会永久处于背压状态。
	// Dequeuing one datagram must release that destination's quota or it would remain backpressured forever.
	budget.release(first, packetBytes);
	if (!require(budget.tryReserve(first, packetBytes, destinationLimit), "destination quota did not recover"))
		return EXIT_FAILURE;

	budget.clear();
	if (!require(budget.pendingBytes(first) == 0 && budget.pendingBytes(second) == 0, "clear left stale destination bytes"))
		return EXIT_FAILURE;

	std::cout << "COMPLETION_UDP_SEND_QUEUE_TEST_PASS" << std::endl;
	return EXIT_SUCCESS;
}
