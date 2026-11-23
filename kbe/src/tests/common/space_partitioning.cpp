#include "server/space_partitioning.h"

#include <cstdio>

int main()
{
	using namespace KBEngine;
	using namespace KBEngine::SpacePartitioning;

	const uint8 disabled = modeFlags(false, true, false);
	const uint8 enabled = modeFlags(true, true, false);
	const uint8 enabledY = modeFlags(true, true, true);
	const uint8 noCoordinates = modeFlags(true, false, false);
	struct Case
	{
		const char* name;
		uint8 local;
		uint8 remote;
		bool accepted;
	};
	const Case cases[] = {
		{"disabled", disabled, disabled, true},
		{"disabled_without_coordinates", disabled, 0, true},
		{"disabled_with_y", disabled, modeFlags(false, true, true), true},
		{"enabled", enabled, enabled, true},
		{"enabled_with_y", enabledY, enabledY, true},
		{"local_disabled", disabled, enabled, false},
		{"remote_disabled", enabled, disabled, false},
		{"unknown_flags", enabled, static_cast<uint8>(enabled | 8), false},
		{"disabled_unknown_flags", disabled, static_cast<uint8>(disabled | 8), false},
		{"local_without_coordinates", noCoordinates, enabled, false},
		{"remote_without_coordinates", enabled, noCoordinates, false},
		{"both_without_coordinates", noCoordinates, noCoordinates, false},
		{"different_y", enabled, enabledY, false},
	};
	for (const Case& test : cases)
	{
		if (compatible(test.local, test.remote) != test.accepted)
		{
			std::fprintf(stderr, "SPACE_PARTITIONING_TEST_FAIL case=%s\n", test.name);
			return 1;
		}
	}

	std::printf("SPACE_PARTITIONING_TEST_PASS cases=%zu\n", sizeof(cases) / sizeof(cases[0]));
	return 0;
}
