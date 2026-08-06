#ifndef KBE_WITNESS_DIRTY_QUEUE_H
#define KBE_WITNESS_DIRTY_QUEUE_H

#include <cstdint>
#include <cstddef>
#include <vector>

namespace KBEngine
{

class WitnessDirtyQueue
{
public:
	struct Entry
	{
		std::uint32_t entityID;
		std::uint64_t generation;
	};

	bool enqueue(std::uint32_t entityID, std::uint64_t generation, bool& queued)
	{
		if (queued)
			return false;

		ensureCapacity();
		const std::size_t tail = (head_ + count_) % entries_.size();
		entries_[tail] = Entry{ entityID, generation };
		++count_;
		queued = true;
		return true;
	}

	void enqueueOwned(std::uint32_t entityID, std::uint64_t generation)
	{
		// Ownership is transferred from the delayed queue, so the EntityRef queued flag intentionally stays true.
		// 所有权从延迟队列转移而来，因此 EntityRef 的 queued 标记应继续保持 true。
		ensureCapacity();
		const std::size_t tail = (head_ + count_) % entries_.size();
		entries_[tail] = Entry{ entityID, generation };
		++count_;
	}

	bool pop(Entry& entry)
	{
		if (count_ == 0)
			return false;

		entry = entries_[head_];
		head_ = (head_ + 1) % entries_.size();
		--count_;
		if (count_ == 0)
			head_ = 0;
		return true;
	}

	std::size_t batchSize() const
	{
		// 只消费 Tick 开始时已有的条目，重入队的尾部条目留给下一 Tick，避免停止后补发窗口在同一 Tick 内耗尽。
		// Consume only entries present at tick start, leaving tail requeues for the next tick so the post-movement resend window cannot be exhausted in one tick.
		return count_;
	}

	std::size_t size() const
	{
		return count_;
	}

	void clear()
	{
		head_ = 0;
		count_ = 0;
	}

	void trimEmpty(std::size_t retainedCapacity)
	{
		if (count_ != 0 || entries_.size() <= retainedCapacity)
			return;

		// Full-scan bursts should not pin their peak ring capacity for the entire Witness lifetime.
		// 全量扫描洪峰不应让环形队列在整个 Witness 生命周期内永久保留峰值容量。
		std::vector<Entry> compact;
		compact.resize(retainedCapacity);
		entries_.swap(compact);
		head_ = 0;
	}

private:
	void ensureCapacity()
	{
		if (count_ < entries_.size())
			return;

		// 连续环形存储避免 deque 为每个活跃 Witness 保留分块，同时只在容量翻倍时按 FIFO 搬移一次。
		// Contiguous ring storage avoids retaining deque blocks per active Witness and moves FIFO entries only when capacity doubles.
		const std::size_t newCapacity = entries_.empty() ? 8 : entries_.size() * 2;
		std::vector<Entry> expanded(newCapacity);
		for (std::size_t i = 0; i < count_; ++i)
			expanded[i] = entries_[(head_ + i) % entries_.size()];

		entries_.swap(expanded);
		head_ = 0;
	}

	std::vector<Entry> entries_;
	std::size_t head_ = 0;
	std::size_t count_ = 0;
};

}

#endif
