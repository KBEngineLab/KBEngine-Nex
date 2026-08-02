// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

#include "completion_tcp_send_queue.h"

#include <algorithm>
#include <utility>

namespace KBEngine {
namespace Network
{

CompletionTcpSendBuffer::CompletionTcpSendBuffer() :
	storage_(),
	offset_(0)
{
}

char* CompletionTcpSendBuffer::data()
{
	return empty() ? NULL : storage_.data() + offset_;
}

const char* CompletionTcpSendBuffer::data() const
{
	return empty() ? NULL : storage_.data() + offset_;
}

size_t CompletionTcpSendBuffer::size() const
{
	return offset_ <= storage_.size() ? storage_.size() - offset_ : 0;
}

size_t CompletionTcpSendBuffer::capacity() const
{
	return storage_.capacity();
}

bool CompletionTcpSendBuffer::empty() const
{
	return size() == 0;
}

//-------------------------------------------------------------------------------------
void CompletionTcpSendBuffer::reset(size_t maxRetainedCapacity)
{
	storage_.clear();
	offset_ = 0;
	if (storage_.capacity() > maxRetainedCapacity)
	{
		std::vector<char>().swap(storage_);
	}
}

CompletionTcpSendQueue::CompletionTcpSendQueue() :
	buffers_(),
	pendingBytes_(0)
{
}

bool CompletionTcpSendQueue::push(const void* data, size_t length, size_t maxPendingBytes)
{
	return pushResult(data, length, maxPendingBytes) == PUSH_ACCEPTED;
}

CompletionTcpSendQueue::PushResult CompletionTcpSendQueue::pushResult(
	const void* data, size_t length, size_t maxPendingBytes)
{
	if (length == 0)
	{
		return PUSH_ACCEPTED;
	}

	if (data == NULL)
	{
		return PUSH_INVALID;
	}

	// 单块超过队列硬上限时，等待 drain 永远不会让它变得可入队，必须与瞬时背压分开报告。
	// A chunk above the hard queue limit can never fit after draining, so report it separately from transient backpressure.
	if (length > maxPendingBytes)
	{
		return PUSH_OVERSIZED;
	}

	if (pendingBytes_ > maxPendingBytes - length)
	{
		return PUSH_BACKPRESSURED;
	}

	CompletionTcpSendBuffer buffer;
	const char* first = static_cast<const char*>(data);
	buffer.storage_.assign(first, first + length);
	buffers_.push_back(std::move(buffer));
	pendingBytes_ += length;
	return PUSH_ACCEPTED;
}

bool CompletionTcpSendQueue::popBatch(size_t maxBytes, CompletionTcpSendBuffer& batch, bool& copied)
{
	batch = CompletionTcpSendBuffer();
	copied = false;
	if (maxBytes == 0 || buffers_.empty())
	{
		return false;
	}

	CompletionTcpSendBuffer& front = buffers_.front();
	if (front.size() <= maxBytes && (buffers_.size() == 1 || front.size() == maxBytes))
	{
		// 单个待发块直接把 vector 所有权交给 completion context，避免常见单包路径再次复制。
		// Transfer vector ownership directly to the completion context for a single chunk, avoiding another copy on the common one-packet path.
		const size_t transferredBytes = front.size();
		batch = std::move(front);
		buffers_.pop_front();
		pendingBytes_ -= transferredBytes;
		return true;
	}

	const size_t targetBytes = std::min(maxBytes, pendingBytes_);
	batch.storage_.reserve(targetBytes);
	copied = true;
	while (!buffers_.empty() && batch.storage_.size() < targetBytes)
	{
		CompletionTcpSendBuffer& source = buffers_.front();
		const size_t bytesToCopy = std::min(source.size(), targetBytes - batch.storage_.size());
		batch.storage_.insert(batch.storage_.end(), source.data(), source.data() + bytesToCopy);
		source.offset_ += bytesToCopy;
		pendingBytes_ -= bytesToCopy;

		if (source.empty())
		{
			buffers_.pop_front();
		}
	}

	return !batch.empty();
}

bool CompletionTcpSendQueue::restore(CompletionTcpSendBuffer& buffer, size_t consumedBytes)
{
	if (consumedBytes > buffer.size())
	{
		return false;
	}

	buffer.offset_ += consumedBytes;
	if (buffer.empty())
	{
		buffer = CompletionTcpSendBuffer();
		return true;
	}

	const size_t remainingBytes = buffer.size();
	buffers_.push_front(std::move(buffer));
	pendingBytes_ += remainingBytes;
	return true;
}

bool CompletionTcpSendQueue::consumeFront(size_t consumedBytes)
{
	if (buffers_.empty() || consumedBytes > buffers_.front().size())
	{
		return false;
	}

	CompletionTcpSendBuffer& front = buffers_.front();
	front.offset_ += consumedBytes;
	pendingBytes_ -= consumedBytes;
	if (front.empty())
	{
		buffers_.pop_front();
	}

	return true;
}

char* CompletionTcpSendQueue::frontData()
{
	return buffers_.empty() ? NULL : buffers_.front().data();
}

const char* CompletionTcpSendQueue::frontData() const
{
	return buffers_.empty() ? NULL : buffers_.front().data();
}

size_t CompletionTcpSendQueue::frontSize() const
{
	return buffers_.empty() ? 0 : buffers_.front().size();
}

bool CompletionTcpSendQueue::empty() const
{
	return buffers_.empty();
}

size_t CompletionTcpSendQueue::pendingBytes() const
{
	return pendingBytes_;
}

void CompletionTcpSendQueue::clear()
{
	buffers_.clear();
	pendingBytes_ = 0;
}

}
}
