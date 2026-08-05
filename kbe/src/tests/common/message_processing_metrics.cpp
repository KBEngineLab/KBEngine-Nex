#include "network/message_processing_metrics.h"

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

bool testClassification()
{
	using namespace KBEngine::Network;
	return require(MessageProcessingMetrics::classify("CellappInterface::onUpdateDataFromClient") ==
		MESSAGE_PROCESSING_CLIENT_MOVEMENT, "client movement classification drifted") &&
		require(MessageProcessingMetrics::classify("BaseappInterface::onEntityCall") ==
		MESSAGE_PROCESSING_PYTHON_METHOD, "Python method classification drifted") &&
		require(MessageProcessingMetrics::classify("CellappInterface::reqTeleportToCellApp") ==
		MESSAGE_PROCESSING_CELL_MIGRATION, "Cell migration classification drifted") &&
		require(MessageProcessingMetrics::classify("CellappInterface::queryWatcher") ==
		MESSAGE_PROCESSING_WATCHER_CONTROL, "Watcher classification drifted") &&
		require(MessageProcessingMetrics::classify("CellappInterface::lookApp") ==
		MESSAGE_PROCESSING_OTHER, "other-message classification drifted");
}

bool testDeterministicSampling()
{
	KBEngine::Network::MessageProcessingCategoryStats stats(4);
	bool sampled[8] = {};
	for (std::size_t i = 0; i < 8; ++i)
		sampled[i] = stats.beginCall();

	if (!require(sampled[0] && sampled[3] && sampled[7], "first and periodic calls were not sampled") ||
		!require(!sampled[1] && !sampled[2] && !sampled[4] && !sampled[5] && !sampled[6],
			"unexpected calls were sampled"))
	{
		return false;
	}

	stats.recordSample(500000, 41, "Client::fastMessage");
	stats.recordSample(1500000, 42, "Client::slowMessage");
	stats.recordSample(1000000);
	return require(stats.calls() == 8, "call count drifted") &&
		require(stats.sampledCalls() == 3, "sample count drifted") &&
		require(stats.sampledTotalNanos() == 3000000, "sample duration total drifted") &&
		require(stats.sampledAverageNanos() == 1000000, "sample duration average drifted") &&
		require(stats.sampledMaxNanos() == 1500000, "sample duration maximum drifted") &&
		require(stats.slowestHandlerID() == 42, "slowest handler ID drifted") &&
		require(stats.slowestHandlerName() == "Client::slowMessage", "slowest handler name drifted") &&
		require(stats.slowSamplesOver1ms() == 2, "slow sample count drifted") &&
		require(stats.sampleRate() == 4, "sample rate drifted");
}

bool testCategoryRates()
{
	KBEngine::Network::MessageProcessingMetrics metrics;
	return require(metrics.stats(KBEngine::Network::MESSAGE_PROCESSING_CLIENT_MOVEMENT).sampleRate() == 64,
		"client movement sample rate drifted") &&
		require(metrics.stats(KBEngine::Network::MESSAGE_PROCESSING_PYTHON_METHOD).sampleRate() == 8,
			"Python method sample rate drifted") &&
		require(metrics.stats(KBEngine::Network::MESSAGE_PROCESSING_CELL_MIGRATION).sampleRate() == 1,
			"Cell migration sample rate drifted") &&
		require(metrics.stats(KBEngine::Network::MESSAGE_PROCESSING_WATCHER_CONTROL).sampleRate() == 1,
			"Watcher sample rate drifted") &&
		require(metrics.stats(KBEngine::Network::MESSAGE_PROCESSING_OTHER).sampleRate() == 256,
			"other-message sample rate drifted");
}
}

int main()
{
	if (!testClassification() || !testDeterministicSampling() || !testCategoryRates())
		return EXIT_FAILURE;

	std::cout << "MESSAGE_PROCESSING_METRICS_TEST_PASS" << std::endl;
	return EXIT_SUCCESS;
}
