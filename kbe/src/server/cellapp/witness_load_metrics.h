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

	void recordDirtyEnqueued(std::size_t queueDepth, bool requeue, bool structural = false, bool promotion = false)
	{
		++dirtyQueued_;
		++dirtyEnqueued_;
		if (structural)
		{
			++structuralQueued_;
			++structuralEnqueued_;
			if (promotion)
				++structuralPromotions_;
		}
		else
		{
			++volatileQueued_;
			++volatileEnqueued_;
		}
		if (requeue)
			++dirtyRequeues_;

		if (queueDepth > maxQueueDepth_)
			maxQueueDepth_ = static_cast<std::uint64_t>(queueDepth);
	}

	void recordDirtyDequeued(std::size_t count = 1, bool structural = false)
	{
		assert(dirtyQueued_ >= count);
		dirtyQueued_ -= static_cast<std::uint64_t>(count);
		std::uint64_t& queueDepth = structural ? structuralQueued_ : volatileQueued_;
		assert(queueDepth >= count);
		queueDepth -= static_cast<std::uint64_t>(count);
	}
	void recordQueueDeduplicated() { ++queueDeduplicated_; }
	void recordProducerCoalesced() { ++producerCoalesced_; }
	void recordPromotedVolatileSkip() { ++promotedVolatileSkips_; }

	void recordDirtyProcessed() { ++dirtyProcessed_; }
	void recordStaleDiscard() { ++staleDiscards_; }
	void recordStateSkip() { ++stateSkips_; }
	void recordVolatileBytes(std::uint64_t bytes) { volatileBytesSent_ += bytes; }
	void recordVolatileBudgetDeferred() { ++volatileBudgetDeferred_; }
	void recordVolatileBudgetExhaustion() { ++volatileBudgetExhaustions_; }
	void recordSendBytes(std::uint64_t bytes) { sendBytes_ += bytes; }
	void recordSendBudgetExhaustion() { ++sendBudgetExhaustions_; }
	void recordStructuralProcessed() { ++structuralProcessed_; }
	void recordGlobalAdmission(bool admitted)
	{
		admitted ? ++globalAdmitted_ : ++globalDeferred_;
	}
	void recordEnter(std::uint64_t bytes) { ++enterUpdates_; enterBytes_ += bytes; }
	void recordLeave(std::uint64_t bytes) { ++leaveUpdates_; leaveBytes_ += bytes; }
	void recordVolatileUpdate(std::uint64_t bytes) { ++volatileUpdates_; volatileUpdateBytes_ += bytes; }
	void recordVolatileSuppression(bool suppressed)
	{
		if (suppressed)
		{
			++activeSuppressed_;
			++suppressionTransitions_;
		}
		else
		{
			assert(activeSuppressed_ > 0);
			--activeSuppressed_;
			++resumeTransitions_;
		}
	}
	void recordSuppressedUpdateSkip() { ++suppressedUpdateSkips_; }
	void recordStructuralWhileSuppressed() { ++structuralWhileSuppressed_; }
	void recordBundle(std::size_t bytes)
	{
		++bundlesSent_;
		if (bytes > maxBundleBytes_)
			maxBundleBytes_ = static_cast<std::uint64_t>(bytes);
	}

	std::uint64_t viewEntities() const { return viewEntities_; }
	std::uint64_t maxViewEntities() const { return maxViewEntities_; }
	std::uint64_t dirtyQueued() const { return dirtyQueued_; }
	std::uint64_t dirtyEnqueued() const { return dirtyEnqueued_; }
	std::uint64_t dirtyRequeues() const { return dirtyRequeues_; }
	std::uint64_t structuralQueued() const { return structuralQueued_; }
	std::uint64_t volatileQueued() const { return volatileQueued_; }
	std::uint64_t structuralEnqueued() const { return structuralEnqueued_; }
	std::uint64_t volatileEnqueued() const { return volatileEnqueued_; }
	std::uint64_t queueDeduplicated() const { return queueDeduplicated_; }
	std::uint64_t producerCoalesced() const { return producerCoalesced_; }
	std::uint64_t structuralPromotions() const { return structuralPromotions_; }
	std::uint64_t promotedVolatileSkips() const { return promotedVolatileSkips_; }
	std::uint64_t fullScans() const { return fullScans_; }
	std::uint64_t fullScanEntities() const { return fullScanEntities_; }
	std::uint64_t dirtyProcessed() const { return dirtyProcessed_; }
	std::uint64_t maxQueueDepth() const { return maxQueueDepth_; }
	std::uint64_t staleDiscards() const { return staleDiscards_; }
	std::uint64_t stateSkips() const { return stateSkips_; }
	std::uint64_t volatileBytesSent() const { return volatileBytesSent_; }
	std::uint64_t volatileBudgetDeferred() const { return volatileBudgetDeferred_; }
	std::uint64_t volatileBudgetExhaustions() const { return volatileBudgetExhaustions_; }
	std::uint64_t sendBytes() const { return sendBytes_; }
	std::uint64_t sendBudgetExhaustions() const { return sendBudgetExhaustions_; }
	std::uint64_t structuralProcessed() const { return structuralProcessed_; }
	std::uint64_t globalAdmitted() const { return globalAdmitted_; }
	std::uint64_t globalDeferred() const { return globalDeferred_; }
	std::uint64_t enterUpdates() const { return enterUpdates_; }
	std::uint64_t enterBytes() const { return enterBytes_; }
	std::uint64_t leaveUpdates() const { return leaveUpdates_; }
	std::uint64_t leaveBytes() const { return leaveBytes_; }
	std::uint64_t volatileUpdates() const { return volatileUpdates_; }
	std::uint64_t volatileUpdateBytes() const { return volatileUpdateBytes_; }
	std::uint64_t activeSuppressed() const { return activeSuppressed_; }
	std::uint64_t suppressionTransitions() const { return suppressionTransitions_; }
	std::uint64_t resumeTransitions() const { return resumeTransitions_; }
	std::uint64_t suppressedUpdateSkips() const { return suppressedUpdateSkips_; }
	std::uint64_t structuralWhileSuppressed() const { return structuralWhileSuppressed_; }
	std::uint64_t bundlesSent() const { return bundlesSent_; }
	std::uint64_t maxBundleBytes() const { return maxBundleBytes_; }

