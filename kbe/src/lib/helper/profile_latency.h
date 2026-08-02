/*
This source file is part of KBEngine
For the latest info, see http://www.kbengine.org/
*/

#ifndef KBENGINE_PROFILE_LATENCY_H
#define KBENGINE_PROFILE_LATENCY_H

#include "common/common.h"

#include <vector>

namespace KBEngine
{

/**
 * A fixed-capacity recent latency window owned by one component event thread.
 * 由单个组件事件线程持有的固定容量近期延迟窗口。
 *
 * Storage is allocated by the constructor. record() never allocates or locks; the
 * more expensive exact percentile calculation only runs when Watcher reads a snapshot.
 * 存储只在构造时分配；record() 不分配也不加锁，精确分位数只在 Watcher 读取快照时计算。
 */
class ProfileLatencyWindow
{
public:
	static const size_t DEFAULT_CAPACITY = 10000;
	static const uint64 P999_MIN_SAMPLES = 10000;

	struct Snapshot
	{
		Snapshot();

		uint64 count;
		double meanStamps;
		uint64 p50Stamps;
		uint64 p95Stamps;
		uint64 p99Stamps;
		uint64 p999Stamps;
		uint64 maxStamps;
		bool p999Available;
	};

	ProfileLatencyWindow(size_t capacity, uint64 maxAgeStamps);

	void record(uint64 durationStamps, uint64 completedAtStamps);
	const Snapshot& snapshot(uint64 nowStamps);

	uint64 capacity() const { return static_cast<uint64>(samples_.size()); }
	uint64 maxAgeStamps() const { return maxAgeStamps_; }
	uint64 allocatedBytes() const;

private:
	struct Sample
	{
		Sample() : durationStamps(0), completedAtStamps(0) {}

		uint64 durationStamps;
		uint64 completedAtStamps;
	};

	static uint64 percentile(const std::vector<uint64>& sorted, uint64 numerator, uint64 denominator);
	void refreshSnapshot(uint64 nowStamps);

	std::vector<Sample> samples_;
	std::vector<uint64> scratch_;
	size_t nextIndex_;
	size_t retainedCount_;
	uint64 maxAgeStamps_;
	uint64 revision_;
	uint64 cachedRevision_;
	uint64 cachedValidUntil_;
	Snapshot cachedSnapshot_;
};

}

#endif // KBENGINE_PROFILE_LATENCY_H
