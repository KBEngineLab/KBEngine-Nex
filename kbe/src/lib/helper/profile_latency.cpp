/*
This source file is part of KBEngine
For the latest info, see http://www.kbengine.org/
*/

#include "profile_latency.h"

#include <algorithm>
#include <limits>

namespace KBEngine
{

namespace
{
uint64 saturatedAdd(uint64 left, uint64 right)
{
	return left > std::numeric_limits<uint64>::max() - right ?
		std::numeric_limits<uint64>::max() : left + right;
}
}

ProfileLatencyWindow::Snapshot::Snapshot() :
	count(0),
	meanStamps(0.0),
	p50Stamps(0),
	p95Stamps(0),
	p99Stamps(0),
	p999Stamps(0),
	maxStamps(0),
	p999Available(false)
{
}

ProfileLatencyWindow::ProfileLatencyWindow(size_t capacity, uint64 maxAgeStamps) :
	samples_(std::max<size_t>(capacity, 1)),
	scratch_(),
	nextIndex_(0),
	retainedCount_(0),
	maxAgeStamps_(std::max<uint64>(maxAgeStamps, 1)),
	revision_(0),
	cachedRevision_(std::numeric_limits<uint64>::max()),
	cachedValidUntil_(0),
	cachedSnapshot_()
{
	// Reserve once so Watcher snapshots also avoid allocator noise.
	// 一次性预留快照空间，避免 Watcher 查询产生分配器噪声。
	scratch_.reserve(samples_.size());
}

void ProfileLatencyWindow::record(uint64 durationStamps, uint64 completedAtStamps)
{
	Sample& sample = samples_[nextIndex_];
	sample.durationStamps = durationStamps;
	sample.completedAtStamps = completedAtStamps;
	nextIndex_ = (nextIndex_ + 1) % samples_.size();
	retainedCount_ = std::min(retainedCount_ + 1, samples_.size());
	++revision_;
}

const ProfileLatencyWindow::Snapshot& ProfileLatencyWindow::snapshot(uint64 nowStamps)
{
	if (cachedRevision_ != revision_ || nowStamps > cachedValidUntil_)
		refreshSnapshot(nowStamps);

	return cachedSnapshot_;
}

uint64 ProfileLatencyWindow::allocatedBytes() const
{
	return static_cast<uint64>(samples_.capacity() * sizeof(Sample) +
		scratch_.capacity() * sizeof(uint64));
}

uint64 ProfileLatencyWindow::percentile(
	const std::vector<uint64>& sorted, uint64 numerator, uint64 denominator)
{
	if (sorted.empty())
		return 0;

	// Nearest-rank keeps every published percentile on an observed sample.
	// 最近秩算法保证每个发布分位数都来自真实观测样本。
	const uint64 rank = (static_cast<uint64>(sorted.size()) * numerator + denominator - 1) / denominator;
	return sorted[static_cast<size_t>(std::max<uint64>(rank, 1) - 1)];
}

void ProfileLatencyWindow::refreshSnapshot(uint64 nowStamps)
{
	scratch_.clear();
	uint64 earliestCompletion = std::numeric_limits<uint64>::max();
	double sumStamps = 0.0;

	for (size_t index = 0; index < retainedCount_; ++index)
	{
		const Sample& sample = samples_[index];
		if (sample.completedAtStamps > nowStamps ||
			nowStamps - sample.completedAtStamps > maxAgeStamps_)
		{
			continue;
		}

		scratch_.push_back(sample.durationStamps);
		sumStamps += static_cast<double>(sample.durationStamps);
		earliestCompletion = std::min(earliestCompletion, sample.completedAtStamps);
	}

	std::sort(scratch_.begin(), scratch_.end());
	cachedSnapshot_ = Snapshot();
	cachedSnapshot_.count = static_cast<uint64>(scratch_.size());
	if (!scratch_.empty())
	{
		cachedSnapshot_.meanStamps = sumStamps / static_cast<double>(scratch_.size());
		cachedSnapshot_.p50Stamps = percentile(scratch_, 50, 100);
		cachedSnapshot_.p95Stamps = percentile(scratch_, 95, 100);
		cachedSnapshot_.p99Stamps = percentile(scratch_, 99, 100);
		cachedSnapshot_.maxStamps = scratch_.back();
		cachedSnapshot_.p999Available = scratch_.size() >= P999_MIN_SAMPLES;
		if (cachedSnapshot_.p999Available)
			cachedSnapshot_.p999Stamps = percentile(scratch_, 999, 1000);
	}

	cachedRevision_ = revision_;
	const uint64 naturalExpiry = earliestCompletion == std::numeric_limits<uint64>::max() ?
		saturatedAdd(nowStamps, maxAgeStamps_) : saturatedAdd(earliestCompletion, maxAgeStamps_);
	// Keep one serialized Watcher response coherent if its fields straddle an expiry boundary.
	// 为一次 Watcher 序列化保留极短快照租约，避免各字段跨过过期边界后口径不一致。
	const uint64 queryLease = std::max<uint64>(maxAgeStamps_ / 100, 1);
	cachedValidUntil_ = std::max(naturalExpiry, saturatedAdd(nowStamps, queryLease));
}

}
