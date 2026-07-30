#include "network/completion_rearm_queue.h"

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
	KBEngine::Network::CompletionRearmQueue queue;
	const KBEngine::KBESOCKET first = static_cast<KBEngine::KBESOCKET>(11);
	const KBEngine::KBESOCKET second = static_cast<KBEngine::KBESOCKET>(22);
	KBEngine::KBESOCKET fd = 0;
	KBEngine::uint8 flags = KBEngine::Network::CompletionRearmQueue::NONE;

	// 同一 fd 的读写请求必须合并，避免突发失败按包或按 completion 放大队列。
	// Read and write requests for one fd must coalesce so bursts cannot amplify the queue per packet or completion.
	queue.request(first, KBEngine::Network::CompletionRearmQueue::READ);
	queue.request(first, KBEngine::Network::CompletionRearmQueue::READ);
	queue.request(first, KBEngine::Network::CompletionRearmQueue::WRITE);
	if (!require(queue.size() == 1, "duplicate requests were not coalesced") ||
		!require(queue.take(fd, flags) && fd == first &&
			(flags & KBEngine::Network::CompletionRearmQueue::ALL) == KBEngine::Network::CompletionRearmQueue::ALL,
			"coalesced read/write request was not preserved"))
	{
		return EXIT_FAILURE;
	}

	// 部分取消只能移除对应方向；剩余写请求仍要保持可见。
	// Partial cancellation removes only its direction while the remaining write request stays visible.
	queue.request(first, KBEngine::Network::CompletionRearmQueue::READ);
	queue.request(first, KBEngine::Network::CompletionRearmQueue::WRITE);
	queue.cancel(first, KBEngine::Network::CompletionRearmQueue::READ);
	if (!require(queue.take(fd, flags) && fd == first && flags == KBEngine::Network::CompletionRearmQueue::WRITE,
		"partial cancellation removed the wrong rearm direction"))
	{
		return EXIT_FAILURE;
	}

	// 本轮失败项重新进入队尾后，原本排在后面的 fd 必须先获得下一次机会。
	// When a failed item returns to the tail, the descriptor already behind it must receive the next opportunity first.
	queue.request(first, KBEngine::Network::CompletionRearmQueue::READ);
	queue.request(second, KBEngine::Network::CompletionRearmQueue::WRITE);
	if (!require(queue.take(fd, flags) && fd == first, "FIFO did not return the first descriptor"))
	{
		return EXIT_FAILURE;
	}
	queue.request(first, KBEngine::Network::CompletionRearmQueue::READ);
	if (!require(queue.take(fd, flags) && fd == second, "retry did not rotate behind existing work") ||
		!require(queue.take(fd, flags) && fd == first, "rotated retry request was lost"))
	{
		return EXIT_FAILURE;
	}

	// 完全取消会留下 O(1) 可跳过的 deque 项；后续 fd 必须仍按 FIFO 取出。
	// Full cancellation leaves an O(1) stale deque entry that must be skipped without disturbing FIFO order for later descriptors.
	queue.request(first, KBEngine::Network::CompletionRearmQueue::READ);
	queue.cancel(first);
	queue.request(second, KBEngine::Network::CompletionRearmQueue::WRITE);
	if (!require(queue.take(fd, flags) && fd == second && flags == KBEngine::Network::CompletionRearmQueue::WRITE,
		"stale cancellation entry blocked the next descriptor") ||
		!require(queue.size() == 0 && !queue.take(fd, flags), "rearm queue did not drain cleanly"))
	{
		return EXIT_FAILURE;
	}

	std::cout << "COMPLETION_REARM_QUEUE_TEST_PASS" << std::endl;
	return EXIT_SUCCESS;
}