private:
	std::uint64_t viewEntities_ = 0;
	std::uint64_t maxViewEntities_ = 0;
	std::uint64_t dirtyQueued_ = 0;
	std::uint64_t dirtyEnqueued_ = 0;
	std::uint64_t dirtyRequeues_ = 0;
	std::uint64_t structuralQueued_ = 0;
	std::uint64_t volatileQueued_ = 0;
	std::uint64_t structuralEnqueued_ = 0;
	std::uint64_t volatileEnqueued_ = 0;
	std::uint64_t queueDeduplicated_ = 0;
	std::uint64_t producerCoalesced_ = 0;
	std::uint64_t structuralPromotions_ = 0;
	std::uint64_t promotedVolatileSkips_ = 0;
	std::uint64_t fullScans_ = 0;
	std::uint64_t fullScanEntities_ = 0;
	std::uint64_t dirtyProcessed_ = 0;
	std::uint64_t maxQueueDepth_ = 0;
	std::uint64_t staleDiscards_ = 0;
	std::uint64_t stateSkips_ = 0;
	std::uint64_t volatileBytesSent_ = 0;
	std::uint64_t volatileBudgetDeferred_ = 0;
	std::uint64_t volatileBudgetExhaustions_ = 0;
	std::uint64_t sendBytes_ = 0;
	std::uint64_t sendBudgetExhaustions_ = 0;
	std::uint64_t structuralProcessed_ = 0;
	std::uint64_t globalAdmitted_ = 0;
	std::uint64_t globalDeferred_ = 0;
	std::uint64_t enterUpdates_ = 0;
	std::uint64_t enterBytes_ = 0;
	std::uint64_t leaveUpdates_ = 0;
	std::uint64_t leaveBytes_ = 0;
	std::uint64_t volatileUpdates_ = 0;
	std::uint64_t volatileUpdateBytes_ = 0;
	std::uint64_t activeSuppressed_ = 0;
	std::uint64_t suppressionTransitions_ = 0;
	std::uint64_t resumeTransitions_ = 0;
	std::uint64_t suppressedUpdateSkips_ = 0;
	std::uint64_t structuralWhileSuppressed_ = 0;
	std::uint64_t bundlesSent_ = 0;
	std::uint64_t maxBundleBytes_ = 0;
};

}

#endif
