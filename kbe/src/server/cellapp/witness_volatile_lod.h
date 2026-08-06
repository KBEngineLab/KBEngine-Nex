#ifndef KBE_WITNESS_VOLATILE_LOD_H
#define KBE_WITNESS_VOLATILE_LOD_H

#include <cstddef>
#include <cstdint>
#include <limits>

namespace KBEngine
{

struct WitnessVolatileLodConfig
{
	bool enabled;
	std::size_t minimumViewEntities;
	float nearDistance;
	float mediumDistance;
	std::uint32_t mediumIntervalTicks;
	std::uint32_t farIntervalTicks;
};

inline bool witnessVolatileWithinDistance(float configuredDistance, float distanceSquared)
{
	if (configuredDistance <= 0.f)
		return false;

	// FLT_MAX represents ALWAYS. Test it before squaring to avoid overflow in the hot path.
	// FLT_MAX 表示 ALWAYS，必须先判断再平方，避免热路径中的浮点溢出。
	if (configuredDistance == std::numeric_limits<float>::max())
		return true;

	return distanceSquared <= configuredDistance * configuredDistance;
}

inline std::uint32_t witnessVolatileIntervalTicks(
	const WitnessVolatileLodConfig& config,
	std::size_t viewEntityCount,
	float distanceSquared)
{
	if (!config.enabled || viewEntityCount < config.minimumViewEntities)
		return 1;

	const float nearDistanceSquared = config.nearDistance * config.nearDistance;
	if (distanceSquared <= nearDistanceSquared)
		return 1;

	const float mediumDistanceSquared = config.mediumDistance * config.mediumDistance;
	if (distanceSquared <= mediumDistanceSquared)
		return config.mediumIntervalTicks > 1 ? config.mediumIntervalTicks : 1;

	return config.farIntervalTicks > 1 ? config.farIntervalTicks : 1;
}

inline std::uint64_t witnessNextVolatileTick(
	std::uint64_t currentTick,
	std::uint32_t intervalTicks,
	std::uint32_t phaseKey)
{
	if (intervalTicks <= 1)
		return currentTick + 1;

	// Stable phasing spreads dense relations across future ticks without adding randomness or per-tick state.
	// 稳定相位把高密度关系分散到未来 Tick，无需随机数或额外的逐 Tick 状态。
	const std::uint64_t desiredPhase = phaseKey % intervalTicks;
	const std::uint64_t nextTick = currentTick + 1;
	const std::uint64_t phaseDelta = (desiredPhase + intervalTicks - (nextTick % intervalTicks)) % intervalTicks;
	return nextTick + phaseDelta;
}

inline std::uint32_t witnessVolatilePhaseKey(std::uint32_t observerID, std::uint32_t targetID)
{
	// Mix both ends of the AOI relation. Target-only phasing synchronizes every observer of a popular entity.
	// 混合 AOI 关系两端；仅按目标错峰会让热门实体的所有观察者仍在同一 Tick 集中更新。
	std::uint32_t value = targetID ^ (observerID + 0x9e3779b9U + (targetID << 6) + (targetID >> 2));
	value ^= value >> 16;
	value *= 0x7feb352dU;
	value ^= value >> 15;
	return value;
}

}

#endif
