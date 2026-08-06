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
		beginTick(activeWitnesses, updateLimit, 0);
	}

	void beginTick(std::uint64_t activeWitnesses, std::uint32_t updateLimit,
		std::uint64_t pendingWitnesses)
	{
		activeWitnesses_ = activeWitnesses;
		observed_ = 0;
		pendingObserved_ = 0;
		admittedThisTick_ = 0;
		nonPendingAdmitted_ = 0;
		const std::uint64_t requested = updateLimit == 0 ? activeWitnesses : updateLimit;
		admissionCount_ = std::min(activeWitnesses, requested);
		start_ = activeWitnesses_ > 0 ? nextStart_ % activeWitnesses_ : 0;
		nextStart_ = activeWitnesses_ > 0 ? (start_ + admissionCount_) % activeWitnesses_ : 0;
		pendingCount_ = std::min(activeWitnesses, pendingWitnesses);
		pendingAdmissionCount_ = std::min(admissionCount_, pendingCount_);
		pendingStart_ = pendingCount_ > 0 ? nextPendingStart_ % pendingCount_ : 0;
		nextPendingStart_ = pendingCount_ > 0
			? (pendingStart_ + pendingAdmissionCount_) % pendingCount_ : 0;
	}

	bool admit()
	{
		return admit(false);
	}

	bool admit(bool hasPendingWork)
	{
		const std::uint64_t ordinal = observed_++;
		if (admissionCount_ >= activeWitnesses_)
			return ordinal < activeWitnesses_;

		if (pendingCount_ > 0)
		{
			if (hasPendingWork)
			{
				const std::uint64_t pendingOrdinal = pendingObserved_++;
				const bool inWindow = pendingAdmissionCount_ >= pendingCount_ ||
					(pendingStart_ < (pendingStart_ + pendingAdmissionCount_) % pendingCount_
						? pendingOrdinal >= pendingStart_ &&
							pendingOrdinal < pendingStart_ + pendingAdmissionCount_
						: pendingOrdinal >= pendingStart_ ||
							pendingOrdinal < (pendingStart_ + pendingAdmissionCount_) % pendingCount_);
				if (!inWindow || admittedThisTick_ >= admissionCount_)
					return false;
				++admittedThisTick_;
				return true;
			}

			const std::uint64_t nonPendingLimit = admissionCount_ - pendingAdmissionCount_;
			if (nonPendingAdmitted_ >= nonPendingLimit)
				return false;

			++nonPendingAdmitted_;
			++admittedThisTick_;
			return true;
		}

		if (admissionCount_ == 0 || ordinal >= activeWitnesses_ ||
			admittedThisTick_ >= admissionCount_)
			return false;

		const std::uint64_t end = (start_ + admissionCount_) % activeWitnesses_;
		const bool inWindow = start_ < end
			? ordinal >= start_ && ordinal < end
			: ordinal >= start_ || ordinal < end;
		if (inWindow)
			++admittedThisTick_;
		return inWindow;
	}

	std::uint64_t admissionCount() const { return admissionCount_; }
	std::uint64_t pendingSnapshot() const { return pendingCount_; }
	std::uint64_t nextStart() const { return nextStart_; }

private:
	std::uint64_t activeWitnesses_ = 0;
	std::uint64_t admissionCount_ = 0;
	std::uint64_t observed_ = 0;
	std::uint64_t start_ = 0;
	std::uint64_t nextStart_ = 0;
	std::uint64_t pendingCount_ = 0;
	std::uint64_t pendingAdmissionCount_ = 0;
	std::uint64_t pendingObserved_ = 0;
	std::uint64_t pendingStart_ = 0;
	std::uint64_t nextPendingStart_ = 0;
	std::uint64_t admittedThisTick_ = 0;
	std::uint64_t nonPendingAdmitted_ = 0;
};

}

#endif
