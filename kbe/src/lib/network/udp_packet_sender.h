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

#ifndef KBE_NETWORKUDPPACKET_SENDER_H
#define KBE_NETWORKUDPPACKET_SENDER_H

#include "common/common.h"
#include "common/timer.h"
#include "common/objectpool.h"
#include "helper/debug_helper.h"
#include "network/common.h"
#include "network/interfaces.h"
#include "network/tcp_packet.h"
#include "network/packet_sender.h"
#include "network/udp_send_backpressure.h"

namespace KBEngine {
namespace Network
{
class EndPoint;
class Channel;
class Address;
class NetworkInterface;
class EventDispatcher;

class UDPPacketSender : public PacketSender
{
public:
	typedef KBEShared_ptr< SmartPoolObject< UDPPacketSender > > SmartPoolObjectPtr;
	static SmartPoolObjectPtr createSmartPoolObj(const std::string& logPoint);
	static ObjectPool<UDPPacketSender>& ObjPool();
	static UDPPacketSender* createPoolObject(const std::string& logPoint);
	static void reclaimPoolObject(UDPPacketSender* obj);
	virtual void onReclaimObject();
	static void destroyObjPool();

	UDPPacketSender():PacketSender(){}
	UDPPacketSender(EndPoint & endpoint, NetworkInterface & networkInterface);
	virtual ~UDPPacketSender();

	virtual bool processSend(Channel* pChannel, int userarg);

	virtual PacketSender::PACKET_SENDER_TYPE type() const
	{
		return UDP_PACKET_SENDER;
	}

protected:
	virtual void onGetError(Channel* pChannel, const std::string& err);
	virtual void onSent(Packet* pPacket);
	virtual Reason processFilterPacket(Channel* pChannel, Packet * pPacket, int userarg);
	// 普通 UDP 没有可靠重传层，长期无法发送时仍允许关闭；KCP 由自身窗口和重传机制负责恢复，不能因暂时背压断开。
	// Plain UDP has no reliable retransmission layer and may still close after a prolonged stall; KCP must rely on its own window and retransmission instead of disconnecting on temporary backpressure.
	virtual bool closeOnSustainedBackpressure() const { return true; }

protected:
	UdpSendBackpressure sendBackpressure_;

};
}
}

#ifdef CODE_INLINE
#include "udp_packet_sender.inl"
#endif
#endif // KBE_NETWORKUDPPACKET_SENDER_H
