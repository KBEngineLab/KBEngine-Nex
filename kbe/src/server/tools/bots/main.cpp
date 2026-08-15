/*
This source file is part of KBEngine
For the latest info, see http://www.kbengine.org/

Copyright (c) 2008-2018 KBEngine.

KBEngine is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

KBEngine is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.
 
You should have received a copy of the GNU Lesser General Public License
along with KBEngine.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "client_lib/kbemain.h"
#include "bots.h"

#include <cstdio>
#include <string>

#undef DEFINE_IN_INTERFACE
#include "client_lib/client_interface.h"
#define DEFINE_IN_INTERFACE
#define CLIENT
#include "client_lib/client_interface.h"

#undef DEFINE_IN_INTERFACE
#include "baseapp/baseapp_interface.h"
#define DEFINE_IN_INTERFACE
#include "baseapp/baseapp_interface.h"

#undef DEFINE_IN_INTERFACE
#include "loginapp/loginapp_interface.h"
#define DEFINE_IN_INTERFACE
#include "loginapp/loginapp_interface.h"

#undef DEFINE_IN_INTERFACE
#include "cellapp/cellapp_interface.h"
#define DEFINE_IN_INTERFACE
#include "cellapp/cellapp_interface.h"

#undef DEFINE_IN_INTERFACE
#include "baseappmgr/baseappmgr_interface.h"
#define DEFINE_IN_INTERFACE
#include "baseappmgr/baseappmgr_interface.h"

#undef DEFINE_IN_INTERFACE
#include "dbmgr/dbmgr_interface.h"
#define DEFINE_IN_INTERFACE
#include "dbmgr/dbmgr_interface.h"

#undef DEFINE_IN_INTERFACE
#include "machine/machine_interface.h"
#define DEFINE_IN_INTERFACE
#include "machine/machine_interface.h"

#undef DEFINE_IN_INTERFACE
#include "cellappmgr/cellappmgr_interface.h"
#define DEFINE_IN_INTERFACE
#include "cellappmgr/cellappmgr_interface.h"

#undef DEFINE_IN_INTERFACE
#include "tools/logger/logger_interface.h"
#define DEFINE_IN_INTERFACE
#include "tools/logger/logger_interface.h"

#undef DEFINE_IN_INTERFACE
#include "tools/interfaces/interfaces_interface.h"
#define DEFINE_IN_INTERFACE
#include "tools/interfaces/interfaces_interface.h"

using namespace KBEngine;

namespace
{
bool parseBotsTransport(const std::string& value)
{
	if (value == "kcp")
	{
		g_botsTransportOverride = 0;
		return true;
	}

	if (value == "tcp")
	{
		g_botsTransportOverride = 1;
		return true;
	}

	return false;
}

bool parseBooleanOverride(const std::string& value, int8& output)
{
	if (value == "true" || value == "1")
	{
		output = 1;
		return true;
	}

	if (value == "false" || value == "0")
	{
		output = 0;
		return true;
	}

	return false;
}
}

int KBENGINE_MAIN(int argc, char* argv[])
{
	g_componentType = BOTS_TYPE;
	for (int index = 1; index < argc; ++index)
	{
		if (std::string(argv[index]) == "--dev")
		{
			g_botsDevMode = true;
			g_botsPublishComponent = true;
		}
		else if (std::string(argv[index]) == "--publish-component")
		{
			g_botsPublishComponent = true;
		}
		else if (std::string(argv[index]) == "--reuse-existing-accounts")
		{
			g_botsReuseAccounts = true;
		}
		else if (std::string(argv[index]).find("--bots-transport=") == 0)
		{
			const std::string value = std::string(argv[index]).substr(std::string("--bots-transport=").size());
			if (!parseBotsTransport(value))
			{
				fprintf(stderr, "[ERROR]: --bots-transport expects 'kcp' or 'tcp'.\n");
				return 2;
			}
		}
		else if (std::string(argv[index]).find("--bots-allow-tcp-fallback=") == 0)
		{
			const std::string value = std::string(argv[index]).substr(
				std::string("--bots-allow-tcp-fallback=").size());
			if (!parseBooleanOverride(value, g_botsAllowTcpFallbackOverride))
			{
				fprintf(stderr, "[ERROR]: --bots-allow-tcp-fallback expects 'true' or 'false'.\n");
				return 2;
			}
		}
	}

	return kbeMainT<Bots>(argc, argv, g_componentType, -1, -1, 0, 0, 0, "");
}
