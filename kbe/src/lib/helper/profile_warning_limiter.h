#ifndef KBE_HELPER_PROFILE_WARNING_LIMITER_H
#define KBE_HELPER_PROFILE_WARNING_LIMITER_H

#include <algorithm>
#include <cstdint>

namespace KBEngine
{

/**
 * Profile slow-call accounting with bounded log emission.
 * Profile 慢调用累计器：完整保留诊断计数，同时限制日志输出频率。
 */
class ProfileWarningLimiter
{
public:
	struct Decision
	{
		bool shouldLog;
		std::uint64_t suppressedSinceLastLog;
		std::uint64_t intervalMaxDuration;
	};

	ProfileWarningLimiter() :
		hasLogged_(false),
		lastLogTime_(0),
		intervalMaxDuration_(0),
		suppressedSinceLastLog_(0),
		slowCallCount_(0),
		warningLogCount_(0),
		suppressedWarningCount_(0),
		maxSlowDuration_(0)
	{
	}

	Decision record(std::uint64_t now, std::uint64_t duration,
		std::uint64_t logInterval)
	{
		++slowCallCount_;
		maxSlowDuration_ = std::max(maxSlowDuration_, duration);
		intervalMaxDuration_ = std::max(intervalMaxDuration_, duration);

		const bool shouldLog = !hasLogged_ || logInterval == 0 ||
			now - lastLogTime_ >= logInterval;
		if (!shouldLog)
		{
			++suppressedSinceLastLog_;
			++suppressedWarningCount_;
			return Decision{ false, 0, 0 };
		}

		Decision decision{ true, suppressedSinceLastLog_, intervalMaxDuration_ };
		hasLogged_ = true;
		lastLogTime_ = now;
		intervalMaxDuration_ = 0;
		suppressedSinceLastLog_ = 0;
		++warningLogCount_;
		return decision;
	}

	std::uint64_t slowCallCount() const { return slowCallCount_; }
	std::uint64_t warningLogCount() const { return warningLogCount_; }
	std::uint64_t suppressedWarningCount() const { return suppressedWarningCount_; }
	std::uint64_t maxSlowDuration() const { return maxSlowDuration_; }

private:
	bool hasLogged_;
	std::uint64_t lastLogTime_;
	std::uint64_t intervalMaxDuration_;
	std::uint64_t suppressedSinceLastLog_;
	std::uint64_t slowCallCount_;
	std::uint64_t warningLogCount_;
	std::uint64_t suppressedWarningCount_;
	std::uint64_t maxSlowDuration_;
};

}

#endif // KBE_HELPER_PROFILE_WARNING_LIMITER_H
