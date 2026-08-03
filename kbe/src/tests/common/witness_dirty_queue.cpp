#include "server/cellapp/witness_dirty_queue.h"

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

bool testDeduplicationAndFifo()
{
	KBEngine::WitnessDirtyQueue queue;
	bool firstQueued = false;
	bool secondQueued = false;
	if (!require(queue.enqueue(11, 101, firstQueued), "first entry was not queued") ||
		!require(!queue.enqueue(11, 101, firstQueued), "duplicate entry was queued") ||
		!require(queue.enqueue(22, 202, secondQueued), "second entry was not queued"))
	{
		return false;
	}

	KBEngine::WitnessDirtyQueue::Entry entry{};
	return require(queue.pop(entry) && entry.entityID == 11 && entry.generation == 101,
		"queue did not preserve FIFO order") &&
		require(queue.pop(entry) && entry.entityID == 22 && entry.generation == 202,
			"queue did not preserve the second FIFO entry");
}

bool testGenerationIsolation()
{
	KBEngine::WitnessDirtyQueue queue;
	bool staleQueued = false;
	bool currentQueued = false;
	queue.enqueue(77, 1, staleQueued);
	queue.enqueue(77, 2, currentQueued);

	KBEngine::WitnessDirtyQueue::Entry staleEntry{};
	if (!require(queue.pop(staleEntry), "stale entry was not available"))
		return false;

	const std::uint64_t currentGeneration = 2;
	KBEngine::WitnessDirtyQueue::Entry currentEntry{};
	return require(staleEntry.entityID == 77 && staleEntry.generation != currentGeneration,
		"same-ID lifecycle was not isolated by generation") &&
		require(queue.pop(currentEntry) && currentEntry.entityID == 77 &&
			currentEntry.generation == currentGeneration,
			"current same-ID lifecycle was blocked by a stale entry");
}

bool testBatchSnapshotDefersRequeue()
{
	KBEngine::WitnessDirtyQueue queue;
	bool queued = false;
	queue.enqueue(33, 303, queued);
	const std::size_t batchSize = queue.batchSize();

	KBEngine::WitnessDirtyQueue::Entry entry{};
	if (!require(batchSize == 1 && queue.pop(entry), "initial batch was invalid"))
		return false;

	queued = false;
	queue.enqueue(entry.entityID, entry.generation, queued);
	return require(queue.size() == 1, "requeued entry was consumed in its original batch");
}

bool testRingWrapAndGrowthPreserveFifo()
{
	KBEngine::WitnessDirtyQueue queue;
	bool queued[14]{};
	for (std::uint32_t id = 1; id <= 8; ++id)
		queue.enqueue(id, id, queued[id]);

	KBEngine::WitnessDirtyQueue::Entry entry{};
	for (std::uint32_t expected = 1; expected <= 4; ++expected)
	{
		if (!require(queue.pop(entry) && entry.entityID == expected,
			"ring queue lost FIFO order before wrapping"))
		{
			return false;
		}
	}

	for (std::uint32_t id = 9; id <= 13; ++id)
		queue.enqueue(id, id, queued[id]);

	for (std::uint32_t expected = 5; expected <= 13; ++expected)
	{
		if (!require(queue.pop(entry) && entry.entityID == expected,
			"ring queue lost FIFO order while wrapping or growing"))
		{
			return false;
		}
	}

	return require(queue.size() == 0, "ring queue did not drain completely");
}

bool testStructuralQueueDoesNotScanVolatileBacklog()
{
	KBEngine::WitnessDirtyQueue volatileQueue;
	KBEngine::WitnessDirtyQueue structuralQueue;
	bool volatileQueued[101]{};
	for (std::uint32_t id = 1; id <= 100; ++id)
		volatileQueue.enqueue(id, id, volatileQueued[id]);

	bool structuralQueued = false;
	if (!require(structuralQueue.enqueue(100, 100, structuralQueued),
		"structural promotion was not queued") ||
		!require(!structuralQueue.enqueue(100, 100, structuralQueued),
			"duplicate structural promotion was queued"))
	{
		return false;
	}

	KBEngine::WitnessDirtyQueue::Entry entry{};
	return require(structuralQueue.pop(entry) && entry.entityID == 100,
		"structural queue did not bypass the volatile backlog") &&
		require(volatileQueue.size() == 100,
			"structural processing scanned or reordered the volatile backlog");
}
}

int main()
{
	if (!testDeduplicationAndFifo() || !testGenerationIsolation() ||
		!testBatchSnapshotDefersRequeue() || !testRingWrapAndGrowthPreserveFifo() ||
		!testStructuralQueueDoesNotScanVolatileBacklog())
		return EXIT_FAILURE;

	std::cout << "WITNESS_DIRTY_QUEUE_TEST_PASS" << std::endl;
	return EXIT_SUCCESS;
}
