// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

#ifndef KBE_COMPLETION_REARM_QUEUE_H
#define KBE_COMPLETION_REARM_QUEUE_H

#include "common/common.h"
#include "network/common.h"

#include <deque>
#include <map>

namespace KBEngine {
namespace Network
{

// completion 投递失败通常只影响少量 fd；独立队列让后端按失败集合工作，而不是扫描全部连接。
// Completion submission failures normally affect few descriptors; this queue lets backends process that failure set instead of scanning every connection.
class CompletionRearmQueue
{
public:
	enum Flags
	{
		NONE = 0,
		READ = 1,
		WRITE = 2,
		ALL = READ | WRITE
	};

	CompletionRearmQueue();

	// 同一 fd 的方向位合并，队列中始终最多保留一个有效请求。
	// Merge direction bits for one fd so the queue contains at most one live request per descriptor.
	void request(KBESOCKET fd, uint8 flags);
	// 取消操作只更新索引；deque 陈旧项在 take 时跳过，保证注销不退化为 O(N)。
	// Cancellation updates only the index; take skips stale deque entries so deregistration never degrades to O(N).
	void cancel(KBESOCKET fd, uint8 flags = ALL);
	// 取出后调用方可把失败项重新入队尾，从而在资源紧张时保持轮转公平性。
	// After taking an item, the caller can append a failed request to preserve rotation fairness under resource pressure.
	bool take(KBESOCKET& fd, uint8& flags);
	size_t size() const;

private:
	typedef std::deque<KBESOCKET> Queue;
	typedef std::map<KBESOCKET, uint8> Requests;
	Queue queue_;
	Requests requests_;
};

}
}

#endif // KBE_COMPLETION_REARM_QUEUE_H
