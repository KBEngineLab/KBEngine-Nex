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

#include "kcp_packet_sender_ex.h"
#include "clientobject.h"
#include "network/common.h"

namespace KBEngine
{
namespace Network
{

KCPPacketSenderEx::KCPPacketSenderEx(EndPoint& endpoint,
	NetworkInterface& networkInterface, ClientObject* pClientObject) :
	KCPPacketSender(endpoint, networkInterface),
	pClientObject_(pClientObject)
{
}

KCPPacketSenderEx::~KCPPacketSenderEx() = default;

Channel* KCPPacketSenderEx::getChannel()
{
	return pClientObject_->pServerChannel();
}

void KCPPacketSenderEx::onGetError(Channel* pChannel, const std::string& err)
{
	pClientObject_->onNetworkError(err);
}

void KCPPacketSenderEx::onSent(Packet* pPacket)
{
	// KCP output 创建独立 UDP packet，发送完成后必须归还正确的对象池。
	// KCP output creates an independent UDP packet that must return to the correct pool after completion.
	RECLAIM_PACKET(pPacket->isTCPPacket(), pPacket);
}

}
}
