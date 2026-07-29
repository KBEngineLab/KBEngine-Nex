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

#include "network_stats.h"
#include "helper/watcher.h"
#include "network/message_handler.h"
#include <limits>

namespace KBEngine { 

KBE_SINGLETON_INIT(Network::NetworkStats);

namespace Network
{

NetworkStats g_networkStats;

//-------------------------------------------------------------------------------------
NetworkStats::NetworkStats():
stats_(),
handlers_()
{
}

//-------------------------------------------------------------------------------------
NetworkStats::~NetworkStats()
{
}

//-------------------------------------------------------------------------------------
void NetworkStats::addHandler(NetworkStatsHandler* pHandler)
{
	handlers_.push_back(pHandler);
}

//-------------------------------------------------------------------------------------
void NetworkStats::removeHandler(NetworkStatsHandler* pHandler)
{
	std::vector<NetworkStatsHandler*>::iterator iter = handlers_.begin();
	for(; iter != handlers_.end(); ++iter)
	{
		if((*iter) == pHandler)
		{
			handlers_.erase(iter);
			break;
		}
	}
}

//-------------------------------------------------------------------------------------
void NetworkStats::trackMessage(S_OP op, const MessageHandler& msgHandler, size_t size)
{
	// 单条网络消息的协议长度最多为uint32，统计层在更新历史计数器前验证这一不变量。
	// A protocol message is at most uint32 bytes, so validate that invariant before updating legacy statistics counters.
	if(size > static_cast<size_t>(std::numeric_limits<uint32>::max()))
	{
		ERROR_MSG(fmt::format("NetworkStats::trackMessage: message '{}' size {} exceeds uint32.\n",
			msgHandler.name, size));
		return;
	}

	const uint32 messageSize = static_cast<uint32>(size);
	MessageHandler* pMsgHandler = const_cast<MessageHandler*>(&msgHandler);

	if(op == SEND)
	{
		pMsgHandler->send_size += messageSize;
		pMsgHandler->send_count++;
	}
	else
	{
		pMsgHandler->recv_size += messageSize;
		pMsgHandler->recv_count++;
	}

	std::vector<NetworkStatsHandler*>::iterator iter = handlers_.begin();
	for(; iter != handlers_.end(); ++iter)
	{
		if(op == SEND)
			(*iter)->onSendMessage(msgHandler, messageSize);
		else
			(*iter)->onRecvMessage(msgHandler, messageSize);
	}
}

//-------------------------------------------------------------------------------------
}
}
