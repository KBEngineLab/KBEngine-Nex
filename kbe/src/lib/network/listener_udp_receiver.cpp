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

#include "listener_udp_receiver.h"
#ifndef CODE_INLINE
#include "listener_udp_receiver.inl"
#endif

#include "network/address.h"
#include "network/bundle.h"
#include "network/endpoint.h"
#include "network/event_dispatcher.h"
#include "network/network_interface.h"
#include "network/packet_receiver.h"
#include "network/kcp_packet_receiver.h"
#include "network/error_reporter.h"

#include "network/ikcp.h"

namespace KBEngine {
namespace Network
{
//-------------------------------------------------------------------------------------
ListenerUdpReceiver::ListenerUdpReceiver(EndPoint & endpoint,
								   Channel::Traits traits,
									NetworkInterface & networkInterface	):
	ListenerReceiver(endpoint, traits, networkInterface),
	pUDPPacketReceiver_(NULL)
{
	pUDPPacketReceiver_ = new KCPPacketReceiver(endpoint, networkInterface);
}

//-------------------------------------------------------------------------------------
ListenerUdpReceiver::~ListenerUdpReceiver()
{
	SAFE_RELEASE(pUDPPacketReceiver_);
}

//-------------------------------------------------------------------------------------
int ListenerUdpReceiver::handleInputNotification(int fd)
{
	int tickcount = 0;

	while (tickcount++ < 256)
	{
		if (!pUDPPacketReceiver_->processRecv(false))
			return 0;
	}

	return 0;
}

//-------------------------------------------------------------------------------------
}
}
