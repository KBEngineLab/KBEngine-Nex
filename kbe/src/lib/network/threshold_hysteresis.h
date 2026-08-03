#ifndef KBE_NETWORK_THRESHOLD_HYSTERESIS_H
#define KBE_NETWORK_THRESHOLD_HYSTERESIS_H

#include <algorithm>
#include <cstdint>

namespace KBEngine
{
namespace Network
{

/**
 * 纯判定函数让高频调用不持有共享状态；调用方仍负责对象生命周期和状态迁移副作用。
 * A pure decision function keeps the hot path free of shared state; callers own lifecycle and transition side effects.
 */
class ThresholdHysteresis
{
public:
	static bool next(bool active, std::uint64_t value, std::uint64_t high, std::uint64_t low)
	{
		if (high == 0)
			return false;

		const std::uint64_t boundedLow = std::min(low, high);
		return active ? value > boundedLow : value >= high;
	}
};

}
}

#endif
