#ifndef KBE_WITNESS_LOAD_METRICS_H
#define KBE_WITNESS_LOAD_METRICS_H

#include <cassert>
#include <cstddef>
#include <cstdint>

namespace KBEngine
{

/**
 * CellApp 在组件主线程更新 Witness 并处理 Watcher 查询，因此这里使用无锁累计值，避免给每次 AOI 变化增加原子读改写开销。
 * CellApp updates Witness instances and serves Watcher queries on its component thread, so lock-free plain counters avoid atomic read-modify-write overhead on every AOI change.
 */
class WitnessLoadMetrics
{
public:
	void synchronizeViewCount(std::size_t& trackedCount, std::size_t currentCount)
	{
		// 每个 Witness 保存上次已计入的数量，因此增量更新全局值不需要在 Watcher 查询时扫描所有对象。
		// Each Witness retains its last-accounted count, allowing the global value to update incrementally without scanning every object on Watcher reads.
		if (currentCount >= trackedCount)
		{
			viewEntities_ += static_cast<std::uint64_t>(currentCount - trackedCount);
		}
		else
		{
			const std::uint64_t removed = static_cast<std::uint64_t>(trackedCount - currentCount);
			assert(viewEntities_ >= removed);
			viewEntities_ -= removed;
		}

		trackedCount = currentCount;
		if (currentCount > maxViewEntities_)
			maxViewEntities_ = static_cast<std::uint64_t>(currentCount);
	}

	void recordFullScan(std::size_t scannedEntities)
	{
		++fullScans_;
		fullScanEntities_ += static_cast<std::uint64_t>(scannedEntities);
	}

	void recordDirtyEnqueued(std::size_t queueDepth, bool requeue)
	{
		++dirtyQueued_;
		++dirtyEnqueued_;
		if (requeue)
			++dirtyRequeues_;

		if (queueDepth > maxQueueDepth_)
			maxQueueDepth_ = static_cast<std::uint64_t>(queueDepth);
	}

	void recordDirtyDequeued(std::size_t count = 1)
	{
		assert(dirtyQueued_ >= count);
		dirtyQueued_ -= static_cast<std::uint64_t>(count);
	}

	void recordDirtyProcessed() { ++dirtyProcessed_; }
	void recordStaleDiscard() { ++staleDiscards_; }
	void recordStateSkip() { ++stateSkips_; }

	std::uint64_t viewEntities() const { return viewEntities_; }
	std::uint64_t maxViewEntities() const { return maxViewEntities_; }
	std::uint64_t dirtyQueued() const { return dirtyQueued_; }
	std::uint64_t dirtyEnqueued() const { return dirtyEnqueued_; }
	std::uint64_t dirtyRequeues() const { return dirtyRequeues_; }
	std::uint64_t fullScans() const { return fullScans_; }
	std::uint64_t fullScanEntities() const { return fullScanEntities_; }
	std::uint64_t dirtyProcessed() const { return dirtyProcessed_; }
	std::uint64_t maxQueueDepth() const { return maxQueueDepth_; }
	std::uint64_t staleDiscards() const { return staleDiscards_; }
	std::uint64_t stateSkips() const { return stateSkips_; }

private:
	std::uint64_t viewEntities_ = 0;
	std::uint64_t maxViewEntities_ = 0;
	std::uint64_t dirtyQueued_ = 0;
	std::uint64_t dirtyEnqueued_ = 0;
	std::uint64_t dirtyRequeues_ = 0;
	std::uint64_t fullScans_ = 0;
	std::uint64_t fullScanEntities_ = 0;
	std::uint64_t dirtyProcessed_ = 0;
	std::uint64_t maxQueueDepth_ = 0;
	std::uint64_t staleDiscards_ = 0;
	std::uint64_t stateSkips_ = 0;
};

}

#endif
