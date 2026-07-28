#pragma once

#include "KBECommon.h"

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace KBEngine
{

// 单生产者接收线程与单消费者主线程共享固定容量环形缓冲，容量只在连接重置且工作线程停止后调整。
// A single receiver producer and game-thread consumer share this fixed-capacity ring; capacity changes only after the worker stops during connection reset.
class TcpReceiveQueue
{
public:
	explicit TcpReceiveQueue(std::size_t capacity):
		buffer_(capacity),
		readPosition_(0),
		writePosition_(0),
		count_(0),
		stopped_(false)
	{
		if (capacity == 0)
		{
			throw std::invalid_argument("TcpReceiveQueue capacity must be positive");
		}
	}

	TcpReceiveQueue(const TcpReceiveQueue&) = delete;
	TcpReceiveQueue& operator=(const TcpReceiveQueue&) = delete;

	void reset(std::size_t capacity)
	{
		if (capacity == 0)
		{
			throw std::invalid_argument("TcpReceiveQueue capacity must be positive");
		}

		// 调用方必须先停止并 join 旧接收线程；重新启用同一对象可避免重登录期间替换同步原语。
		// The caller must stop and join the old receiver first; rearming the same object avoids replacing synchronization primitives during relogin.
		std::lock_guard<std::mutex> lock(mutex_);
		buffer_.assign(capacity, 0);
		readPosition_ = 0;
		writePosition_ = 0;
		count_ = 0;
		stopped_ = false;
	}

	std::size_t capacity() const
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return buffer_.size();
	}

	bool write(const uint8* source, std::size_t count)
	{
		if (!source && count > 0)
		{
			throw std::invalid_argument("TcpReceiveQueue source must not be null");
		}

		std::unique_lock<std::mutex> lock(mutex_);
		if (count > buffer_.size())
		{
			throw std::length_error("TcpReceiveQueue write exceeds capacity");
		}

		// 队列满时暂停接收线程，让 TCP 内核窗口向服务端传播背压；消费或停止会精确唤醒，不需要轮询。
		// Pause the receiver while full so the TCP kernel window propagates backpressure; consumption or shutdown wakes it without polling.
		spaceAvailable_.wait(lock, [this, count]()
		{
			return stopped_ || buffer_.size() - count_ >= count;
		});

		if (stopped_)
		{
			return false;
		}

		const std::size_t first = std::min(count, buffer_.size() - writePosition_);
		std::copy_n(source, first, buffer_.begin() + writePosition_);
		const std::size_t second = count - first;
		if (second > 0)
		{
			std::copy_n(source + first, second, buffer_.begin());
		}

		writePosition_ = (writePosition_ + count) % buffer_.size();
		count_ += count;
		return true;
	}

	std::size_t drain(std::vector<uint8>& destination)
	{
		std::unique_lock<std::mutex> lock(mutex_);
		destination.resize(count_);
		if (count_ == 0)
		{
			return 0;
		}

		const std::size_t drained = count_;
		const std::size_t first = std::min(drained, buffer_.size() - readPosition_);
		std::copy_n(buffer_.begin() + readPosition_, first, destination.begin());
		const std::size_t second = drained - first;
		if (second > 0)
		{
			std::copy_n(buffer_.begin(), second, destination.begin() + first);
		}

		readPosition_ = (readPosition_ + drained) % buffer_.size();
		count_ = 0;
		lock.unlock();
		spaceAvailable_.notify_all();
		return drained;
	}

	void stop()
	{
		std::unique_lock<std::mutex> lock(mutex_);
		stopped_ = true;
		readPosition_ = 0;
		writePosition_ = 0;
		count_ = 0;
		lock.unlock();
		spaceAvailable_.notify_all();
	}

private:
	mutable std::mutex mutex_;
	std::condition_variable spaceAvailable_;
	std::vector<uint8> buffer_;
	std::size_t readPosition_;
	std::size_t writePosition_;
	std::size_t count_;
	bool stopped_;
};

}
