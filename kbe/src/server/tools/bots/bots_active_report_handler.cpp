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

#include "bots_active_report_handler.h"

#include "bots_interface.h"
#include "baseapp/baseapp_interface.h"
#include "baseappmgr/baseappmgr_interface.h"
#include "cellapp/cellapp_interface.h"
#include "cellappmgr/cellappmgr_interface.h"
#include "client_lib/clientapp.h"
#include "dbmgr/dbmgr_interface.h"
#include "loginapp/loginapp_interface.h"
#include "network/bundle.h"
#include "server/components.h"
#include "tools/logger/logger_interface.h"
#include "tools/interfaces/interfaces_interface.h"

namespace KBEngine
{

//-------------------------------------------------------------------------------------
BotsActiveReportHandler::BotsActiveReportHandler(ClientApp* pApp) :
	pApp_(pApp),
	timerHandle_()
{
}

//-------------------------------------------------------------------------------------
BotsActiveReportHandler::~BotsActiveReportHandler()
{
	cancel();
}

//-------------------------------------------------------------------------------------
void BotsActiveReportHandler::start(float period)
{
	cancel();
	timerHandle_ = pApp_->dispatcher().addTimer(
		static_cast<int64>(period * 1000000.f), this,
		reinterpret_cast<void*>(TIMEOUT_ACTIVE_TICK));
}

//-------------------------------------------------------------------------------------
void BotsActiveReportHandler::cancel()
{
	timerHandle_.cancel();
}

//-------------------------------------------------------------------------------------
void BotsActiveReportHandler::handleTimeout(TimerHandle handle, void* pUser)
{
	if (reinterpret_cast<uintptr>(pUser) != TIMEOUT_ACTIVE_TICK)
		return;

	// Bots 没有 ServerApp 的公共心跳发送器，因此必须主动向内部组件报告存活，避免组件表把长时间运行的 Bots 判为失联。
	// Bots has no ServerApp heartbeat publisher, so it must report liveness to internal components to avoid being expired from their registries.
	const COMPONENT_TYPE componentTypes[] = {
		BASEAPPMGR_TYPE,
		CELLAPPMGR_TYPE,
		DBMGR_TYPE,
		CELLAPP_TYPE,
		BASEAPP_TYPE,
		LOGINAPP_TYPE,
		LOGGER_TYPE,
		BOTS_TYPE
	};

	for (size_t typeIndex = 0; typeIndex < sizeof(componentTypes) / sizeof(componentTypes[0]); ++typeIndex)
	{
		const COMPONENT_TYPE componentType = componentTypes[typeIndex];
		Components::COMPONENTS& components = Components::getSingleton().getComponents(componentType);

		for (Components::COMPONENTS::iterator iter = components.begin(); iter != components.end(); ++iter)
		{
			if (iter->pChannel == NULL || iter->pChannel->isDestroyed())
				continue;

			Network::Bundle* pBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
			COMMON_NETWORK_MESSAGE(componentType, (*pBundle), onAppActiveTick);
			(*pBundle) << g_componentType << g_componentID;
			iter->pChannel->send(pBundle);
		}
	}
}

}
