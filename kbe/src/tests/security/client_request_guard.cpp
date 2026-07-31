#include "server/client_request_guard.h"

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

bool testAccountEntityMustMatchPrincipal()
{
	return require(KBEngine::Security::isBoundClientEntity(42, 42),
		"matching client principal was rejected") &&
		require(!KBEngine::Security::isBoundClientEntity(42, 43),
			"payload entity ID bypassed the Channel principal") &&
		require(!KBEngine::Security::isBoundClientEntity(0, 42),
			"unbound Channel was accepted for an account request") &&
		require(!KBEngine::Security::isBoundClientEntity(42, 0),
			"zero payload entity ID was accepted");
}

bool testCellTargetRelationshipFailsClosed()
{
	return require(KBEngine::Security::isAuthorizedClientCellTarget(
		42, 42, 7, 7, false, false),
		"self Cell RPC was rejected") &&
		require(KBEngine::Security::isAuthorizedClientCellTarget(
			42, 43, 7, 7, true, false),
			"controlled Cell RPC target was rejected") &&
		require(KBEngine::Security::isAuthorizedClientCellTarget(
			42, 43, 7, 7, false, true),
			"visible Cell RPC target was rejected") &&
		require(!KBEngine::Security::isAuthorizedClientCellTarget(
			42, 43, 7, 8, true, true),
			"cross-Space Cell RPC target was accepted") &&
		require(!KBEngine::Security::isAuthorizedClientCellTarget(
			42, 43, 7, 7, false, false),
			"unrelated Cell RPC target was accepted") &&
		require(!KBEngine::Security::isAuthorizedClientCellTarget(
			42, 43, 0, 0, true, true),
			"cross-entity Cell RPC without an active Space was accepted") &&
		require(!KBEngine::Security::isAuthorizedClientCellTarget(
			0, 43, 7, 7, true, true),
			"zero source entity ID was accepted");
}

bool testCellRelayRequiresLiveGhostOwnership()
{
	return require(KBEngine::Security::isAuthorizedCellRelay(true, 7, 7),
		"CellApp relay from the live Ghost holder was rejected") &&
		require(!KBEngine::Security::isAuthorizedCellRelay(true, 7, 8),
		"CellApp relay from an unrelated component was accepted") &&
		require(!KBEngine::Security::isAuthorizedCellRelay(false, 7, 7),
		"CellApp relay to a non-real target was accepted") &&
		require(!KBEngine::Security::isAuthorizedCellRelay(true, 0, 7),
		"CellApp relay with an unbound source was accepted") &&
		require(!KBEngine::Security::isAuthorizedCellRelay(true, 7, 0),
		"CellApp relay without a live Ghost route was accepted");
}
}

int main()
{
	if (!testAccountEntityMustMatchPrincipal() ||
		!testCellTargetRelationshipFailsClosed() ||
		!testCellRelayRequiresLiveGhostOwnership())
		return EXIT_FAILURE;

	std::cout << "SECURITY_CLIENT_REQUEST_GUARD_TEST_PASS" << std::endl;
	return EXIT_SUCCESS;
}
