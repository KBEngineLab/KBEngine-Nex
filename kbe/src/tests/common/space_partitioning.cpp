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
		uint16 version;
		uint8 remote;
		bool accepted;
	};
	const Case cases[] = {
		{"disabled", disabled, PROTOCOL_VERSION, disabled, true},
		{"disabled_without_coordinates", disabled, PROTOCOL_VERSION, 0, true},
		{"disabled_with_y", disabled, PROTOCOL_VERSION, modeFlags(false, true, true), true},
		{"enabled", enabled, PROTOCOL_VERSION, enabled, true},
		{"enabled_with_y", enabledY, PROTOCOL_VERSION, enabledY, true},
		{"local_disabled", disabled, PROTOCOL_VERSION, enabled, false},
		{"remote_disabled", enabled, PROTOCOL_VERSION, disabled, false},
		{"old_protocol", enabled, 0, enabled, false},
		{"switch_only_protocol", enabled, 1, enabled, false},
		{"previous_layout_protocol", enabled, 2, enabled, false},
		{"disabled_previous_layout_protocol", disabled, 2, disabled, false},
		{"new_protocol", enabled, PROTOCOL_VERSION + 1, enabled, false},
		{"disabled_old_protocol", disabled, 0, disabled, false},
		{"unknown_flags", enabled, PROTOCOL_VERSION, static_cast<uint8>(enabled | 8), false},
		{"disabled_unknown_flags", disabled, PROTOCOL_VERSION, static_cast<uint8>(disabled | 8), false},
		{"local_without_coordinates", noCoordinates, PROTOCOL_VERSION, enabled, false},
		{"remote_without_coordinates", enabled, PROTOCOL_VERSION, noCoordinates, false},
		{"both_without_coordinates", noCoordinates, PROTOCOL_VERSION, noCoordinates, false},
		{"different_y", enabled, PROTOCOL_VERSION, enabledY, false},
	};
	for (const Case& test : cases)
	{
		if (compatible(test.local, test.version, test.remote) != test.accepted)
		{
			std::fprintf(stderr, "SPACE_PARTITIONING_TEST_FAIL case=%s\n", test.name);
			return 1;
		}
	}

	std::printf("SPACE_PARTITIONING_TEST_PASS cases=%zu\n", sizeof(cases) / sizeof(cases[0]));
	return 0;
}
