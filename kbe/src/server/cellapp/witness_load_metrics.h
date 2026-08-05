#ifndef KBE_WITNESS_LOAD_METRICS_H
#define KBE_WITNESS_LOAD_METRICS_H

#include "common/performance_probes.h"

#include <cassert>
#include <cstddef>
#include <cstdint>

namespace KBEngine
{

class WitnessProcessingStats
{
public:
	explicit WitnessProcessingStats(std::uint32_t sampleRate = 32) :
		calls_(0),
		sampledCalls_(0),
		sampledTotalNanos_(0),
		sampledMaxNanos_(0),
		slowSamplesOver1ms_(0),
		sampleRate_(sampleRate)
	{
		assert(sampleRate_ > 0);
	}

	bool beginCall()
	{
		if (!g_performanceProbesEnabled)
			return false;

		++calls_;
		// 首次事件始终采样，使低频 Enter/Leave 在短窗口内仍可观测。
		// Always sample the first event so rare Enter/Leave work remains visible in short windows.
		return calls_ == 1 || calls_ % sampleRate_ == 0;
	}

	void recordSample(std::uint64_t durationNanos)
	{
		if (!g_performanceProbesEnabled)
			return;

		++sampledCalls_;
		sampledTotalNanos_ += durationNanos;
		if (durationNanos > sampledMaxNanos_)
			sampledMaxNanos_ = durationNanos;
		if (durationNanos >= 1000000)
			++slowSamplesOver1ms_;
	}

	std::uint64_t calls() const { return calls_; }
	std::uint64_t sampledCalls() const { return sampledCalls_; }
	std::uint64_t sampledTotalNanos() const { return sampledTotalNanos_; }
	std::uint64_t sampledAverageNanos() const
	{
		return sampledCalls_ == 0 ? 0 : sampledTotalNanos_ / sampledCalls_;
	}
	std::uint64_t sampledMaxNanos() const { return sampledMaxNanos_; }
	std::uint64_t slowSamplesOver1ms() const { return slowSamplesOver1ms_; }
	std::uint32_t sampleRate() const { return sampleRate_; }

private:
	std::uint64_t calls_;
	std::uint64_t sampledCalls_;
	std::uint64_t sampledTotalNanos_;
	std::uint64_t sampledMaxNanos_;
	std::uint64_t slowSamplesOver1ms_;
	std::uint32_t sampleRate_;
};

/**
 * CellApp 在组件主线程更新 Witness 并处理 Watcher 查询，因此这里使用无锁累计值，避免给每次 AOI 变化增加原子读改写开销。
 * CellApp updates Witness instances and serves Watcher queries on its component thread, so lock-free plain counters avoid atomic read-modify-write overhead on every AOI change.
 */
class WitnessLoadMetrics
{
public:
	void synchronizeViewCount(std::size_t& trackedCount, std::size_t currentCount)
	{
		if (!g_performanceProbesEnabled)
		{
			// trackedCount 属于 Witness 生命周期状态，即使关闭统计也必须保持同步，避免析构或未来扩展开关时出现错误增量。
			// trackedCount belongs to Witness lifecycle state and must stay synchronized even when metrics are disabled.
			trackedCount = currentCount;
			return;
		}

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
		if (!g_performanceProbesEnabled)
			return;

		++fullScans_;
		fullScanEntities_ += static_cast<std::uint64_t>(scannedEntities);
	}

	void recordDirtyEnqueued(std::size_t queueDepth, bool requeue, bool structural = false, bool promotion = false)
	{
		if (!g_performanceProbesEnabled)
			return;

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
		if (!g_performanceProbesEnabled)
			return;

		assert(dirtyQueued_ >= count);
		dirtyQueued_ -= static_cast<std::uint64_t>(count);
		std::uint64_t& queueDepth = structural ? structuralQueued_ : volatileQueued_;
		assert(queueDepth >= count);
		queueDepth -= static_cast<std::uint64_t>(count);
	}
	void recordQueueDeduplicated() { if (g_performanceProbesEnabled) ++queueDeduplicated_; }
	void recordProducerCoalesced() { if (g_performanceProbesEnabled) ++producerCoalesced_; }
	void recordPromotedVolatileSkip() { if (g_performanceProbesEnabled) ++promotedVolatileSkips_; }
	void recordCancelledPendingLeave() { if (g_performanceProbesEnabled) ++cancelledPendingLeaves_; }

