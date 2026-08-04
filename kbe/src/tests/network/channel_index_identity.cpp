#include "network/network_interface.h"

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

bool testPoolAddressReuseRequiresMatchingGeneration()
{
	char storage = 0;
	auto* reusedAddress = reinterpret_cast<KBEngine::Network::Channel*>(&storage);
	const KBEngine::Network::ChannelIndexEntry oldSession(reusedAddress, 41);

	return require(oldSession.matches(reusedAddress, 41),
		"current channel generation was rejected") &&
		require(!oldSession.matches(reusedAddress, 42),
			"object-pool address reuse bypassed the session generation guard");
}

bool testPointerAndGenerationAreBothRequired()
{
	char firstStorage = 0;
	char secondStorage = 0;
	auto* first = reinterpret_cast<KBEngine::Network::Channel*>(&firstStorage);
	auto* second = reinterpret_cast<KBEngine::Network::Channel*>(&secondStorage);
	const KBEngine::Network::ChannelIndexEntry entry(first, 7);
	const KBEngine::Network::ChannelIndexEntry empty;

	return require(!entry.matches(second, 7),
		"different channel pointer was accepted for the same generation") &&
		require(!entry.matches(first, 0),
			"unassigned channel generation was accepted") &&
		require(!empty.matches(NULL, 0),
			"empty index entry was accepted as a current channel");
}
}

int main()
{
	if (!testPoolAddressReuseRequiresMatchingGeneration() ||
		!testPointerAndGenerationAreBothRequired())
		return EXIT_FAILURE;

	std::cout << "CHANNEL_INDEX_IDENTITY_TEST_PASS" << std::endl;
	return EXIT_SUCCESS;
}
