#include "network/kcp_update_queue.h"

#include <cstdlib>
#include <iostream>
#include <vector>

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

bool testDeadlineOrderAndDeduplication()
{
	KBEngine::Network::KcpUpdateQueue queue;
	queue.schedule(1, 30);
	queue.schedule(2, 10);
	queue.schedule(3, 20);
	if (!require(!queue.schedule(2, 40), "later duplicate replaced an earlier deadline") ||
		!require(queue.scheduledCount() == 3, "deduplication changed active count"))
	{
		return false;
	}

	KBEngine::Network::KcpUpdateQueue::Key key = 0;
	return require(!queue.takeDue(9, key), "entry fired before deadline") &&
		require(queue.takeDue(10, key) && key == 2, "first deadline order changed") &&
		require(queue.takeDue(20, key) && key == 3, "second deadline order changed") &&
		require(queue.takeDue(30, key) && key == 1, "third deadline order changed") &&
		require(queue.scheduledCount() == 0, "due entries remained active");
}

bool testEarlierReplacementAndStaleEntry()
{
	KBEngine::Network::KcpUpdateQueue queue;
	queue.schedule(7, 100);
	if (!require(queue.schedule(7, 25), "earlier deadline was not accepted"))
	{
		return false;
	}

	KBEngine::Network::KcpUpdateQueue::Key key = 0;
	KBEngine::Network::KcpUpdateQueue::Time next = 0;
	return require(queue.nextDue(next) && next == 25, "replacement deadline is incorrect") &&
		require(queue.takeDue(25, key) && key == 7, "replacement did not fire") &&
		require(!queue.takeDue(100, key), "stale replaced entry fired") &&
		require(queue.earlierReplacementCount() == 1, "replacement metric is incorrect") &&
		require(queue.staleDiscardCount() == 1, "stale entry was not accounted");
}

bool testCancellationAndKeyReuse()
{
	KBEngine::Network::KcpUpdateQueue queue;
	queue.schedule(11, 15);
	if (!require(queue.cancel(11), "active entry cancellation failed") ||
		!require(!queue.isScheduled(11), "cancelled entry remained active"))
	{
		return false;
	}

	// Reusing the same key models a pooled Channel returning at the same address with a new lifecycle token.
	// 复用相同 key 模拟对象池 Channel 以相同地址进入新的生命周期 token。
	queue.schedule(11, 40);
	KBEngine::Network::KcpUpdateQueue::Key key = 0;
	return require(!queue.takeDue(15, key), "cancelled lifecycle fired after key reuse") &&
		require(queue.takeDue(40, key) && key == 11, "new lifecycle did not fire") &&
		require(queue.cancellationCount() == 1, "cancellation metric is incorrect");
}

bool testBoundedStaleCompaction()
{
	KBEngine::Network::KcpUpdateQueue queue;
	queue.schedule(99, 1000);
	for (KBEngine::Network::KcpUpdateQueue::Time due = 999; due > 700; --due)
	{
		queue.schedule(99, due);
	}

	if (!require(queue.compactionCount() > 0, "stale heap was never compacted") ||
		!require(queue.heapEntryCount() <= queue.scheduledCount() * 2 + 64, "stale heap exceeded compaction bound"))
	{
		return false;
	}

	KBEngine::Network::KcpUpdateQueue::Key key = 0;
	return require(queue.takeDue(701, key) && key == 99, "latest compacted deadline did not fire") &&
		require(queue.scheduledCount() == 0, "compacted entry remained active");
}

bool testRescheduleAfterTake()
{
	KBEngine::Network::KcpUpdateQueue queue;
	queue.schedule(5, 1);
	KBEngine::Network::KcpUpdateQueue::Key key = 0;
	if (!require(queue.takeDue(1, key) && key == 5, "initial entry did not fire"))
	{
		return false;
	}

	queue.schedule(key, 2);
	return require(queue.takeDue(2, key) && key == 5, "callback-style reschedule did not fire") &&
		require(queue.scheduleRequestCount() == 2, "schedule request metric is incorrect");
}

bool testDueAndOverdueMetrics()
{
	KBEngine::Network::KcpUpdateQueue queue;
	queue.schedule(1, 10);
	queue.schedule(2, 20);
	queue.schedule(3, 30);

	KBEngine::Network::KcpUpdateQueue::Key key = 0;
	KBEngine::Network::KcpUpdateQueue::Time dueTime = 0;
	return require(queue.dueCount(20) == 2, "due count is incorrect") &&
		require(queue.overdueCount(20) == 1, "overdue count is incorrect") &&
		require(queue.takeDue(20, key, &dueTime) && key == 1 && dueTime == 10,
			"taken deadline was not reported") &&
		require(queue.dueCount(20) == 1, "taken entry remained in due count");
}

bool testScheduledIterationUsesAuthoritativeEntries()
{
	KBEngine::Network::KcpUpdateQueue queue;
	queue.schedule(1, 30);
	queue.schedule(2, 10);
	queue.schedule(3, 20);
	queue.schedule(1, 25);

	KBEngine::Network::KcpUpdateQueue::Key taken = 0;
	if (!require(queue.takeDue(10, taken) && taken == 2, "due entry was not transferred") ||
		!require(queue.cancel(3), "scheduled entry cancellation failed"))
	{
		return false;
	}

	std::vector<KBEngine::Network::KcpUpdateQueue::Key> visited;
	queue.forEachScheduledKey([&visited](KBEngine::Network::KcpUpdateQueue::Key key)
	{
		visited.push_back(key);
	});

	// Replacement heap nodes, transferred callbacks and cancelled entries are not active
	// Channels and must not be counted by a diagnostic snapshot.
	// 被替换的堆节点、已转交回调的项和已取消项都不是活跃 Channel，诊断快照不得计入。
	return require(visited.size() == 1 && visited[0] == 1,
		"scheduled iteration exposed a stale or inactive entry");
}
}

int main()
{
	if (!testDeadlineOrderAndDeduplication() ||
		!testEarlierReplacementAndStaleEntry() ||
		!testCancellationAndKeyReuse() ||
		!testBoundedStaleCompaction() ||
		!testRescheduleAfterTake() ||
		!testDueAndOverdueMetrics() ||
		!testScheduledIterationUsesAuthoritativeEntries())
	{
		return EXIT_FAILURE;
	}

	std::cout << "KCP_UPDATE_QUEUE_TEST_PASS" << std::endl;
	return EXIT_SUCCESS;
}
