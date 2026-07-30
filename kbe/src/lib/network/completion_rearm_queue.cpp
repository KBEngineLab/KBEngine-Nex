// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

#include "completion_rearm_queue.h"

namespace KBEngine {
namespace Network
{

CompletionRearmQueue::CompletionRearmQueue() :
	queue_(),
	requests_()
{
}

//-------------------------------------------------------------------------------------
void CompletionRearmQueue::request(KBESOCKET fd, uint8 flags)
{
	flags &= ALL;
	if (flags == NONE)
	{
		return;
	}

	Requests::iterator iter = requests_.find(fd);
	if (iter == requests_.end())
	{
		requests_.insert(std::make_pair(fd, flags));
		queue_.push_back(fd);
		return;
	}

	iter->second |= flags;
}

//-------------------------------------------------------------------------------------
void CompletionRearmQueue::cancel(KBESOCKET fd, uint8 flags)
{
	Requests::iterator iter = requests_.find(fd);
	if (iter == requests_.end())
	{
		return;
	}

	iter->second &= static_cast<uint8>(~flags);
	if (iter->second == NONE)
	{
		// deque 中的陈旧 fd 由 take 跳过，注销路径不做 O(N) 删除。
		// take skips the stale deque entry so deregistration never performs an O(N) erase.
		requests_.erase(iter);
	}
}

//-------------------------------------------------------------------------------------
bool CompletionRearmQueue::take(KBESOCKET& fd, uint8& flags)
{
	while (!queue_.empty())
	{
		fd = queue_.front();
		queue_.pop_front();

		Requests::iterator iter = requests_.find(fd);
		if (iter == requests_.end())
		{
			continue;
		}

		flags = iter->second;
		requests_.erase(iter);
		return true;
	}

	return false;
}

//-------------------------------------------------------------------------------------
size_t CompletionRearmQueue::size() const
{
	return requests_.size();
}

}
}
