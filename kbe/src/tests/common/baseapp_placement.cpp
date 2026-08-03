#include "server/baseappmgr/baseapp_placement.h"

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
	return require(std::abs(KBEngine::baseappPlacementScore(0.25f, 0, 0, 3) - 0.25) < 0.000001,
		"empty topology did not preserve live load ordering");
}

bool testClientPressureCorrectsObservedSkew()
{
	const double overloaded = KBEngine::baseappPlacementScore(0.20f, 458, 1000, 3);
	const double balanced = KBEngine::baseappPlacementScore(0.35f, 247, 1000, 3);
	return require(balanced < overloaded,
		"relative client pressure did not overcome the observed 458-to-247 placement skew");
}

bool testPendingReservationAffectsNextDecision()
{
	const double withoutReservation = KBEngine::baseappPlacementScore(0.10f, 10, 30, 3);
	const double withReservation = KBEngine::baseappPlacementScore(0.10f, 11, 31, 3);
	return require(withReservation > withoutReservation,
		"pending login reservation did not raise placement pressure");
}
}

int main()
{
	if (!testInitialPlacementUsesLiveLoad() || !testClientPressureCorrectsObservedSkew() ||
		!testPendingReservationAffectsNextDecision())
	{
		return EXIT_FAILURE;
	}

	std::cout << "BASEAPP_PLACEMENT_TEST_PASS" << std::endl;
	return EXIT_SUCCESS;
}
