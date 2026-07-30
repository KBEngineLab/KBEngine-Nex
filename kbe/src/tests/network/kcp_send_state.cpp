#include "network/kcp_send_state.h"

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

bool testWindowAndAdmissionAreIndependent()
{
	const KBEngine::Network::KcpSendState protocolBlocked(130, 128, 128, 128, 1, false);
	const KBEngine::Network::KcpSendState admissionOnly(258, 0, 128, 128, 1, false);
	return require(protocolBlocked.isWindowBlocked(), "full protocol window was not classified as blocked") &&
		require(protocolBlocked.isAdmissionLimited(), "combined queues did not reach the admission watermark") &&
		require(!admissionOnly.isWindowBlocked(), "unsent admission backlog was misclassified as an ACK-blocked window") &&
		require(admissionOnly.isAdmissionLimited(), "unsent backlog did not trigger admission limiting");
}

bool testRemoteAndCongestionWindowsConstrainSending()
{
	const KBEngine::Network::KcpSendState remoteZero(1, 0, 128, 0, 128, false);
	const KBEngine::Network::KcpSendState congestionBlocked(1, 8, 128, 128, 8, true);
	return require(remoteZero.effectiveWindow() == 0 && remoteZero.isWindowBlocked(),
		"remote zero window did not block queued data") &&
		require(congestionBlocked.effectiveWindow() == 8 && congestionBlocked.isWindowBlocked(),
			"enabled congestion window was not enforced");
}

}

int main()
{
	if (!testWindowAndAdmissionAreIndependent() || !testRemoteAndCongestionWindowsConstrainSending())
		return EXIT_FAILURE;

	std::cout << "KCP_SEND_STATE_TEST_PASS" << std::endl;
	return EXIT_SUCCESS;
}
