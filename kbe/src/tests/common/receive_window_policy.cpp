#include "network/receive_window_policy.h"

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

}

int main()
{
	using namespace KBEngine::Network;

	ReceiveWindowOverflowState unauthenticated;
	if (!require(evaluateReceiveWindowOverflow(false, 10, unauthenticated) ==
		RECEIVE_WINDOW_OVERFLOW_CONDEMN, "unauthenticated overflow was not condemned immediately"))
		return EXIT_FAILURE;

	ReceiveWindowOverflowState authenticated;
	if (!require(evaluateReceiveWindowOverflow(true, 20, authenticated) ==
		RECEIVE_WINDOW_OVERFLOW_DEFER, "first authenticated burst was not deferred"))
		return EXIT_FAILURE;
	if (!require(evaluateReceiveWindowOverflow(true, 20, authenticated) ==
		RECEIVE_WINDOW_OVERFLOW_ALREADY_RECORDED, "same-tick burst changed the consecutive streak"))
		return EXIT_FAILURE;
	if (!require(evaluateReceiveWindowOverflow(true, 21, authenticated) ==
		RECEIVE_WINDOW_OVERFLOW_DEFER, "second authenticated burst was not deferred"))
		return EXIT_FAILURE;
	if (!require(evaluateReceiveWindowOverflow(true, 22, authenticated) ==
		RECEIVE_WINDOW_OVERFLOW_CONDEMN, "third consecutive authenticated burst was not condemned"))
		return EXIT_FAILURE;

	ReceiveWindowOverflowState recovered;
	evaluateReceiveWindowOverflow(true, 30, recovered);
	if (!require(evaluateReceiveWindowOverflow(true, 32, recovered) ==
		RECEIVE_WINDOW_OVERFLOW_DEFER && recovered.consecutiveTicks == 1,
		"an idle tick did not reset the authenticated overflow streak"))
		return EXIT_FAILURE;

	std::cout << "RECEIVE_WINDOW_POLICY_TEST_PASS" << std::endl;
	return EXIT_SUCCESS;
}
