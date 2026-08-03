#include "helper/profile_latency.h"

#include <chrono>
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
		require(full.p999Stamps == 999, "P99.9 nearest-rank value was incorrect") &&
		require(publishable.allocatedBytes() >= 240000,
			"latency window did not report its fixed storage cost");
}

bool testRecordCost()
{
	const KBEngine::uint64 iterations = 2000000;
	KBEngine::ProfileLatencyWindow window(10000, iterations + 1);
	const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
	for (KBEngine::uint64 index = 0; index < iterations; ++index)
		window.record(index & 1023U, index);
	const std::chrono::steady_clock::time_point finished = std::chrono::steady_clock::now();
	const double elapsedNanos = static_cast<double>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count());
	const double nanosPerRecord = elapsedNanos / static_cast<double>(iterations);

	std::cout << "PROFILE_LATENCY_RECORD_NS=" << nanosPerRecord << std::endl;
	// This loose regression ceiling catches accidental sorting/allocation in record()
	// without turning normal Debug-build and virtual-machine variance into test flakes.
	// 宽松上限用于捕获 record() 中误入排序/分配的回归，同时容纳 Debug 和虚拟机抖动。
	if (!require(nanosPerRecord < 5000.0, "latency record hot path exceeded 5 microseconds"))
		return false;

	const std::chrono::steady_clock::time_point snapshotStarted = std::chrono::steady_clock::now();
	const KBEngine::ProfileLatencyWindow::Snapshot snapshot = window.snapshot(iterations);
	const std::chrono::steady_clock::time_point snapshotFinished = std::chrono::steady_clock::now();
	const double snapshotMicros = static_cast<double>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(snapshotFinished - snapshotStarted).count()) / 1000.0;
	std::cout << "PROFILE_LATENCY_SNAPSHOT_US=" << snapshotMicros << std::endl;
	return require(snapshot.count == 10000, "latency benchmark snapshot did not retain its full window") &&
		require(snapshotMicros < 10000.0, "latency Watcher snapshot exceeded 10 milliseconds");
}
}

int main()
{
	if (!testExactPercentilesAndExpiry() || !testCapacityAndP999Gate() || !testRecordCost())
		return EXIT_FAILURE;

	std::cout << "PROFILE_LATENCY_TEST_PASS" << std::endl;
	return EXIT_SUCCESS;
}
