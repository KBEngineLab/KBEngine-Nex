// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

#ifndef KBE_KCP_UPDATE_QUEUE_H
#define KBE_KCP_UPDATE_QUEUE_H

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace KBEngine {
namespace Network
{

class KcpUpdateQueue
{
public:
	typedef std::uintptr_t Key;
	typedef std::uint64_t Time;

	KcpUpdateQueue();

	// An earlier request replaces the active deadline; later duplicates keep the existing wakeup.
	// 更早的请求替换当前截止时间；更晚的重复请求保留现有唤醒时间。
	bool schedule(Key key, Time dueTime);
	bool cancel(Key key);
	bool isScheduled(Key key) const;

	// Due entries transfer out one at a time so callbacks may safely reschedule the same key.
	// 到期项逐个移出，使回调能够安全地重新调度同一个 key。
	bool takeDue(Time now, Key& key, Time* pDueTime = NULL);
	bool nextDue(Time& dueTime);
	size_t dueCount(Time now) const;
	size_t overdueCount(Time now) const;

	size_t scheduledCount() const { return scheduledCount_; }
	size_t heapEntryCount() const { return heap_.size(); }
	std::uint64_t scheduleRequestCount() const { return scheduleRequestCount_; }
	std::uint64_t earlierReplacementCount() const { return earlierReplacementCount_; }
	std::uint64_t cancellationCount() const { return cancellationCount_; }
	std::uint64_t staleDiscardCount() const { return staleDiscardCount_; }
	std::uint64_t compactionCount() const { return compactionCount_; }

	template<typename Visitor>
	void forEachScheduledKey(Visitor visitor) const
	{
		// active_ is the authoritative set; heap_ may contain stale replacement entries.
		// active_ 是权威集合；heap_ 可能保留被更早截止时间替换后的陈旧项。
		for (ActiveEntries::const_iterator iter = active_.begin(); iter != active_.end(); ++iter)
		{
			if (iter->second.token != 0)
				visitor(iter->first);
		}
	}

private:
	struct ActiveEntry
	{
		Time dueTime;
		std::uint64_t token;
	};

	struct HeapEntry
	{
		Time dueTime;
		std::uint64_t token;
		Key key;
	};

	struct LaterDue
	{
		bool operator()(const HeapEntry& left, const HeapEntry& right) const;
	};

	typedef std::unordered_map<Key, ActiveEntry> ActiveEntries;

	bool isCurrent(const HeapEntry& entry) const;
	void popHeapTop();
	void pruneStaleTop();
	void compactIfNeeded();
	std::uint64_t nextToken();

	ActiveEntries active_;
	std::vector<HeapEntry> heap_;
	size_t scheduledCount_;
	std::uint64_t nextToken_;
	std::uint64_t scheduleRequestCount_;
	std::uint64_t earlierReplacementCount_;
	std::uint64_t cancellationCount_;
	std::uint64_t staleDiscardCount_;
	std::uint64_t compactionCount_;
};

}
}

#endif // KBE_KCP_UPDATE_QUEUE_H
