#include "server/cellapp/witness_volatile_budget.h"
#include "server/cellapp/witness_update_scheduler.h"
#include "server/cellapp/witness_delayed_queue.h"
#include "server/cellapp/witness_volatile_lod.h"

#include <cstdlib>
#include <limits>
#include <iostream>

namespace
{
bool require(bool condition, const char* message)
{
	if (!condition)
		std::cerr << message << std::endl;
	return condition;
}

bool testBoundedBudgetAllowsOneCompleteUpdate()
{
	KBEngine::WitnessVolatileBudget budget(16);
	budget.recordBundleGrowth(100, 112);
	if (!require(budget.canSend(false), "budget stopped before reaching its byte limit"))
		return false;

	budget.recordBundleGrowth(112, 120);
	return require(!budget.canSend(false), "budget admitted another update after reaching its limit") &&
		require(budget.canSend(true), "structural update was blocked by the volatile byte budget") &&
		require(budget.bytesSent() == 20, "budget did not retain the complete encoded update size");
}

bool testUnlimitedAndShrinkingBundle()
{
	KBEngine::WitnessVolatileBudget budget(0);
	budget.recordBundleGrowth(20, 12);
	budget.recordBundleGrowth(12, 100000);
	return require(budget.canSend(false), "zero byte limit did not preserve unlimited mode") &&
		require(budget.bytesSent() == 99988, "bundle growth accounting underflowed or drifted");
}

bool testAdaptiveTotalBudget()
{
	using KBEngine::witnessEffectiveByteLimit;
	return require(witnessEffectiveByteLimit(2048, 1048576, 1000) == 1048,
		"global target did not reduce the per-Witness limit") &&
		require(witnessEffectiveByteLimit(512, 1048576, 1000) == 512,
			"global target incorrectly raised the per-Witness limit") &&
		require(witnessEffectiveByteLimit(0, 1048576, 2000000) == 1,
			"oversubscribed global target did not preserve forward progress") &&
		require(witnessEffectiveByteLimit(2048, 0, 10000) == 2048,
			"disabled global target changed the configured limit") &&
		require(witnessEffectiveByteLimit(0, 0, 10000) == 0,
			"fully unlimited mode did not remain unlimited");
}

bool testRotatingGlobalAdmission()
{
	KBEngine::WitnessUpdateScheduler scheduler;
	bool admitted[6] = {};
	scheduler.beginTick(6, 2);
	for (std::size_t index = 0; index < 6; ++index)
		admitted[index] = scheduler.admit();
	if (!require(admitted[0] && admitted[1] && !admitted[2] && !admitted[5],
		"first admission window was not bounded"))
	{
		return false;
	}

	scheduler.beginTick(6, 2);
	for (std::size_t index = 0; index < 6; ++index)
		admitted[index] = scheduler.admit();
	if (!require(!admitted[0] && !admitted[1] && admitted[2] && admitted[3] && !admitted[4],
		"admission window did not rotate"))
	{
		return false;
	}

	scheduler.beginTick(6, 2);
	for (std::size_t index = 0; index < 6; ++index)
		admitted[index] = scheduler.admit();
	return require(!admitted[0] && admitted[4] && admitted[5],
		"rotating admission did not reach the tail") &&
		require(scheduler.nextStart() == 0, "admission cursor did not wrap");
}

bool testPendingAdmissionPrefersWorkAndRotates()
{
	KBEngine::WitnessUpdateScheduler scheduler;
	scheduler.beginTick(6, 2, 3);
	bool admitted[6] = {};
	const bool pending[6] = { false, false, true, false, true, true };
	for (std::size_t index = 0; index < 6; ++index)
		admitted[index] = scheduler.admit(pending[index]);
	if (!require(!admitted[0] && !admitted[1] && admitted[2] && !admitted[3] &&
		admitted[4] && !admitted[5], "pending admission did not reserve its rotating window"))
		return false;

	scheduler.beginTick(6, 2, 3);
	for (std::size_t index = 0; index < 6; ++index)
		admitted[index] = scheduler.admit(pending[index]);
	return require(admitted[5] && admitted[2],
		"pending admission did not rotate across ticks") &&
		require(scheduler.admissionCount() == 2, "pending admission changed the global limit");
}

bool testPendingBelowLimitFillsRemainingSlots()
{
	KBEngine::WitnessUpdateScheduler scheduler;
	scheduler.beginTick(5, 4, 1);
	bool admitted[5] = {};
	const bool pending[5] = { false, false, false, true, false };
	for (std::size_t index = 0; index < 5; ++index)
		admitted[index] = scheduler.admit(pending[index]);
	return require(admitted[0] && admitted[1] && admitted[2] && admitted[3],
		"non-pending work did not fill slots left by a small pending set") &&
		require(!admitted[4], "admission exceeded the configured limit");
}

bool testUnlimitedAdmissionIgnoresPendingPartition()
{
	KBEngine::WitnessUpdateScheduler scheduler;
	scheduler.beginTick(4, 0, 2);
	const bool pending[4] = { false, true, false, true };
	for (std::size_t index = 0; index < 4; ++index)
	{
		if (!require(scheduler.admit(pending[index]),
			"unlimited admission rejected an active Witness"))
			return false;
	}
	return require(!scheduler.admit(false), "unlimited admission exceeded the active Witness count");
}

bool testVolatileDistanceSemantics()
{
	using KBEngine::witnessVolatileWithinDistance;
	return require(!witnessVolatileWithinDistance(0.f, 0.f), "NEVER volatile field was enabled") &&
		require(witnessVolatileWithinDistance(std::numeric_limits<float>::max(), 1.0e20f),
			"ALWAYS volatile field was distance limited") &&
		require(witnessVolatileWithinDistance(20.f, 400.f), "distance boundary was excluded") &&
		require(!witnessVolatileWithinDistance(20.f, 400.01f), "field exceeded its configured distance");
}

bool testDenseViewTemporalLod()
{
	const KBEngine::WitnessVolatileLodConfig config = { true, 32, 20.f, 50.f, 2, 4 };
	using KBEngine::witnessNextVolatileTick;
	using KBEngine::witnessVolatilePhaseKey;
	using KBEngine::witnessVolatileIntervalTicks;
	return require(witnessVolatileIntervalTicks(config, 31, 10000.f) == 1,
		"small view unexpectedly enabled temporal LOD") &&
		require(witnessVolatileIntervalTicks(config, 100, 400.f) == 1,
			"near relation lost full tick rate") &&
		require(witnessVolatileIntervalTicks(config, 100, 1600.f) == 2,
			"medium relation used the wrong interval") &&
		require(witnessVolatileIntervalTicks(config, 100, 10000.f) == 4,
			"far relation used the wrong interval") &&
		require(witnessNextVolatileTick(100, 4, 2) == 102,
			"stable phase did not select the expected future tick") &&
		require(witnessNextVolatileTick(102, 4, 2) == 106,
			"stable phase did not preserve cadence") &&
		require(witnessVolatilePhaseKey(1, 100) != witnessVolatilePhaseKey(2, 100),
			"relation phasing synchronized all observers of the same target");
}

bool testDelayedQueueOrdersDueEntriesAndKeepsOwnership()
{
	KBEngine::WitnessDelayedQueue queue;
	KBEngine::WitnessDirtyQueue activeQueue;
	bool firstQueued = false;
	bool secondQueued = false;
	if (!require(queue.schedule(1, 10, 3, 8, 5, firstQueued), "first delayed relation was rejected") ||
		!require(queue.schedule(2, 20, 3, 4, 5, secondQueued), "second delayed relation was rejected") ||
		!require(firstQueued && secondQueued, "delayed queue did not retain relation ownership"))
	{
		return false;
	}

	KBEngine::WitnessDirtyQueue::Entry entry = {};
	queue.activateDue(3, activeQueue);
	if (!require(activeQueue.size() == 0, "future relation was released early"))
		return false;
	queue.activateDue(4, activeQueue);
	if (!require(activeQueue.pop(entry) && entry.entityID == 2 && entry.generation == 20,
			"delayed queue did not release the earliest due relation") &&
		!require(queue.size() == 1, "delayed queue lost the later relation"))
	{
		return false;
	}
	queue.activateDue(7, activeQueue);
	if (!require(activeQueue.size() == 0, "later relation was released early"))
		return false;
	queue.activateDue(8, activeQueue);
	return require(activeQueue.pop(entry) && entry.entityID == 1 && queue.size() == 0,
		"delayed queue did not drain in due order");
}

bool testDirtyQueueTrimsOnlyWhenEmpty()
{
	KBEngine::WitnessDirtyQueue queue;
	bool queued = false;
	for (std::uint32_t entityID = 1; entityID <= 128; ++entityID)
	{
		queued = false;
		queue.enqueue(entityID, entityID, queued);
	}
	queue.trimEmpty(8);
	if (!require(queue.size() == 128, "non-empty dirty queue was trimmed"))
		return false;

	KBEngine::WitnessDirtyQueue::Entry entry = {};
	while (queue.pop(entry))
	{
	}
	queue.trimEmpty(8);
	queued = false;
	return require(queue.enqueue(999, 999, queued) && queue.size() == 1,
		"trimmed dirty queue could not be reused");
}
}

int main()
{
	if (!testBoundedBudgetAllowsOneCompleteUpdate() || !testUnlimitedAndShrinkingBundle() ||
		!testAdaptiveTotalBudget() || !testRotatingGlobalAdmission() ||
		!testPendingAdmissionPrefersWorkAndRotates() || !testPendingBelowLimitFillsRemainingSlots() ||
		!testUnlimitedAdmissionIgnoresPendingPartition() ||
		!testVolatileDistanceSemantics() || !testDenseViewTemporalLod() ||
		!testDelayedQueueOrdersDueEntriesAndKeepsOwnership() || !testDirtyQueueTrimsOnlyWhenEmpty())
		return EXIT_FAILURE;

	std::cout << "WITNESS_VOLATILE_BUDGET_TEST_PASS" << std::endl;
	return EXIT_SUCCESS;
}
