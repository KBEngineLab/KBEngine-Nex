#include "common/common.h"
#include "common/memorystream.h"
#include "common/objectpool.h"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace
{
bool require(bool condition, const char* message)
{
	if (!condition)
	{
		std::cerr << message << std::endl;
	}

	return condition;
}

class TestPoolObject : public KBEngine::PoolObject
{
public:
	TestPoolObject() : reclaimCount(0)
	{
	}

	void onReclaimObject() override
	{
		++reclaimCount;
	}

	int reclaimCount;
};

bool testObjectPoolReuse()
{
	KBEngine::ObjectPool<TestPoolObject> pool("CommonPoolingTest", 0, 32);
	TestPoolObject* first = pool.createObject("first");
	TestPoolObject* second = pool.createObject("second");
	pool.reclaimObject(first);
	pool.reclaimObject(second);

	if (!require(pool.size() == OBJECT_POOL_INIT_SIZE,
		"object pool did not preserve the expected preassigned batch"))
	{
		return false;
	}

	// 空闲栈应优先复用刚归还的对象，使常用对象及其缓存保持热态。
	// The free stack should reuse the most recently returned object first, keeping frequently used objects and their caches hot.
	TestPoolObject* reused = pool.createObject("reused");
	const bool passed = require(reused == second, "object pool did not use LIFO reuse") &&
		require(reused->reclaimCount == 1, "object pool did not reset the reused object");
	pool.reclaimObject(reused);
	return passed;
}

bool testMemoryStreamRetention()
{
	KBEngine::MemoryStream smallStream;
	std::vector<KBEngine::uint8> smallPayload(KBEngine::MemoryStream::DEFAULT_SIZE * 2, 0x2a);
	smallStream.append(smallPayload.data(), smallPayload.size());
	const size_t smallCapacity = smallStream.capacity();
	smallStream.onReclaimObject();

	if (!require(smallStream.capacity() == smallCapacity, "small MemoryStream capacity was not retained") ||
		!require(smallStream.size() == smallPayload.size(), "retained MemoryStream storage was unexpectedly resized") ||
		!require(smallStream.rpos() == 0 && smallStream.wpos() == 0 && smallStream.length() == 0,
			"retained MemoryStream cursors were not reset"))
	{
		return false;
	}

	KBEngine::MemoryStream largeStream;
	std::vector<KBEngine::uint8> largePayload(KBEngine::MemoryStream::MAX_RETAINED_CAPACITY + 1, 0x5a);
	largeStream.append(largePayload.data(), largePayload.size());
	if (!require(largeStream.capacity() > KBEngine::MemoryStream::MAX_RETAINED_CAPACITY,
		"test fixture did not exceed the retained-capacity limit"))
	{
		return false;
	}

	largeStream.onReclaimObject();
	if (!require(largeStream.capacity() <= KBEngine::MemoryStream::MAX_RETAINED_CAPACITY,
			"large MemoryStream capacity remained pinned after reclaim") ||
		!require(largeStream.size() == 0 && largeStream.rpos() == 0 && largeStream.wpos() == 0,
			"compacted MemoryStream state was not reset"))
	{
		return false;
	}

	const KBEngine::uint32 expected = 0x12345678U;
	largeStream << expected;
	largeStream.rpos(0);
	KBEngine::uint32 actual = 0;
	largeStream >> actual;
	return require(actual == expected, "compacted MemoryStream could not be reused for serialization");
}
}

int main()
{
	if (!testObjectPoolReuse() || !testMemoryStreamRetention())
	{
		return EXIT_FAILURE;
	}

	std::cout << "COMMON_POOLING_TEST_PASS" << std::endl;
	return EXIT_SUCCESS;
}
