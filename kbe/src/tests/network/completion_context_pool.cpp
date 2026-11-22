#include "network/completion_context_pool.h"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace
{
struct TestContext
{
	TestContext()
	{
		++liveCount;
	}

	~TestContext()
	{
		--liveCount;
	}

	size_t retainedBytes() const
	{
		return data.capacity();
	}

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

bool testReuseAndCacheAccounting()
{
	KBEngine::Network::CompletionContextPool<TestContext> pool(2);
	TestContext* first = pool.acquire();
	first->data.resize(1024);
	const size_t capacity = first->data.capacity();
	pool.release(first);

	if (!require(pool.cachedCount() == 1, "released context was not cached") ||
		!require(pool.cachedBytes() == capacity, "released context buffer was not counted"))
	{
		return false;
	}

	TestContext* reused = pool.acquire();
	const bool valid = require(reused == first, "released context was not reused") &&
		require(pool.cachedCount() == 0, "reacquired context is still counted as cached") &&
		require(pool.cachedBytes() == 0, "reacquired buffer is still counted as cached") &&
		require(pool.allocationCount() == 1, "allocation count is incorrect") &&
		require(pool.reuseCount() == 1, "reuse count is incorrect") &&
		require(pool.outstandingCount() == 1, "outstanding count is incorrect") &&
		require(pool.peakOutstandingCount() == 1, "peak outstanding count is incorrect");
	pool.release(reused);
	return valid && require(pool.cachedCount() == 1, "cached count is incorrect") &&
		require(pool.cachedBytes() == capacity, "cached bytes are incorrect");
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
	if (!testReuseAndCacheAccounting() || !testOutstandingAndCacheLimit() || !testDiscardAndDestructor())
	{
		return EXIT_FAILURE;
	}

	std::cout << "COMPLETION_CONTEXT_POOL_TEST_PASS" << std::endl;
	return EXIT_SUCCESS;
}
