// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

#ifndef KBE_COMPLETION_CONTEXT_POOL_H
#define KBE_COMPLETION_CONTEXT_POOL_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace KBEngine {
namespace Network
{

// Completion context storage stays constructed while cached so platform reset code can retain useful buffer capacity.
// completion context 在缓存期间保持构造状态，使平台 reset 逻辑能够保留有价值的缓冲容量。
template<typename Context>
class CompletionContextPool
{
public:
	explicit CompletionContextPool(size_t cacheLimit) :
		cacheLimit_(cacheLimit),
		cached_(),
		allocationCount_(0),
		reuseCount_(0),
		outstandingCount_(0),
		peakOutstandingCount_(0)
	{
		cached_.reserve(cacheLimit_);
	}

	~CompletionContextPool()
	{
		for (Context* context : cached_)
		{
			delete context;
		}
	}

	CompletionContextPool(const CompletionContextPool&) = delete;
	CompletionContextPool& operator=(const CompletionContextPool&) = delete;

	Context* acquire()
	{
		Context* context = NULL;
		if (cached_.empty())
		{
			context = new Context();
			++allocationCount_;
		}
		else
		{
			context = cached_.back();
			cached_.pop_back();
			++reuseCount_;
		}

		++outstandingCount_;
		if (outstandingCount_ > peakOutstandingCount_)
		{
			peakOutstandingCount_ = outstandingCount_;
		}

		return context;
	}

	// Only contexts no longer referenced by the kernel may enter this method.
	// 只有已不再被内核引用的 context 才允许进入此方法。
	void release(Context* context)
	{
		if (context == NULL)
		{
			return;
		}

		if (outstandingCount_ > 0)
		{
			--outstandingCount_;
		}

		if (cached_.size() < cacheLimit_)
		{
			cached_.push_back(context);
		}
		else
		{
			delete context;
		}
	}

	// Poller destruction can prove safety after destroying a ring but should not populate a pool that is itself being destroyed.
	// poller 析构可在销毁 ring 后确认安全，但不应再填充一个也将析构的池。
	void discard(Context* context)
	{
		if (context == NULL)
		{
			return;
		}

		if (outstandingCount_ > 0)
		{
			--outstandingCount_;
		}
		delete context;
	}

	uint64_t allocationCount() const { return allocationCount_; }
	uint64_t reuseCount() const { return reuseCount_; }
	uint64_t outstandingCount() const { return outstandingCount_; }
	uint64_t peakOutstandingCount() const { return peakOutstandingCount_; }
	size_t cachedCount() const { return cached_.size(); }

private:
	size_t cacheLimit_;
	std::vector<Context*> cached_;
	uint64_t allocationCount_;
	uint64_t reuseCount_;
	uint64_t outstandingCount_;
	uint64_t peakOutstandingCount_;
};

}
}

#endif // KBE_COMPLETION_CONTEXT_POOL_H
