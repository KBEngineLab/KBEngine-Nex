#include "helper/profile_latency.h"

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

bool testExactPercentilesAndExpiry()
{
	KBEngine::ProfileLatencyWindow window(8, 100);
	window.record(10, 100);
	window.record(20, 101);
	window.record(30, 102);
	window.record(40, 103);

	const KBEngine::ProfileLatencyWindow::Snapshot first = window.snapshot(104);
	if (!require(first.count == 4, "latency window count was incorrect") ||
		!require(first.meanStamps == 25.0, "latency window mean was incorrect") ||
		!require(first.p50Stamps == 20, "latency window P50 was incorrect") ||
		!require(first.p95Stamps == 40 && first.p99Stamps == 40,
			"latency window tail percentile was incorrect") ||
		!require(first.maxStamps == 40, "latency window maximum was incorrect") ||
		!require(!first.p999Available, "P99.9 was published below its sample threshold"))
	{
		return false;
	}

	const KBEngine::ProfileLatencyWindow::Snapshot expired = window.snapshot(201);
	return require(expired.count == 3, "expired latency sample remained in the recent window") &&
		require(expired.p50Stamps == 30, "percentiles were not refreshed after sample expiry");
}

bool testCapacityAndP999Gate()
{
	KBEngine::ProfileLatencyWindow bounded(3, 1000);
	bounded.record(10, 1);
	bounded.record(20, 2);
	bounded.record(30, 3);
	bounded.record(40, 4);
	const KBEngine::ProfileLatencyWindow::Snapshot recent = bounded.snapshot(4);
	if (!require(recent.count == 3, "latency window exceeded its fixed capacity") ||
		!require(recent.p50Stamps == 30 && recent.maxStamps == 40,
			"latency window did not retain the newest samples"))
	{
		return false;
	}

	KBEngine::ProfileLatencyWindow publishable(10000, 1000);
	for (KBEngine::uint64 value = 1; value <= 1000; ++value)
		publishable.record(value, 100);

	const KBEngine::ProfileLatencyWindow::Snapshot full = publishable.snapshot(100);
	return require(full.p999Available, "P99.9 was unavailable at 1000 samples") &&
		require(full.p999Stamps == 999, "P99.9 nearest-rank value was incorrect");
}
}

int main()
{
	if (!testExactPercentilesAndExpiry() || !testCapacityAndP999Gate())
		return EXIT_FAILURE;

	std::cout << "PROFILE_LATENCY_TEST_PASS" << std::endl;
	return EXIT_SUCCESS;
}
