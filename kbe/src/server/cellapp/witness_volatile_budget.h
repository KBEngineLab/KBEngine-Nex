#ifndef KBE_WITNESS_VOLATILE_BUDGET_H
#define KBE_WITNESS_VOLATILE_BUDGET_H

#include <cstddef>
#include <cstdint>

namespace KBEngine
{

/**
 * 每个 Witness 每 Tick 的易变数据预算只约束位置和方向更新，结构消息由调用方无条件放行。
 * The per-Witness, per-tick volatile budget limits only position and direction updates; callers always admit structural messages.
 */
class WitnessVolatileBudget
{
public:
	explicit WitnessVolatileBudget(std::uint32_t byteLimit) : byteLimit_(byteLimit) {}

	bool canSend(bool structuralUpdate) const
	{
		if (structuralUpdate)
			return true;

		// 零值用于显式关闭预算，便于兼容旧配置并进行无预算基线对比。
		// Zero explicitly disables the budget for backward compatibility and unthrottled baseline comparisons.
		return byteLimit_ == 0 || bytesSent_ < byteLimit_;
	}

	void recordBundleGrowth(std::size_t beforeBytes, std::size_t afterBytes)
	{
		// Bundle 可能因消息撤销而缩短；只累计正向增长可避免无符号下溢污染长期指标。
		// A Bundle may shrink when a message is revoked; counting only positive growth prevents unsigned underflow from corrupting long-lived metrics.
		if (afterBytes > beforeBytes)
			bytesSent_ += static_cast<std::uint64_t>(afterBytes - beforeBytes);
	}

	std::uint64_t bytesSent() const { return bytesSent_; }

private:
	std::uint32_t byteLimit_;
	std::uint64_t bytesSent_ = 0;
};

}

#endif
