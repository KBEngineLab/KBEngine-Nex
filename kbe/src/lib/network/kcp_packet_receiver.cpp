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

#include "kcp_packet_receiver.h"
#ifndef CODE_INLINE
#include "kcp_packet_receiver.inl"
#endif

#include "network/address.h"
#include "network/bundle.h"
#include "network/channel.h"
#include "network/endpoint.h"
#include "network/event_dispatcher.h"
#include "network/network_interface.h"
#include "network/event_poller.h"
#include "network/error_reporter.h"
#include <limits>

namespace KBEngine {
namespace Network
{
namespace
{
inline int toIntSize(size_t v)
{
	KBE_ASSERT(v <= static_cast<size_t>(std::numeric_limits<int>::max()));
	return static_cast<int>(v);
}

inline long toLongSize(size_t v)
{
	KBE_ASSERT(v <= static_cast<size_t>(std::numeric_limits<long>::max()));
	return static_cast<long>(v);
}
}

//-------------------------------------------------------------------------------------
static ObjectPool<KCPPacketReceiver> _g_objPool("KCPPacketReceiver");
ObjectPool<KCPPacketReceiver>& KCPPacketReceiver::ObjPool()
{
	return _g_objPool;
}

//-------------------------------------------------------------------------------------
KCPPacketReceiver* KCPPacketReceiver::createPoolObject(const std::string& logPoint)
{
	return _g_objPool.createObject(logPoint);
}

//-------------------------------------------------------------------------------------
void KCPPacketReceiver::reclaimPoolObject(KCPPacketReceiver* obj)
{
	_g_objPool.reclaimObject(obj);
}

//-------------------------------------------------------------------------------------
void KCPPacketReceiver::destroyObjPool()
{
	DEBUG_MSG(fmt::format("KCPPacketReceiver::destroyObjPool(): size {}.\n",
		_g_objPool.size()));

	_g_objPool.destroy();
}

//-------------------------------------------------------------------------------------
KCPPacketReceiver::SmartPoolObjectPtr KCPPacketReceiver::createSmartPoolObj(const std::string& logPoint)
{
	return SmartPoolObjectPtr(new SmartPoolObject<KCPPacketReceiver>(ObjPool().createObject(logPoint), _g_objPool));
}

//-------------------------------------------------------------------------------------
KCPPacketReceiver::KCPPacketReceiver(EndPoint & endpoint,
	   NetworkInterface & networkInterface	) :
	UDPPacketReceiver(endpoint, networkInterface)
{
}

//-------------------------------------------------------------------------------------
KCPPacketReceiver::~KCPPacketReceiver()
{
}

//-------------------------------------------------------------------------------------
bool KCPPacketReceiver::processRecv(bool expectingPacket)
{
	return UDPPacketReceiver::processRecv(expectingPacket);
}

//-------------------------------------------------------------------------------------
bool KCPPacketReceiver::processRecv(UDPPacket* pReceiveWindow)
{
	Channel* pChannel = getChannel();
	if (pChannel == NULL || pChannel->isDestroyed() || pChannel->condemn() > 0)
	{
		UDPPacket::reclaimPoolObject(pReceiveWindow);
		return false;
	}

	Reason ret = this->processPacket(pChannel, pReceiveWindow);

	if (ret != REASON_SUCCESS)
		this->dispatcher().errorReporter().reportException(ret, pEndpoint_->addr());

	return true;
}

//-------------------------------------------------------------------------------------
Reason KCPPacketReceiver::processPacket(Channel* pChannel, Packet * pPacket)
{
	if (pChannel != NULL && pChannel->hasHandshake())
	{
		if (pChannel->handshake(pPacket))
		{
			RECLAIM_PACKET(pPacket->isTCPPacket(), pPacket);
			return REASON_SUCCESS;
		}

		// KCP ACK 与窗口更新本身就是有效的对端活动，即使尚未重组出业务消息也必须刷新超时基准。
		// KCP ACKs and window updates are valid peer activity, so refresh the timeout baseline even before an application message is reassembled.
		pChannel->updateLastReceivedTime();
		pChannel->scheduleKcpUpdate();

		const size_t packetLength = pPacket->length();
		const int inputResult = ikcp_input(pChannel->pKCP(), (const char*)pPacket->data(), toLongSize(packetLength));
		if (inputResult < 0)
		{
			const uint8* data = reinterpret_cast<const uint8*>(pPacket->data());
			const uint32 packetConversation = packetLength >= sizeof(uint32)
				? static_cast<uint32>(data[0]) |
				  (static_cast<uint32>(data[1]) << 8) |
				  (static_cast<uint32>(data[2]) << 16) |
				  (static_cast<uint32>(data[3]) << 24)
				: 0;
			const uint64 errorCount = pChannel->networkInterface().recordKcpInputError(inputResult, packetLength);
			// 指数退避保留首个和长期异常证据，同时避免畸形报文风暴通过 Logger 放大 CPU 与 IO 压力。
			// Exponential backoff preserves first and persistent-failure evidence without amplifying malformed traffic through Logger CPU and IO.
			if ((errorCount & (errorCount - 1)) == 0)
			{
				WARNING_MSG(fmt::format(
					"KCPPacketReceiver::processPacket: invalid input, result={}, packetLength={}, expectedConv={}, packetConv={}, addr={}, sessionEpoch={}, total={}\n",
					inputResult, packetLength, static_cast<uint32>(pChannel->pKCP()->conv), packetConversation,
					pChannel->c_str(), pChannel->sessionEpoch(), errorCount));
			}
			RECLAIM_PACKET(pPacket->isTCPPacket(), pPacket);
			return REASON_CHANNEL_LOST;
		}
		pChannel->scheduleKcpAck();

		RECLAIM_PACKET(pPacket->isTCPPacket(), pPacket);

		while (true)
		{
			const int messageSize = ikcp_peeksize(pChannel->pKCP());
			if (messageSize < 0)
				return REASON_SUCCESS;

			Packet* pRcvdUDPPacket = UDPPacket::createPoolObject(OBJECTPOOL_POINT);
			// KCP 会重组跨数据报消息，完整消息可能大于单个 UDP MTU，必须按 peeksize 扩容后再读取。
			// KCP reassembles messages across datagrams, so a complete message may exceed one UDP MTU and must be resized from peeksize before reading.
			if (messageSize > toIntSize(pRcvdUDPPacket->size()))
				pRcvdUDPPacket->data_resize(static_cast<size_t>(messageSize));

			int bytes_recvd = ikcp_recv(pChannel->pKCP(), (char*)pRcvdUDPPacket->data(), toIntSize(pRcvdUDPPacket->size()));
			if (bytes_recvd < 0)
			{
				//WARNING_MSG(fmt::format("KCPPacketReceiver::processPacket(): recvd_bytes({}) <= 0! addr={}\n", bytes_recvd, pChannel->c_str()));
				RECLAIM_PACKET(pRcvdUDPPacket->isTCPPacket(), pRcvdUDPPacket);
				return REASON_SUCCESS;
			}
			else
			{
				// KCP 返回值等于 peeksize 和缓冲容量时表示消息恰好装满，并非越界；只有大于容量才是实现异常。
				// A KCP result equal to peeksize and buffer capacity is an exact fit, not an overflow; only a larger result is invalid.
				if (bytes_recvd > toIntSize(pRcvdUDPPacket->size()))
				{
					ERROR_MSG(fmt::format("KCPPacketReceiver::processPacket(): recvd_bytes({}) > maxBuf({})! addr={}\n", bytes_recvd, pRcvdUDPPacket->size(), pChannel->c_str()));
				}

				pRcvdUDPPacket->wpos(bytes_recvd);

				Reason r = PacketReceiver::processPacket(pChannel, pRcvdUDPPacket);
				if (r != REASON_SUCCESS)
				{
					RECLAIM_PACKET(pRcvdUDPPacket->isTCPPacket(), pRcvdUDPPacket);
					return r;
				}
			}
		}
	}
	else
	{
		return PacketReceiver::processPacket(pChannel, pPacket);
	}

	return REASON_SUCCESS;
}

//-------------------------------------------------------------------------------------
}
}
