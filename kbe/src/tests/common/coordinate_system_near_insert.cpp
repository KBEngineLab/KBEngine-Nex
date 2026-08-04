#include "server/cellapp/coordinate_node.h"
#include "server/cellapp/coordinate_system.h"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace
{
class TestNode : public KBEngine::CoordinateNode
{
public:
	TestNode(float x, float y, float z, bool hidden = false) :
		targetX_(x), targetY_(y), targetZ_(z), passCount_(0)
	{
		if (hidden)
			addFlags(COORDINATE_NODE_FLAG_HIDE);
	}

	float xx() const override { return targetX_; }
	float yy() const override { return targetY_; }
	float zz() const override { return targetZ_; }

	void target(float x, float y, float z)
	{
		targetX_ = x;
		targetY_ = y;
		targetZ_ = z;
	}

	void onNodePassX(KBEngine::CoordinateNode*, bool) override { ++passCount_; }
	void onNodePassY(KBEngine::CoordinateNode*, bool) override { ++passCount_; }
	void onNodePassZ(KBEngine::CoordinateNode*, bool) override { ++passCount_; }

	std::size_t passCount() const { return passCount_; }

private:
	float targetX_;
	float targetY_;
	float targetZ_;
	std::size_t passCount_;
};

bool require(bool condition, const char* message)
{
	if (!condition)
		std::cerr << message << std::endl;
	return condition;
}

bool isSortedX(const KBEngine::CoordinateSystem& system)
{
	const KBEngine::CoordinateNode* previous = NULL;
	for (const KBEngine::CoordinateNode* node = system.pFirstXNode(); node;
		node = node->pNextX())
	{
		if ((previous && previous->x() > node->x()) || node->pPrevX() != previous)
			return false;
		previous = node;
	}
	return true;
}

bool isSortedY(const KBEngine::CoordinateSystem& system)
{
	const KBEngine::CoordinateNode* previous = NULL;
	for (const KBEngine::CoordinateNode* node = system.pFirstYNode(); node;
		node = node->pNextY())
	{
		if ((previous && previous->y() > node->y()) || node->pPrevY() != previous)
			return false;
		previous = node;
	}
	return true;
}

bool isSortedZ(const KBEngine::CoordinateSystem& system)
{
	const KBEngine::CoordinateNode* previous = NULL;
	for (const KBEngine::CoordinateNode* node = system.pFirstZNode(); node;
		node = node->pNextZ())
	{
		if ((previous && previous->z() > node->z()) || node->pPrevZ() != previous)
			return false;
		previous = node;
	}
	return true;
}

bool testNearInsertSkipsUnrelatedPrefix(bool hasY)
{
	KBEngine::CoordinateSystem::hasY = hasY;
	KBEngine::CoordinateSystem system;
	std::vector<TestNode*> nodes;
	for (int coordinate = -100; coordinate <= 100; ++coordinate)
	{
		TestNode* node = new TestNode(static_cast<float>(coordinate),
			static_cast<float>(coordinate), static_cast<float>(coordinate));
		nodes.push_back(node);
		system.insert(node);
	}

	TestNode* anchor = nodes[100];
	TestNode* boundary = new TestNode(0.0f, 0.0f, 0.0f, true);
	if (!require(system.insertNear(boundary, anchor), "near insertion failed") ||
		!require(boundary->pPrevX() == anchor && boundary->pPrevZ() == anchor,
			"boundary was not linked beside the anchor") ||
		!require(!hasY || boundary->pPrevY() == anchor,
			"3D boundary was not linked beside the Y anchor") ||
		!require(boundary->passCount() == 0,
			"near insertion fired a pass callback before range expansion"))
	{
		return false;
	}

	boundary->target(5.0f, hasY ? 5.0f : 0.0f, 5.0f);
	boundary->update();
	const std::size_t expectedPasses = hasY ? 15 : 10;
	if (!require(boundary->passCount() == expectedPasses,
		"range expansion did not visit exactly the covered axis intervals") &&
		require(isSortedX(system) && isSortedZ(system) && (!hasY || isSortedY(system)),
			"near insertion or expansion broke coordinate ordering"))
	{
		return false;
	}

	// 负边界从 origin 后方挂入后会先穿过 origin，再覆盖负方向实体；RangeTrigger
	// 自身会忽略 origin 回调。该计数确保反向移动没有跳过或全表扫描。
	// A negative boundary linked after the origin crosses the origin first and
	// then covers negative entities; RangeTrigger itself ignores the origin
	// callback. This count proves reverse movement neither skips nor scans globally.
	TestNode* negativeBoundary = new TestNode(0.0f, 0.0f, 0.0f, true);
	if (!require(system.insertNear(negativeBoundary, anchor),
		"negative boundary near insertion failed"))
	{
		return false;
	}

	negativeBoundary->target(-5.0f, hasY ? -5.0f : 0.0f, -5.0f);
	negativeBoundary->update();
	// 旧等值排序会在每个轴上对目标端点先反向越过、再正向越回一次；保留该
	// 兼容顺序，但断言扫描仍局限于 origin 到目标端点之间。
	// Legacy equal-coordinate ordering crosses the target endpoint backward and
	// then forward once per axis. Preserve that compatibility while asserting
	// that scanning remains bounded to the origin-to-target interval.
	const std::size_t expectedNegativePasses = hasY ? 21 : 14;
	return require(negativeBoundary->passCount() == expectedNegativePasses,
		"negative range expansion visited an unexpected coordinate interval") &&
		require(isSortedX(system) && isSortedZ(system) && (!hasY || isSortedY(system)),
			"negative expansion broke coordinate ordering") &&
		require(system.size() == 203, "near insertions did not preserve system size");
}

bool testNearInsertRejectsUnsafeNodes()
{
	KBEngine::CoordinateSystem::hasY = false;
	KBEngine::CoordinateSystem system;
	TestNode* anchor = new TestNode(0.0f, 0.0f, 0.0f);
	system.insert(anchor);

	TestNode* visible = new TestNode(0.0f, 0.0f, 0.0f);
	const bool rejected = !system.insertNear(visible, anchor);
	delete visible;
	return require(rejected, "near insertion accepted a visible node");
}
}

int main()
{
	if (!testNearInsertSkipsUnrelatedPrefix(false) ||
		!testNearInsertSkipsUnrelatedPrefix(true) ||
		!testNearInsertRejectsUnsafeNodes())
	{
		return EXIT_FAILURE;
	}

	std::cout << "COORDINATE_SYSTEM_NEAR_INSERT_TEST_PASS" << std::endl;
	return EXIT_SUCCESS;
}
