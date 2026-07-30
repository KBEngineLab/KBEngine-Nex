#include "network/udp_send_backpressure.h"

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
	KBEngine::Network::UdpSendBackpressure state;
	const KBEngine::uint64 timeout = 5000;

	// 调用频率不能替代真实持续时间；同一瞬间的拒绝风暴不能关闭健康连接。
	// Call frequency cannot substitute for elapsed time; a rejection burst at one instant must not close a healthy peer.
	for (int i = 0; i < 1000; ++i)
	{
		if (!require(!state.recordBlocked(100, timeout), "same-timestamp rejection burst expired"))
			return EXIT_FAILURE;
	}

	if (!require(!state.recordBlocked(5099, timeout), "backpressure expired before timeout") ||
		!require(state.recordBlocked(5100, timeout), "sustained backpressure did not expire"))
	{
		return EXIT_FAILURE;
	}

	// 任意一次成功发送都证明队列已取得进展，下一次拒绝必须开始新的周期。
	// Any successful send proves queue progress, so the next rejection must begin a new period.
	state.recordProgress();
	if (!require(state.blockedSince() == 0, "progress did not reset the blocked period") ||
		!require(!state.recordBlocked(6000, timeout), "new blocked period inherited the old deadline") ||
		!require(!state.recordBlocked(10999, timeout), "new blocked period expired early") ||
		!require(state.recordBlocked(11000, timeout), "new blocked period did not expire"))
	{
		return EXIT_FAILURE;
	}

	std::cout << "UDP_SEND_BACKPRESSURE_TEST_PASS" << std::endl;
	return EXIT_SUCCESS;
}
