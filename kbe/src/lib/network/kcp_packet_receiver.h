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
#ifndef KBE_NETWORKKCPPACKET_RECEIVER_H
#define KBE_NETWORKKCPPACKET_RECEIVER_H

#include "network/udp_packet_receiver.h"

namespace KBEngine {
namespace Network
{

class KCPPacketReceiver : public UDPPacketReceiver
{
public:
	typedef KBEShared_ptr< SmartPoolObject< KCPPacketReceiver > > SmartPoolObjectPtr;
	static SmartPoolObjectPtr createSmartPoolObj(const std::string& logPoint);
	static ObjectPool<KCPPacketReceiver>& ObjPool();
	static KCPPacketReceiver* createPoolObject(const std::string& logPoint);
	static void reclaimPoolObject(KCPPacketReceiver* obj);
	static void destroyObjPool();

	KCPPacketReceiver():UDPPacketReceiver(){}
	KCPPacketReceiver(EndPoint & endpoint, NetworkInterface & networkInterface);
	virtual ~KCPPacketReceiver();

	bool processRecv(UDPPacket* pReceiveWindow) override;
	virtual bool processRecv(bool expectingPacket);

	virtual Reason processPacket(Channel* pChannel, Packet * pPacket);

	virtual ProtocolSubType protocolSubType() const {
		return SUB_PROTOCOL_KCP;
	}

protected:

};

}
}

#ifdef CODE_INLINE
#include "kcp_packet_receiver.inl"
#endif
#endif // KBE_NETWORKKCPPACKET_RECEIVER_H
