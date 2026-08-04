#include "server/script_stage_metrics.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace
{
bool require(bool condition, const char* message)
{
	if (!condition)
		std::cerr << message << std::endl;
	return condition;
}

bool testSamplingAndStages()
{
	KBEngine::ScriptStageMetrics metrics;
	bool sampled[8] = {};
	for (std::size_t i = 0; i < 8; ++i)
		sampled[i] = metrics.beginRpcCall();

	metrics.record(KBEngine::SCRIPT_STAGE_RPC_LOOKUP, 500000, sampled[0], "Avatar.first");
	metrics.record(KBEngine::SCRIPT_STAGE_RPC_LOOKUP, 1500000, false, "Avatar.unsampled");
	metrics.record(KBEngine::SCRIPT_STAGE_RPC_LOOKUP, 2500000, sampled[7], "Avatar.eighth");

	const KBEngine::ScriptStageStats& stats = metrics.stats(KBEngine::SCRIPT_STAGE_RPC_LOOKUP);
	return require(sampled[0] && sampled[7], "first and eighth RPC calls were not sampled") &&
		require(!sampled[1] && !sampled[6], "unexpected RPC call was sampled") &&
		require(metrics.rpcCalls() == 8, "RPC call count drifted") &&
		require(stats.calls() == 3, "stage call count drifted") &&
		require(stats.sampledCalls() == 2, "stage sample count drifted") &&
		require(stats.totalNanos() == 3000000, "stage total drifted") &&
		require(stats.averageNanos() == 1500000, "stage average drifted") &&
		require(stats.maxNanos() == 2500000, "stage maximum drifted") &&
		require(stats.slowOver1ms() == 1, "slow sample count drifted") &&
		require(metrics.slow(0).name == "Avatar.eighth", "slow handler name drifted");
}

bool testBoundedSlowTop()
{
	KBEngine::ScriptStageMetrics metrics;
	for (std::size_t i = 0; i < 12; ++i)
	{
		const std::string name = "handler" + std::to_string(i);
		metrics.record(KBEngine::SCRIPT_STAGE_PYTHON_CALL,
			1000000 + static_cast<std::uint64_t>(i) * 100000, true, name.c_str());
	}

	return require(metrics.slow(0).name == "handler11", "slow Top-N was not descending") &&
		require(metrics.slow(7).name == "handler4", "slow Top-N did not evict the smallest sample") &&
		require(metrics.slow(0).stage == KBEngine::SCRIPT_STAGE_PYTHON_CALL, "slow stage drifted") &&
		require(metrics.slowDurationNanos(7) == 1400000, "slow duration drifted") &&
		require(metrics.slowStageName(0) == "pythonCall", "slow stage name drifted");
}

bool testSameHandlerKeepsMaximum()
{
	KBEngine::ScriptStageMetrics metrics;
	metrics.record(KBEngine::SCRIPT_STAGE_PYTHON_CALL, 2000000, true, "Avatar.move");
	metrics.record(KBEngine::SCRIPT_STAGE_PYTHON_CALL, 1500000, true, "Avatar.move");
	metrics.record(KBEngine::SCRIPT_STAGE_PYTHON_CALL, 3000000, true, "Avatar.move");

	return require(metrics.slow(0).name == "Avatar.move", "same handler disappeared") &&
		require(metrics.slow(0).durationNanos == 3000000, "same handler did not retain its maximum") &&
		require(metrics.slow(1).durationNanos == 0, "same handler occupied multiple slots");
}
}

int main()
{
	if (!testSamplingAndStages() || !testBoundedSlowTop() || !testSameHandlerKeepsMaximum())
		return EXIT_FAILURE;

	std::cout << "SCRIPT_STAGE_METRICS_TEST_PASS" << std::endl;
	return EXIT_SUCCESS;
}
