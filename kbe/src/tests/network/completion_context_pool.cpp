#include "network/completion_context_pool.h"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace
{
struct TestContext
{
	TestContext() : fd(-1), generation(0), operation(0), data()
	{
		++liveCount;
	}

	~TestContext()
	{
		--liveCount;
	}

	void reset(int newFd, unsigned long long newGeneration, int newOperation)
	{
		fd = newFd;
		generation = newGeneration;
		operation = newOperation;
		data.clear();
		if (data.capacity() > 64)
		{
			std::vector<char>().swap(data);
		}
	}

	size_t retainedBytes() const
	{
		return data.capacity();
	}

	int fd;
	unsigned long long generation;
	int operation;
	std::vector<char> data;
	static int liveCount;
};

int TestContext::liveCount = 0;

bool require(bool condition, const char* message)
{
	if (!condition)
	{
		std::cerr << message << std::endl;
	}
	return condition;
}

bool testReuseAndReset()
{
	KBEngine::Network::CompletionContextPool<TestContext> pool(2);
	TestContext* first = pool.acquire();
	first->reset(41, 7, 3);
	first->data.resize(1024);
	pool.release(first);

	TestContext* reused = pool.acquire();
	if (!require(reused == first, "released context was not reused"))
	{
		return false;
	}
	reused->reset(52, 8, 4);
	const bool valid = require(reused->fd == 52, "old fd survived context reset") &&
		require(reused->generation == 8, "old generation survived context reset") &&
		require(reused->operation == 4, "old operation survived context reset") &&
		require(reused->data.empty() && reused->data.capacity() <= 64, "oversized buffer survived context reset") &&
		require(pool.allocationCount() == 1, "allocation count is incorrect") &&
		require(pool.reuseCount() == 1, "reuse count is incorrect") &&
		require(pool.outstandingCount() == 1, "outstanding count is incorrect") &&
		require(pool.peakOutstandingCount() == 1, "peak outstanding count is incorrect");
	pool.release(reused);
	return valid && require(pool.cachedCount() == 1, "cached count is incorrect") &&
		require(pool.cachedBytes() == reused->data.capacity(), "cached bytes are incorrect");
}

bool testOutstandingAndCacheLimit()
{
	KBEngine::Network::CompletionContextPool<TestContext> pool(1);
	TestContext* first = pool.acquire();
	TestContext* second = pool.acquire();
	if (!require(first != second, "outstanding context was reused") ||
		!require(pool.peakOutstandingCount() == 2, "concurrent peak was not recorded"))
	{
		return false;
	}

	pool.release(first);
	pool.release(second);
	return require(pool.cachedCount() == 1, "cache limit was not enforced") &&
		require(TestContext::liveCount == 1, "overflow context was not deallocated");
}

bool testDiscardAndDestructor()
{
	{
		KBEngine::Network::CompletionContextPool<TestContext> pool(2);
		TestContext* discarded = pool.acquire();
		pool.discard(discarded);
		if (!require(pool.outstandingCount() == 0, "discard did not update outstanding count") ||
			!require(pool.cachedCount() == 0, "discard unexpectedly cached context"))
		{
			return false;
		}

		pool.release(pool.acquire());
	}

	return require(TestContext::liveCount == 0, "pool destructor did not release cached contexts");
}
}

int main()
{
	if (!testReuseAndReset() || !testOutstandingAndCacheLimit() || !testDiscardAndDestructor())
	{
		return EXIT_FAILURE;
	}

	std::cout << "COMPLETION_CONTEXT_POOL_TEST_PASS" << std::endl;
	return EXIT_SUCCESS;
}
