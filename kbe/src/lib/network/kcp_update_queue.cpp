// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

#include "kcp_update_queue.h"

#include <algorithm>
#include <cassert>

namespace KBEngine {
namespace Network
{
namespace
{
const size_t KCP_UPDATE_QUEUE_COMPACTION_SLACK = 64;
}

KcpUpdateQueue::KcpUpdateQueue() :
	active_(),
	heap_(),
	scheduledCount_(0),
	nextToken_(0),
	scheduleRequestCount_(0),
	earlierReplacementCount_(0),
	cancellationCount_(0),
	staleDiscardCount_(0),
	compactionCount_(0)
{
}

//-------------------------------------------------------------------------------------
bool KcpUpdateQueue::LaterDue::operator()(const HeapEntry& left, const HeapEntry& right) const
{
	if (left.dueTime != right.dueTime)
	{
		return left.dueTime > right.dueTime;
	}
	return left.token > right.token;
}

//-------------------------------------------------------------------------------------
std::uint64_t KcpUpdateQueue::nextToken()
{
	++nextToken_;
	if (nextToken_ == 0)
	{
		++nextToken_;
	}
	return nextToken_;
}

//-------------------------------------------------------------------------------------
bool KcpUpdateQueue::schedule(Key key, Time dueTime)
{
	++scheduleRequestCount_;
	ActiveEntries::iterator existing = active_.find(key);
	if (existing != active_.end() && existing->second.token != 0 && existing->second.dueTime <= dueTime)
	{
		return false;
	}

	if (existing != active_.end() && existing->second.token != 0)
	{
		++earlierReplacementCount_;
	}
	else
	{
		++scheduledCount_;
	}

	const std::uint64_t token = nextToken();
	active_[key] = ActiveEntry{ dueTime, token };
	heap_.push_back(HeapEntry{ dueTime, token, key });
	std::push_heap(heap_.begin(), heap_.end(), LaterDue());
	compactIfNeeded();
	return true;
}

//-------------------------------------------------------------------------------------
bool KcpUpdateQueue::cancel(Key key)
{
	ActiveEntries::iterator iter = active_.find(key);
	if (iter == active_.end())
	{
		return false;
	}

	const bool wasScheduled = iter->second.token != 0;
	active_.erase(iter);
	if (wasScheduled)
	{
		--scheduledCount_;
		++cancellationCount_;
	}
	compactIfNeeded();
	return wasScheduled;
}

//-------------------------------------------------------------------------------------
bool KcpUpdateQueue::isScheduled(Key key) const
{
	ActiveEntries::const_iterator iter = active_.find(key);
	return iter != active_.end() && iter->second.token != 0;
}

//-------------------------------------------------------------------------------------
bool KcpUpdateQueue::isCurrent(const HeapEntry& entry) const
{
	ActiveEntries::const_iterator iter = active_.find(entry.key);
	return iter != active_.end() && iter->second.token == entry.token && iter->second.dueTime == entry.dueTime;
}

//-------------------------------------------------------------------------------------
void KcpUpdateQueue::popHeapTop()
{
	std::pop_heap(heap_.begin(), heap_.end(), LaterDue());
	heap_.pop_back();
}

//-------------------------------------------------------------------------------------
void KcpUpdateQueue::pruneStaleTop()
{
	while (!heap_.empty() && !isCurrent(heap_.front()))
	{
		popHeapTop();
		++staleDiscardCount_;
	}
}

//-------------------------------------------------------------------------------------
bool KcpUpdateQueue::takeDue(Time now, Key& key, Time* pDueTime)
{
	pruneStaleTop();
	if (heap_.empty() || heap_.front().dueTime > now)
	{
		return false;
	}

	const HeapEntry entry = heap_.front();
	popHeapTop();
	ActiveEntries::iterator active = active_.find(entry.key);
	assert(active != active_.end() && active->second.token == entry.token);
	active->second.token = 0;
	--scheduledCount_;
	key = entry.key;
	if (pDueTime != NULL)
		*pDueTime = entry.dueTime;
	return true;
}

//-------------------------------------------------------------------------------------
size_t KcpUpdateQueue::dueCount(Time now) const
{
	size_t count = 0;
	for (ActiveEntries::const_iterator iter = active_.begin(); iter != active_.end(); ++iter)
	{
		if (iter->second.token != 0 && iter->second.dueTime <= now)
			++count;
	}
	return count;
}

//-------------------------------------------------------------------------------------
size_t KcpUpdateQueue::overdueCount(Time now) const
{
	size_t count = 0;
	for (ActiveEntries::const_iterator iter = active_.begin(); iter != active_.end(); ++iter)
	{
		if (iter->second.token != 0 && iter->second.dueTime < now)
			++count;
	}
	return count;
}

//-------------------------------------------------------------------------------------
bool KcpUpdateQueue::nextDue(Time& dueTime)
{
	pruneStaleTop();
	if (heap_.empty())
	{
		return false;
	}

	dueTime = heap_.front().dueTime;
	return true;
}

//-------------------------------------------------------------------------------------
void KcpUpdateQueue::compactIfNeeded()
{
	if (heap_.size() <= scheduledCount_ * 2 + KCP_UPDATE_QUEUE_COMPACTION_SLACK)
	{
		return;
	}

	heap_.clear();
	heap_.reserve(scheduledCount_);
	for (const ActiveEntries::value_type& item : active_)
	{
		if (item.second.token != 0)
		{
			heap_.push_back(HeapEntry{ item.second.dueTime, item.second.token, item.first });
		}
	}
	std::make_heap(heap_.begin(), heap_.end(), LaterDue());
	++compactionCount_;
}

}
}
