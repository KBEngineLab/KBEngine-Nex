#ifndef KBE_WITNESS_UPDATE_SCHEDULER_H
#define KBE_WITNESS_UPDATE_SCHEDULER_H

#include <algorithm>
#include <cstdint>

namespace KBEngine
{

/**
 * CellApp 每 Tick 的 Witness 准入窗口。窗口按累计游标轮转，因此限流时不会永久偏向注册顺序靠前的客户端。
 * Per-CellApp Witness admission window. Its persistent cursor rotates the window so throttling never favors early registrations forever.
 */
class WitnessUpdateScheduler
{
public:
	void beginTick(std::uint64_t activeWitnesses, std::uint32_t updateLimit)
	{
		activeWitnesses_ = activeWitnesses;
		observed_ = 0;
		const std::uint64_t requested = updateLimit == 0 ? activeWitnesses : updateLimit;
		admissionCount_ = std::min(activeWitnesses, requested);
		start_ = activeWitnesses_ > 0 ? nextStart_ % activeWitnesses_ : 0;
		nextStart_ = activeWitnesses_ > 0 ? (start_ + admissionCount_) % activeWitnesses_ : 0;
	}

	bool admit()
	{
		const std::uint64_t ordinal = observed_++;
		if (admissionCount_ >= activeWitnesses_)
			return ordinal < activeWitnesses_;
		if (admissionCount_ == 0 || ordinal >= activeWitnesses_)
			return false;

		const std::uint64_t end = (start_ + admissionCount_) % activeWitnesses_;
		if (start_ < end)
			return ordinal >= start_ && ordinal < end;

		return ordinal >= start_ || ordinal < end;
	}

	std::uint64_t admissionCount() const { return admissionCount_; }
	std::uint64_t nextStart() const { return nextStart_; }

private:
	std::uint64_t activeWitnesses_ = 0;
	std::uint64_t admissionCount_ = 0;
	std::uint64_t observed_ = 0;
	std::uint64_t start_ = 0;
	std::uint64_t nextStart_ = 0;
};

}

#endif
