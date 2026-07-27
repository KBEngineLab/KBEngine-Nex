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

#ifndef KBE_NETWORKKCPPACKET_SENDER_EX_H
#define KBE_NETWORKKCPPACKET_SENDER_EX_H

#include "network/kcp_packet_sender.h"

namespace KBEngine
{
class ClientObject;

namespace Network
{

// 发送器把 KCP 错误和数据报所有权接回 ClientObject 生命周期，避免通用 sender 销毁 Bots。
// The sender routes KCP errors and datagram ownership through ClientObject lifecycle management.
class KCPPacketSenderEx : public KCPPacketSender
{
public:
	KCPPacketSenderEx(EndPoint& endpoint, NetworkInterface& networkInterface, ClientObject* pClientObject);
	~KCPPacketSenderEx() override;

	Channel* getChannel() override;

protected:
	void onGetError(Channel* pChannel, const std::string& err) override;
	void onSent(Packet* pPacket) override;

	ClientObject* pClientObject_;
};

}
}

#endif // KBE_NETWORKKCPPACKET_SENDER_EX_H
