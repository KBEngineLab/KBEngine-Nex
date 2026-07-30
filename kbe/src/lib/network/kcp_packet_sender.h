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

#ifndef KBE_NETWORKKCPPACKET_SENDER_H
#define KBE_NETWORKKCPPACKET_SENDER_H

#include "common/common.h"
#include "common/timer.h"
#include "common/objectpool.h"
#include "helper/debug_helper.h"
#include "network/common.h"
#include "network/interfaces.h"
#include "network/tcp_packet.h"
#include "network/udp_packet_sender.h"
#include "network/ikcp.h"

namespace KBEngine {
namespace Network
{

class KCPPacketSender : public UDPPacketSender
{
public:
	typedef KBEShared_ptr< SmartPoolObject< KCPPacketSender > > SmartPoolObjectPtr;
	static SmartPoolObjectPtr createSmartPoolObj(const std::string& logPoint);
	static ObjectPool<KCPPacketSender>& ObjPool();
	static KCPPacketSender* createPoolObject(const std::string& logPoint);
	static void reclaimPoolObject(KCPPacketSender* obj);
	virtual void onReclaimObject();
	static void destroyObjPool();

	KCPPacketSender():UDPPacketSender(){}
	KCPPacketSender(EndPoint & endpoint, NetworkInterface & networkInterface);
	virtual ~KCPPacketSender();

	int kcp_output(const char *buf, int len, ikcpcb *kcp, Channel* pChannel);

protected:
	virtual void onSent(Packet* pPacket);
	virtual Reason processFilterPacket(Channel* pChannel, Packet * pPacket, int userarg);
	// KCP 背压表示发送窗口或底层队列暂时饱和，必须保留 Channel 并等待后续重试。
	// KCP backpressure means the send window or lower queue is temporarily full; keep the Channel and retry later.
	virtual bool closeOnSustainedBackpressure() const { return false; }

};
}
}

#ifdef CODE_INLINE
#include "udp_packet_sender.inl"
#endif
#endif // KBE_NETWORKKCPPACKET_SENDER_H
