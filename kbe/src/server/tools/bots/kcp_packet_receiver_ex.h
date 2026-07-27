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

#ifndef KBE_NETWORKKCPPACKET_RECEIVER_EX_H
#define KBE_NETWORKKCPPACKET_RECEIVER_EX_H

#include "network/kcp_packet_receiver.h"

namespace KBEngine
{
class ClientObject;

namespace Network
{

// Bots 复用一个外部 Channel，接收器必须直接返回所属 ClientObject 的服务端 Channel。
// Bots reuse one external Channel, so the receiver must resolve the server Channel owned by its ClientObject.
class KCPPacketReceiverEx : public KCPPacketReceiver
{
public:
	KCPPacketReceiverEx(EndPoint& endpoint, NetworkInterface& networkInterface, ClientObject* pClientObject);
	~KCPPacketReceiverEx() override;

	Channel* getChannel() override;
	Channel* findChannel(const Address& addr) override;

	// ClientObject 拥有接收器及其 Channel，该反向引用只在 Bots 主线程内使用。
	// ClientObject owns the receiver and its Channel; this back-reference is used only on the Bots main thread.
protected:
	ClientObject* pClientObject_;
};

}
}

#endif // KBE_NETWORKKCPPACKET_RECEIVER_EX_H
