#include "server/cellapp/witness_load_metrics.h"

#include <cstdlib>
#include <iostream>

namespace KBEngine
{
bool g_performanceProbesEnabled = false;
}

namespace
{
bool require(bool condition, const char* message)
{
	if (!condition)
		std::cerr << message << std::endl;
	return condition;
}

bool testIncrementalViewAccounting()
{
	KBEngine::g_performanceProbesEnabled = true;
	KBEngine::WitnessLoadMetrics metrics;
	std::size_t firstWitness = 0;
	std::size_t secondWitness = 0;

	metrics.synchronizeViewCount(firstWitness, 3);
	metrics.synchronizeViewCount(secondWitness, 5);
	metrics.synchronizeViewCount(firstWitness, 1);

	if (!require(metrics.viewEntities() == 6, "aggregate view count drifted") ||
		!require(metrics.maxViewEntities() == 5, "per-Witness maximum view count was not retained"))
	{
		return false;
	}

	metrics.synchronizeViewCount(firstWitness, 0);
	metrics.synchronizeViewCount(secondWitness, 0);
	return require(metrics.viewEntities() == 0, "released Witness views remained globally accounted");
}

bool testQueueAttribution()
{
	KBEngine::WitnessLoadMetrics metrics;
	metrics.recordDirtyEnqueued(1, false, false, false);
	metrics.recordDirtyEnqueued(2, true);
	metrics.recordDirtyEnqueued(3, false, true, true);
	metrics.recordQueueDeduplicated();
	metrics.recordProducerCoalesced();
	metrics.recordPromotedVolatileSkip();
	metrics.recordDirtyDequeued(1, false);
	metrics.recordDirtyDequeued(1, true);
	metrics.recordDirtyProcessed();
	metrics.recordStaleDiscard();
	metrics.recordStateSkip();
	metrics.recordVolatileBytes(37);
	metrics.recordVolatileBudgetDeferred();
	metrics.recordVolatileBudgetExhaustion();
	metrics.recordSendBytes(91);
	metrics.recordSendBudgetExhaustion();
	metrics.recordStructuralProcessed();
	metrics.recordGlobalAdmission(true);
	metrics.recordGlobalAdmission(false);
	metrics.recordEnter(120);
	metrics.recordLeave(7);
	metrics.recordVolatileUpdate(19);
	metrics.recordBundle(2048);
	metrics.recordBundle(1024);

	return require(metrics.dirtyQueued() == 1, "current dirty queue depth was not maintained") &&
		require(metrics.dirtyEnqueued() == 3, "cumulative enqueue count was not maintained") &&
		require(metrics.dirtyRequeues() == 1, "requeue count was not attributed") &&
		require(metrics.maxQueueDepth() == 3, "maximum queue depth was not retained") &&
		require(metrics.structuralQueued() == 0 && metrics.volatileQueued() == 1,
			"split queue depths were not maintained") &&
		require(metrics.structuralEnqueued() == 1 && metrics.volatileEnqueued() == 2,
			"split queue enqueue totals were not attributed") &&
		require(metrics.queueDeduplicated() == 1 && metrics.producerCoalesced() == 1 &&
			metrics.structuralPromotions() == 1 &&
			metrics.promotedVolatileSkips() == 1, "queue amplification counters drifted") &&
		require(metrics.dirtyProcessed() == 1, "processed count was not maintained") &&
		require(metrics.staleDiscards() == 1, "stale discard was not attributed") &&
		require(metrics.stateSkips() == 1, "state skip was not attributed") &&
		require(metrics.volatileBytesSent() == 37, "volatile byte count was not accumulated") &&
		require(metrics.volatileBudgetDeferred() == 1, "deferred volatile update was not attributed") &&
		require(metrics.volatileBudgetExhaustions() == 1, "volatile budget exhaustion was not attributed") &&
		require(metrics.sendBytes() == 91, "total send bytes were not accumulated") &&
		require(metrics.sendBudgetExhaustions() == 1, "total budget exhaustion was not attributed") &&
		require(metrics.structuralProcessed() == 1, "structural work was not attributed") &&
		require(metrics.globalAdmitted() == 1 && metrics.globalDeferred() == 1,
			"global scheduler attribution drifted") &&
		require(metrics.enterUpdates() == 1 && metrics.enterBytes() == 120,
			"enter work attribution drifted") &&
		require(metrics.leaveUpdates() == 1 && metrics.leaveBytes() == 7,
			"leave work attribution drifted") &&
		require(metrics.volatileUpdates() == 1 && metrics.volatileUpdateBytes() == 19,
			"volatile work attribution drifted") &&
		require(metrics.bundlesSent() == 2, "bundle count was not accumulated") &&
		require(metrics.maxBundleBytes() == 2048, "maximum bundle size was not retained");
}

bool testFullScanWorkAccounting()
{
	KBEngine::WitnessLoadMetrics metrics;
	metrics.recordFullScan(7);
	metrics.recordFullScan(0);

	return require(metrics.fullScans() == 2, "full scan count was not maintained") &&
		require(metrics.fullScanEntities() == 7, "full scan work was not accumulated");
}

bool testDisabledMetrics()
{
	KBEngine::g_performanceProbesEnabled = false;
	KBEngine::WitnessLoadMetrics metrics;
	std::size_t trackedCount = 0;
	metrics.synchronizeViewCount(trackedCount, 5);
	metrics.recordDirtyEnqueued(1, false);
	metrics.recordDirtyDequeued();
	metrics.recordEnter(100);
	return require(trackedCount == 5, "disabled metrics did not preserve Witness lifecycle state") &&
		require(metrics.viewEntities() == 0 && metrics.dirtyEnqueued() == 0 && metrics.enterUpdates() == 0,
			"disabled Witness metrics changed diagnostic counters") &&
		require(!metrics.beginEnterProcessing(), "disabled Witness timing unexpectedly sampled an event");
}

bool testVolatileBackpressureAccounting()
{
	KBEngine::WitnessLoadMetrics metrics;
	metrics.recordVolatileSuppression(true);
	metrics.recordSuppressedUpdateSkip();
	metrics.recordSuppressedVolatileRefresh();
	metrics.recordStructuralWhileSuppressed();
	if (!require(metrics.activeSuppressed() == 1, "active suppression count was not maintained") ||
		!require(metrics.suppressionTransitions() == 1, "suppression transition was not attributed") ||
		!require(metrics.suppressedUpdateSkips() == 1, "suppressed update skip was not attributed") ||
		!require(metrics.suppressedVolatileRefreshes() == 1, "suppressed volatile refresh was not attributed") ||
		!require(metrics.structuralWhileSuppressed() == 1, "structural bypass was not attributed"))
	{
		return false;
	}

	metrics.recordVolatileSuppression(false);
	return require(metrics.activeSuppressed() == 0, "resumed Witness remained suppressed") &&
		require(metrics.resumeTransitions() == 1, "resume transition was not attributed");
}

bool testStructuralProcessingSampling()
{
	KBEngine::WitnessLoadMetrics metrics;
	if (!require(metrics.beginEnterProcessing(), "first Enter event was not sampled") ||
		!require(metrics.beginLeaveProcessing(), "first Leave event was not sampled"))
	{
		return false;
	}

	for (std::size_t i = 1; i < 31; ++i)
	{
		if (!require(!metrics.beginEnterProcessing(), "Enter event sampled before configured period") ||
			!require(!metrics.beginLeaveProcessing(), "Leave event sampled before configured period"))
		{
			return false;
		}
	}

	if (!require(metrics.beginEnterProcessing(), "periodic Enter event was not sampled") ||
		!require(metrics.beginLeaveProcessing(), "periodic Leave event was not sampled"))
	{
		return false;
	}

	metrics.recordEnterProcessing(750000);
	metrics.recordEnterProcessing(2250000);
	metrics.recordLeaveProcessing(1000000);
	metrics.recordLeaveProcessing(3000000);
	return require(metrics.enterProcessing().sampleRate() == 32, "Enter sample rate drifted") &&
		require(metrics.enterProcessing().sampledCalls() == 2, "Enter sample count drifted") &&
		require(metrics.enterProcessing().sampledAverageNanos() == 1500000, "Enter average drifted") &&
		require(metrics.enterProcessing().sampledMaxNanos() == 2250000, "Enter maximum drifted") &&
		require(metrics.enterProcessing().slowSamplesOver1ms() == 1, "Enter slow count drifted") &&
		require(metrics.leaveProcessing().sampledAverageNanos() == 2000000, "Leave average drifted") &&
		require(metrics.leaveProcessing().sampledMaxNanos() == 3000000, "Leave maximum drifted") &&
		require(metrics.leaveProcessing().slowSamplesOver1ms() == 2, "Leave slow count drifted");
}
}

int main()
{
	if (!testDisabledMetrics() || !testIncrementalViewAccounting() || !testQueueAttribution() || !testFullScanWorkAccounting() ||
		!testVolatileBackpressureAccounting() || !testStructuralProcessingSampling())
		return EXIT_FAILURE;

	std::cout << "WITNESS_LOAD_METRICS_TEST_PASS" << std::endl;
	return EXIT_SUCCESS;
}
