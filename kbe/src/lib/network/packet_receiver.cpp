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


#include "packet_receiver.h"
#ifndef CODE_INLINE
#include "packet_receiver.inl"
#endif

#include "network/address.h"
#include "network/bundle.h"
#include "network/channel.h"
#include "network/endpoint.h"
#include "network/event_dispatcher.h"
#include "network/network_interface.h"
#include "network/event_poller.h"

namespace KBEngine { 
namespace Network
{
//-------------------------------------------------------------------------------------
PacketReceiver::PacketReceiver() :
	pEndpoint_(NULL),
	pChannel_(NULL),
	pNetworkInterface_(NULL)
{
}

//-------------------------------------------------------------------------------------
PacketReceiver::PacketReceiver(EndPoint & endpoint,
	   NetworkInterface & networkInterface	) :
	pEndpoint_(&endpoint),
	pChannel_(NULL),
	pNetworkInterface_(&networkInterface)
{
}

//-------------------------------------------------------------------------------------
PacketReceiver::~PacketReceiver()
{
}

//-------------------------------------------------------------------------------------
int PacketReceiver::handleInputNotification(KBESOCKET fd)
{
	EventPoller* pPoller = this->dispatcher().pPoller();
	if (pPoller != NULL && pPoller->supportsCompletion())
	{
		// 完成后端的每次通知只消费一个已完成接收，使 completion 时间/数量预算
		// 与实际报文一一对应。io_uring 会把 socket burst 搬入用户态队列，但不能
		// 再由一个 CQE 无界清空整队，否则 KCP 会长期挤占内部 TCP 与游戏 Tick。
		// A completion notification consumes exactly one completed receive so count/time
		// budgets correspond to real packets. io_uring may stage a socket burst in user
		// space, but one CQE must not drain the whole queue and starve internal TCP or ticks.
		this->processRecv(true);
		return 0;
	}

	if (this->processRecv(true))
	{
		while (this->processRecv(false))
		{
		}
	}

	return 0;
}

//-------------------------------------------------------------------------------------
Reason PacketReceiver::processPacket(Channel* pChannel, Packet * pPacket)
{
	if (pChannel != NULL)
	{
		pChannel->onPacketReceived((int)pPacket->length());

		if (pChannel->pFilter())
		{
			return pChannel->pFilter()->recv(pChannel, *this, pPacket);
		}
	}

	return this->processFilteredPacket(pChannel, pPacket);
}

//-------------------------------------------------------------------------------------
EventDispatcher & PacketReceiver::dispatcher()
{
	return this->pNetworkInterface_->dispatcher();
}

//-------------------------------------------------------------------------------------
Channel* PacketReceiver::getChannel()
{
	const ProtocolType expectedProtocol = type() == UDP_PACKET_RECEIVER ? PROTOCOL_UDP : PROTOCOL_TCP;
	if (pChannel_)
	{
		if (!pChannel_->isDestroyed() && pChannel_->protocoltype() == expectedProtocol)
			return pChannel_;

		pChannel_ = NULL;
	}

	// TCP and UDP may share the same numeric peer port. Bind by the receiver's
	// transport type so a KCP receiver can never cache an unrelated TCP Channel.
	// TCP 与 UDP 可以共享同一数值对端端口。必须按 receiver 的传输类型绑定，
	// 避免 KCP receiver 缓存无关的 TCP Channel。
	pChannel_ = pNetworkInterface_->findChannel(pEndpoint_->addr(), expectedProtocol);
	return pChannel_;
}

//-------------------------------------------------------------------------------------
}
}
