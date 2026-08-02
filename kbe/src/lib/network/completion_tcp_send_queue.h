// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

#ifndef KBE_COMPLETION_TCP_SEND_QUEUE_H
#define KBE_COMPLETION_TCP_SEND_QUEUE_H

#include <cstddef>
#include <deque>
#include <vector>

namespace KBEngine {
namespace Network
{

class CompletionTcpSendBuffer
{
public:
	CompletionTcpSendBuffer();

	char* data();
	const char* data() const;
	size_t size() const;
	size_t capacity() const;
	bool empty() const;
	void reset(size_t maxRetainedCapacity);

private:
	friend class CompletionTcpSendQueue;

	std::vector<char> storage_;
	size_t offset_;
};

class CompletionTcpSendQueue
{
public:
	enum PushResult
	{
		PUSH_ACCEPTED = 0,
		PUSH_BACKPRESSURED,
		PUSH_OVERSIZED,
		PUSH_INVALID
	};

	CompletionTcpSendQueue();

	bool push(const void* data, size_t length, size_t maxPendingBytes);
	PushResult pushResult(const void* data, size_t length, size_t maxPendingBytes);
	bool popBatch(size_t maxBytes, CompletionTcpSendBuffer& batch, bool& copied);
	bool restore(CompletionTcpSendBuffer& buffer, size_t consumedBytes);
	bool consumeFront(size_t consumedBytes);

	char* frontData();
	const char* frontData() const;
	size_t frontSize() const;
	bool empty() const;
	size_t pendingBytes() const;
	void clear();

private:
	typedef std::deque<CompletionTcpSendBuffer> Buffers;

	Buffers buffers_;
	size_t pendingBytes_;
};

}
}

#endif // KBE_COMPLETION_TCP_SEND_QUEUE_H
