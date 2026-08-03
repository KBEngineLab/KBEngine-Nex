#include "server/cellappmgr/cellapp_placement.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace
{
bool require(bool condition, const char* message)
{
	if (!condition)
		std::cerr << message << std::endl;
	return condition;
}

bool testInitialPlacementUsesLiveLoad()
{
	return require(std::abs(KBEngine::cellappPlacementScore(0.25f, 0, 0, 6) - 0.25) < 0.000001,
		"empty topology did not preserve live load ordering");
}

bool testSpacePressureCorrectsSkew()
{
	const double overloaded = KBEngine::cellappPlacementScore(0.05f, 43, 101, 6);
	const double balanced = KBEngine::cellappPlacementScore(0.40f, 8, 101, 6);
	return require(balanced < overloaded,
		"relative Space pressure did not overcome the observed 43-to-8 placement skew");
}

bool testPendingReservationsAffectNextDecision()
{
	const double withoutReservation = KBEngine::cellappPlacementScore(0.10f, 1, 6, 6);
	const double withReservation = KBEngine::cellappPlacementScore(0.10f, 2, 7, 6);
	return require(withReservation > withoutReservation,
		"pending Space reservation did not raise placement pressure");
}

bool testConfiguredSkewBoundsCandidates()
{
	using KBEngine::cellappPlacementWithinSkew;
	return require(cellappPlacementWithinSkew(11, 10, 2),
		"candidate below the configured final Space skew was rejected") &&
		require(!cellappPlacementWithinSkew(12, 10, 2),
			"candidate at the maximum final Space skew was admitted for another allocation") &&
		require(cellappPlacementWithinSkew(100, 1, 0),
			"zero skew configuration did not preserve load-only mode");
}
}

int main()
{
	if (!testInitialPlacementUsesLiveLoad() || !testSpacePressureCorrectsSkew() ||
		!testPendingReservationsAffectNextDecision() || !testConfiguredSkewBoundsCandidates())
	{
		return EXIT_FAILURE;
	}

	std::cout << "CELLAPP_PLACEMENT_TEST_PASS" << std::endl;
	return EXIT_SUCCESS;
}
