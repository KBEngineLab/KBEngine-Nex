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


#include "packet_sender.h"
#ifndef CODE_INLINE
#include "packet_sender.inl"
#endif

#include "network/address.h"
#include "network/bundle.h"
#include "network/channel.h"
#include "network/endpoint.h"
#include "network/event_dispatcher.h"
#include "network/network_interface.h"
#include "network/event_poller.h"
#include <openssl/err.h>

namespace KBEngine { 
namespace Network
{
//-------------------------------------------------------------------------------------
PacketSender::PacketSender() :
	pEndpoint_(NULL),
	pChannel_(NULL),
	pNetworkInterface_(NULL)
{
}

//-------------------------------------------------------------------------------------
PacketSender::PacketSender(EndPoint & endpoint,
	   NetworkInterface & networkInterface):
	pEndpoint_(&endpoint),
	pChannel_(NULL),
	pNetworkInterface_(&networkInterface)
{
}

//-------------------------------------------------------------------------------------
PacketSender::~PacketSender()
{
}

//-------------------------------------------------------------------------------------
Channel* PacketSender::getChannel()
{
	const ProtocolType expectedProtocol = type() == UDP_PACKET_SENDER ? PROTOCOL_UDP : PROTOCOL_TCP;
	if (pChannel_)
	{
		if (!pChannel_->isDestroyed() && pChannel_->protocoltype() == expectedProtocol)
			return pChannel_;

		pChannel_ = NULL;
	}

	// Keep sender binding in the same transport namespace as its receiver peer;
	// address-only lookup is ambiguous when TCP and KCP reuse a numeric port.
	// sender 必须与 receiver 使用相同的传输命名空间；当 TCP 与 KCP 复用
	// 数值端口时，仅地址查找存在歧义。
	pChannel_ = pNetworkInterface_->findChannel(pEndpoint_->addr(), expectedProtocol);
	return pChannel_;
}

//-------------------------------------------------------------------------------------
int PacketSender::handleOutputNotification(KBESOCKET fd)
{
	processSend(NULL, 0);
	return 0;
}

//-------------------------------------------------------------------------------------
Reason PacketSender::processPacket(Channel* pChannel, Packet * pPacket, int userarg)
{
	if (pChannel != NULL)
	{
		if (pChannel->pFilter())
		{
			return pChannel->pFilter()->send(pChannel, *this, pPacket, userarg);
		}
	}

	return this->processFilterPacket(pChannel, pPacket, userarg);
}

//-------------------------------------------------------------------------------------
EventDispatcher & PacketSender::dispatcher()
{
	return pNetworkInterface_->dispatcher();
}

//-------------------------------------------------------------------------------------
Reason PacketSender::checkSocketErrors(const EndPoint * pEndpoint)
{
	int err;
	Reason reason;

#if KBE_PLATFORM != PLATFORM_WIN32
		err = errno;

		switch (err)
		{
			case ECONNREFUSED:	reason = REASON_NO_SUCH_PORT; break;
			case EAGAIN:		reason = REASON_RESOURCE_UNAVAILABLE; break;
			case EPIPE:			reason = REASON_CLIENT_DISCONNECTED; break;
			case ECONNRESET:	reason = REASON_CLIENT_DISCONNECTED; break;
			case ENOBUFS:		reason = REASON_TRANSMIT_QUEUE_FULL; break;
			default:			reason = REASON_GENERAL_NETWORK; break;
		}
#else
		err = WSAGetLastError();

		if (err == WSAEWOULDBLOCK || err == WSAEINTR)
		{
			reason = REASON_RESOURCE_UNAVAILABLE;
		}
		else
		{
			switch (err)
			{
				case WSAECONNREFUSED:	reason = REASON_NO_SUCH_PORT; break;
				case WSAECONNRESET:	reason = REASON_CLIENT_DISCONNECTED; break;
				case WSAECONNABORTED:	reason = REASON_CLIENT_DISCONNECTED; break;
				default:reason = REASON_GENERAL_NETWORK;break;
			}
		}
#endif

	if (err == 0 && pEndpoint->isSSL())
	{
		long sslerr = ERR_get_error();
		if (sslerr > 0)
		{
			return REASON_WEBSOCKET_ERROR;
		}
	}

	return reason;
}

//-------------------------------------------------------------------------------------
}
}
