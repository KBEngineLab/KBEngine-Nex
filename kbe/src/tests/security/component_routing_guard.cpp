#include "server/component_routing_guard.h"

#include <cstdlib>
#include <iostream>
#include <limits>

namespace
{
struct FakeComponentInfos
{
	KBEngine::Network::Channel* pChannel;
	KBEngine::COMPONENT_TYPE componentType;
	KBEngine::COMPONENT_ID cid;
};

bool require(bool condition, const char* message)
{
	if (!condition)
		std::cerr << message << std::endl;
	return condition;
}

bool testMutatedTargetsFailClosed()
{
	char channelStorage = 0;
	auto* channel = reinterpret_cast<KBEngine::Network::Channel*>(&channelStorage);
	FakeComponentInfos online{channel, KBEngine::BASEAPP_TYPE, 42};
	FakeComponentInfos offline{NULL, KBEngine::BASEAPP_TYPE, 42};

	return require(KBEngine::Security::concreteComponentChannel(0, &online) == NULL,
		"component ID 0 retained discovery wildcard behavior in packet routing") &&
		require(KBEngine::Security::concreteComponentChannel(
			std::numeric_limits<KBEngine::COMPONENT_ID>::max(),
			static_cast<FakeComponentInfos*>(NULL)) == NULL,
			"unknown mutated component ID did not fail closed") &&
		require(KBEngine::Security::concreteComponentChannel(42, &offline) == NULL,
			"offline component was accepted as a route target") &&
		require(KBEngine::Security::concreteComponentChannel(42, &online) == channel,
			"valid component route was rejected");
}

bool testPayloadSenderMustMatchChannel()
{
	char sourceStorage = 0;
	char attackerStorage = 0;
	auto* source = reinterpret_cast<KBEngine::Network::Channel*>(&sourceStorage);
	auto* attacker = reinterpret_cast<KBEngine::Network::Channel*>(&attackerStorage);
	FakeComponentInfos registered{source, KBEngine::BASEAPP_TYPE, 7};

	return require(KBEngine::Security::isBoundComponentSource(7, &registered, source),
		"registered component source was rejected") &&
		require(!KBEngine::Security::isBoundComponentSource(7, &registered, attacker),
			"payload sender ID was trusted over the actual Channel") &&
		require(!KBEngine::Security::isBoundComponentSource(0, &registered, source),
			"zero sender ID inherited discovery wildcard behavior") &&
		require(!KBEngine::Security::isBoundComponentSource(7, &registered, NULL),
			"null source Channel was accepted") &&
		require(KBEngine::Security::isExpectedComponentSource(
			KBEngine::BASEAPP_TYPE, &registered, source),
			"registered source component type was rejected") &&
		require(!KBEngine::Security::isExpectedComponentSource(
			KBEngine::CELLAPP_TYPE, &registered, source),
			"wrong source component type was accepted");
}

bool testMalformedMetricsFailClosed()
{
	return require(KBEngine::Security::isValidComponentMetric(0.f),
		"zero component metric was rejected") &&
		require(KBEngine::Security::isValidComponentMetric(1.f),
			"valid component metric was rejected") &&
		require(!KBEngine::Security::isValidComponentMetric(-1.f),
			"negative component metric was accepted") &&
		require(!KBEngine::Security::isValidComponentMetric(
			std::numeric_limits<float>::quiet_NaN()),
			"NaN component metric was accepted") &&
		require(!KBEngine::Security::isValidComponentMetric(
			std::numeric_limits<float>::infinity()),
			"infinite component metric was accepted");
}
}

int main()
{
	if (!testMutatedTargetsFailClosed() ||
		!testPayloadSenderMustMatchChannel() ||
		!testMalformedMetricsFailClosed())
		return EXIT_FAILURE;

	std::cout << "SECURITY_COMPONENT_ROUTING_GUARD_TEST_PASS" << std::endl;
	return EXIT_SUCCESS;
}
