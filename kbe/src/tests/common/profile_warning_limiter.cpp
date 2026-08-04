#include "helper/profile_warning_limiter.h"

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

bool testIntervalAggregation()
{
	KBEngine::ProfileWarningLimiter limiter;
	const KBEngine::ProfileWarningLimiter::Decision first = limiter.record(100, 20, 10);
	const KBEngine::ProfileWarningLimiter::Decision suppressed = limiter.record(105, 40, 10);
	const KBEngine::ProfileWarningLimiter::Decision next = limiter.record(110, 30, 10);

	return require(first.shouldLog, "the first slow call was not logged") &&
		require(!suppressed.shouldLog, "a warning inside the interval was not suppressed") &&
		require(next.shouldLog, "the boundary warning was not logged") &&
		require(next.suppressedSinceLastLog == 1,
			"the interval did not report its suppressed warning count") &&
		require(next.intervalMaxDuration == 40,
			"the interval did not preserve its maximum duration") &&
		require(limiter.slowCallCount() == 3, "the slow-call total was incorrect") &&
		require(limiter.warningLogCount() == 2, "the warning-log total was incorrect") &&
		require(limiter.suppressedWarningCount() == 1,
			"the cumulative suppressed-warning total was incorrect") &&
		require(limiter.maxSlowDuration() == 40, "the lifetime maximum was incorrect");
}

bool testZeroIntervalDisablesThrottling()
{
	KBEngine::ProfileWarningLimiter limiter;
	const KBEngine::ProfileWarningLimiter::Decision first = limiter.record(0, 1, 0);
	const KBEngine::ProfileWarningLimiter::Decision second = limiter.record(0, 2, 0);

	return require(first.shouldLog && second.shouldLog,
		"a zero interval did not preserve legacy per-call logging") &&
		require(limiter.warningLogCount() == 2, "zero-interval logs were not counted") &&
		require(limiter.suppressedWarningCount() == 0,
			"zero-interval logging incorrectly suppressed a warning");
}
}

int main()
{
	if (!testIntervalAggregation() || !testZeroIntervalDisablesThrottling())
		return EXIT_FAILURE;

	std::cout << "PROFILE_WARNING_LIMITER_TEST_PASS" << std::endl;
	return EXIT_SUCCESS;
}
