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

#include "kcp_packet_receiver_ex.h"
#include "clientobject.h"
#include "network/channel.h"

namespace KBEngine
{
namespace Network
{

KCPPacketReceiverEx::KCPPacketReceiverEx(EndPoint& endpoint,
	NetworkInterface& networkInterface, ClientObject* pClientObject) :
	KCPPacketReceiver(endpoint, networkInterface),
	pClientObject_(pClientObject)
{
}

KCPPacketReceiverEx::~KCPPacketReceiverEx() = default;

Channel* KCPPacketReceiverEx::getChannel()
{
	return pClientObject_->pServerChannel();
}

Channel* KCPPacketReceiverEx::findChannel(const Address& addr)
{
	return pClientObject_->pServerChannel();
}

Reason KCPPacketReceiverEx::processPacket(Channel* pChannel, Packet* pPacket)
{
	uint32 channelID = 0;
	if (pChannel != NULL && pChannel->hasHandshake() &&
		ClientObject::parseKcpHelloAck(reinterpret_cast<const char*>(pPacket->data() + pPacket->rpos()), pPacket->length(), channelID) &&
		channelID == static_cast<uint32>(pChannel->id()))
	{
		// 服务端会为客户端重试的 hello 幂等重发 ACK；激活 KCP 后排队到达的同一 ACK 只是握手控制报文。
		// The server idempotently ACKs retried hellos; the same queued ACK arriving after KCP activation remains handshake control traffic.
		pChannel->updateLastReceivedTime();
		RECLAIM_PACKET(pPacket->isTCPPacket(), pPacket);
		return REASON_SUCCESS;
	}

	return KCPPacketReceiver::processPacket(pChannel, pPacket);
}

}
}