	void recordDirtyProcessed() { if (g_performanceProbesEnabled) ++dirtyProcessed_; }
	void recordStaleDiscard() { if (g_performanceProbesEnabled) ++staleDiscards_; }
	void recordStateSkip() { if (g_performanceProbesEnabled) ++stateSkips_; }
	void recordVolatileBytes(std::uint64_t bytes) { if (g_performanceProbesEnabled) volatileBytesSent_ += bytes; }
	void recordVolatileBudgetDeferred() { if (g_performanceProbesEnabled) ++volatileBudgetDeferred_; }
	void recordVolatileBudgetExhaustion() { if (g_performanceProbesEnabled) ++volatileBudgetExhaustions_; }
	void recordSendBytes(std::uint64_t bytes) { if (g_performanceProbesEnabled) sendBytes_ += bytes; }
	void recordSendBudgetExhaustion() { if (g_performanceProbesEnabled) ++sendBudgetExhaustions_; }
	void recordStructuralProcessed() { if (g_performanceProbesEnabled) ++structuralProcessed_; }
	void recordGlobalAdmission(bool admitted)
	{
		if (!g_performanceProbesEnabled)
			return;
		admitted ? ++globalAdmitted_ : ++globalDeferred_;
	}
	void recordEnter(std::uint64_t bytes) { if (g_performanceProbesEnabled) { ++enterUpdates_; enterBytes_ += bytes; } }
	void recordLeave(std::uint64_t bytes) { if (g_performanceProbesEnabled) { ++leaveUpdates_; leaveBytes_ += bytes; } }
	bool beginEnterProcessing() { return enterProcessing_.beginCall(); }
	bool beginLeaveProcessing() { return leaveProcessing_.beginCall(); }
	void recordEnterProcessing(std::uint64_t durationNanos) { enterProcessing_.recordSample(durationNanos); }
	void recordLeaveProcessing(std::uint64_t durationNanos) { leaveProcessing_.recordSample(durationNanos); }
	void recordVolatileUpdate(std::uint64_t bytes) { if (g_performanceProbesEnabled) { ++volatileUpdates_; volatileUpdateBytes_ += bytes; } }
	void recordVolatileSuppression(bool suppressed)
	{
		if (!g_performanceProbesEnabled)
			return;

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
	void recordSuppressedUpdateSkip() { if (g_performanceProbesEnabled) ++suppressedUpdateSkips_; }
	void recordSuppressedVolatileRefresh() { if (g_performanceProbesEnabled) ++suppressedVolatileRefreshes_; }
	void recordStructuralWhileSuppressed() { if (g_performanceProbesEnabled) ++structuralWhileSuppressed_; }
	void recordBundle(std::size_t bytes)
	{
		if (!g_performanceProbesEnabled)
			return;

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
	std::uint64_t cancelledPendingLeaves() const { return cancelledPendingLeaves_; }
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
	const WitnessProcessingStats& enterProcessing() const { return enterProcessing_; }
	const WitnessProcessingStats& leaveProcessing() const { return leaveProcessing_; }
	std::uint64_t volatileUpdates() const { return volatileUpdates_; }
	std::uint64_t volatileUpdateBytes() const { return volatileUpdateBytes_; }
	std::uint64_t activeSuppressed() const { return activeSuppressed_; }
	std::uint64_t suppressionTransitions() const { return suppressionTransitions_; }
	std::uint64_t resumeTransitions() const { return resumeTransitions_; }
	std::uint64_t suppressedUpdateSkips() const { return suppressedUpdateSkips_; }
	std::uint64_t suppressedVolatileRefreshes() const { return suppressedVolatileRefreshes_; }
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
	std::uint64_t cancelledPendingLeaves_ = 0;
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
	WitnessProcessingStats enterProcessing_;
	WitnessProcessingStats leaveProcessing_;
	std::uint64_t volatileUpdates_ = 0;
	std::uint64_t volatileUpdateBytes_ = 0;
	std::uint64_t activeSuppressed_ = 0;
	std::uint64_t suppressionTransitions_ = 0;
	std::uint64_t resumeTransitions_ = 0;
	std::uint64_t suppressedUpdateSkips_ = 0;
	std::uint64_t suppressedVolatileRefreshes_ = 0;
	std::uint64_t structuralWhileSuppressed_ = 0;
	std::uint64_t bundlesSent_ = 0;
	std::uint64_t maxBundleBytes_ = 0;
};

}

#endif
