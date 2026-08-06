#ifndef KBE_WITNESS_DELAYED_QUEUE_H
#define KBE_WITNESS_DELAYED_QUEUE_H

#include "witness_dirty_queue.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace KBEngine
{

class WitnessDelayedQueue
{
public:
	bool schedule(
		std::uint32_t entityID,
		std::uint64_t generation,
		std::uint64_t currentTick,
		std::uint64_t dueTick,
		std::uint32_t maximumIntervalTicks,
		bool& queued)
	{
		if (queued)
			return false;

		if (buckets_.empty())
		{
			// One extra bucket prevents the maximum delay from colliding with the current tick bucket.
			// 多一个桶可避免最大延迟与当前 Tick 的桶发生碰撞。
			buckets_.resize(static_cast<std::size_t>(maximumIntervalTicks) + 1);
			lastActivatedTick_ = currentTick;
		}

		const std::size_t bucketIndex = static_cast<std::size_t>(dueTick % buckets_.size());
		if (!buckets_[bucketIndex].enqueue(entityID, generation, queued))
			return false;

		++count_;
		return true;
	}

	void activateDue(std::uint64_t currentTick, WitnessDirtyQueue& activeQueue)
	{
		if (buckets_.empty() || currentTick <= lastActivatedTick_)
			return;

		for (std::uint64_t tick = lastActivatedTick_ + 1; tick <= currentTick; ++tick)
		{
			WitnessDirtyQueue& bucket = buckets_[static_cast<std::size_t>(tick % buckets_.size())];
			WitnessDirtyQueue::Entry entry;
			while (bucket.pop(entry))
			{
				activeQueue.enqueueOwned(entry.entityID, entry.generation);
				--count_;
			}
		}

		lastActivatedTick_ = currentTick;
	}

	std::size_t size() const { return count_; }
	void clear()
	{
		for (std::size_t index = 0; index < buckets_.size(); ++index)
			buckets_[index].clear();
		count_ = 0;
		lastActivatedTick_ = 0;
	}

private:
	std::vector<WitnessDirtyQueue> buckets_;
	std::size_t count_ = 0;
	std::uint64_t lastActivatedTick_ = 0;
};

}

#endif
