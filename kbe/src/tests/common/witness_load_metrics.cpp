#include "server/cellapp/witness_load_metrics.h"

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

bool testIncrementalViewAccounting()
{
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
	metrics.recordDirtyEnqueued(1, false);
	metrics.recordDirtyEnqueued(2, true);
	metrics.recordDirtyDequeued();
	metrics.recordDirtyProcessed();
	metrics.recordStaleDiscard();
	metrics.recordStateSkip();
	metrics.recordVolatileBytes(37);
	metrics.recordVolatileBudgetDeferred();
	metrics.recordVolatileBudgetExhaustion();
	metrics.recordSendBytes(91);
	metrics.recordSendBudgetExhaustion();
	metrics.recordStructuralProcessed();
	metrics.recordBundle(2048);
	metrics.recordBundle(1024);

	return require(metrics.dirtyQueued() == 1, "current dirty queue depth was not maintained") &&
		require(metrics.dirtyEnqueued() == 2, "cumulative enqueue count was not maintained") &&
		require(metrics.dirtyRequeues() == 1, "requeue count was not attributed") &&
		require(metrics.maxQueueDepth() == 2, "maximum queue depth was not retained") &&
		require(metrics.dirtyProcessed() == 1, "processed count was not maintained") &&
		require(metrics.staleDiscards() == 1, "stale discard was not attributed") &&
		require(metrics.stateSkips() == 1, "state skip was not attributed") &&
		require(metrics.volatileBytesSent() == 37, "volatile byte count was not accumulated") &&
		require(metrics.volatileBudgetDeferred() == 1, "deferred volatile update was not attributed") &&
		require(metrics.volatileBudgetExhaustions() == 1, "volatile budget exhaustion was not attributed") &&
		require(metrics.sendBytes() == 91, "total send bytes were not accumulated") &&
		require(metrics.sendBudgetExhaustions() == 1, "total budget exhaustion was not attributed") &&
		require(metrics.structuralProcessed() == 1, "structural work was not attributed") &&
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
}

int main()
{
	if (!testIncrementalViewAccounting() || !testQueueAttribution() || !testFullScanWorkAccounting())
		return EXIT_FAILURE;

	std::cout << "WITNESS_LOAD_METRICS_TEST_PASS" << std::endl;
	return EXIT_SUCCESS;
}
