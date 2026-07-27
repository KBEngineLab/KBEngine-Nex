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

#include "bots.h"
#include "clientobject.h"
#include "create_and_login_handler.h"
#include "network/network_interface.h"
#include "network/event_dispatcher.h"
#include "network/address.h"
#include "network/bundle.h"
#include "common/memorystream.h"
#include "server/serverconfig.h"

namespace KBEngine { 

uint64 g_accountID = 0;

//-------------------------------------------------------------------------------------
CreateAndLoginHandler::CreateAndLoginHandler():
nextCreateTime_(0),
wasPending_(false)
{
	// 使用 100ms 调度粒度以兑现既有 tickTime 小数秒语义，同时仍由批量参数限制每次创建量。
	// Use a 100 ms scheduling quantum to honor the existing fractional tickTime contract while retaining per-batch limits.
	timerHandle_ = Bots::getSingleton().networkInterface().dispatcher().addTimer(
							100000, this);

	g_accountID = KBEngine::genUUID64() * 100000;
	if(g_kbeSrvConfig.getBots().bots_account_name_suffix_inc > 0)
	{
		g_accountID = g_kbeSrvConfig.getBots().bots_account_name_suffix_inc;
	}
}

//-------------------------------------------------------------------------------------
CreateAndLoginHandler::~CreateAndLoginHandler()
{
	timerHandle_.cancel();
}

//-------------------------------------------------------------------------------------
void CreateAndLoginHandler::handleTimeout(TimerHandle handle, void * arg)
{
	KBE_ASSERT(handle == timerHandle_);
	
	Bots& bots = Bots::getSingleton();

	const bool hasPendingClients = bots.reqCreateAndLoginTotalCount() > bots.clients().size();
	if (!hasPendingClients)
	{
		wasPending_ = false;
		return;
	}

	const uint64 now = timestamp();
	if (!wasPending_)
	{
		// 新增批次立即开始，避免沿用上一次已经完成的等待窗口。
		// Start a newly queued batch immediately instead of inheriting a completed batch's delay window.
		nextCreateTime_ = now;
		wasPending_ = true;
	}

	if (now < nextCreateTime_)
		return;

	// 至少推进一个客户端，避免错误配置把待创建队列永久锁死。
	// Always advance at least one client so an invalid configuration cannot permanently stall the pending queue.
	uint32 count = KBE_MAX<uint32>(1, bots.reqCreateAndLoginTickCount());

	while(bots.reqCreateAndLoginTotalCount() - bots.clients().size() > 0 && count-- > 0)
	{
		ClientObject* pClient = new ClientObject(g_kbeSrvConfig.getBots().bots_account_name_prefix + 
			KBEngine::StringConv::val2str(g_componentID) + "_" + KBEngine::StringConv::val2str(g_accountID++), 
			Bots::getSingleton().networkInterface());

		Bots::getSingleton().addClient(pClient);
	}

	const float intervalSeconds = KBE_MAX(0.f, bots.reqCreateAndLoginTickTime());
	nextCreateTime_ = timestamp() + static_cast<uint64>(intervalSeconds * stampsPerSecond());
}

//-------------------------------------------------------------------------------------

}
