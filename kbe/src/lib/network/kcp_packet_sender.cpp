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

#include "kcp_packet_sender.h"
#ifndef CODE_INLINE
#include "kcp_packet_sender.inl"
#endif

#include "network/address.h"
#include "network/bundle.h"
#include "network/channel.h"
#include "network/endpoint.h"
#include "network/event_dispatcher.h"
#include "network/network_interface.h"
#include "network/event_poller.h"
#include "network/error_reporter.h"
#include "network/tcp_packet.h"
#include "network/udp_packet.h"
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

inline uint32 toUint32Size(size_t v)
{
	KBE_ASSERT(v <= static_cast<size_t>(std::numeric_limits<uint32>::max()));
	return static_cast<uint32>(v);
}
}

//-------------------------------------------------------------------------------------
static ObjectPool<KCPPacketSender> _g_objPool("KCPPacketSender");
ObjectPool<KCPPacketSender>& KCPPacketSender::ObjPool()
{
	return _g_objPool;
}

//-------------------------------------------------------------------------------------
KCPPacketSender* KCPPacketSender::createPoolObject(const std::string& logPoint)
{
	return _g_objPool.createObject(logPoint);
}

//-------------------------------------------------------------------------------------
void KCPPacketSender::reclaimPoolObject(KCPPacketSender* obj)
{
	_g_objPool.reclaimObject(obj);
}

//-------------------------------------------------------------------------------------
void KCPPacketSender::onReclaimObject()
{
	UDPPacketSender::onReclaimObject();
}

//-------------------------------------------------------------------------------------
void KCPPacketSender::destroyObjPool()
{
	DEBUG_MSG(fmt::format("KCPPacketSender::destroyObjPool(): size {}.\n",
		_g_objPool.size()));

	_g_objPool.destroy();
}

//-------------------------------------------------------------------------------------
KCPPacketSender::SmartPoolObjectPtr KCPPacketSender::createSmartPoolObj(const std::string& logPoint)
{
	return SmartPoolObjectPtr(new SmartPoolObject<KCPPacketSender>(ObjPool().createObject(logPoint), _g_objPool));
}

//-------------------------------------------------------------------------------------
KCPPacketSender::KCPPacketSender(EndPoint & endpoint,
	   NetworkInterface & networkInterface	) :
	UDPPacketSender(endpoint, networkInterface)
{
}

//-------------------------------------------------------------------------------------
KCPPacketSender::~KCPPacketSender()
{
	//DEBUG_MSG("KCPPacketSender::~KCPPacketSender()\n");
}

//-------------------------------------------------------------------------------------
Reason KCPPacketSender::processFilterPacket(Channel* pChannel, Packet * pPacket, int userarg)
{
	if (pChannel->condemn() == Channel::FLAG_CONDEMN_AND_DESTROY)
	{
		return REASON_CHANNEL_CONDEMN;
	}

	if (userarg > 0)
	{
		// 队列上限是可恢复背压状态，调用方会保留 Bundle 重试；不要在热路径逐次写 ERROR 放大阻塞。
		// The queue cap is recoverable backpressure and callers retain the Bundle for retry; avoid per-attempt ERROR amplification.
		if (ikcp_waitsnd(pChannel->pKCP()) > (int)(pChannel->pKCP()->snd_wnd * 2))
			return REASON_RESOURCE_UNAVAILABLE;

		const int sendResult = ikcp_send(
			pChannel->pKCP(), (const char*)pPacket->data(), toIntSize(pPacket->length()));
		if (sendResult < 0)
		{
			ERROR_MSG(fmt::format("KCPPacketSender::ikcp_send: rejected packet, result={}, currPacketSize={}, ikcp_waitsnd={}, snd_wndsize={}\n",
				sendResult, pPacket->length(), ikcp_waitsnd(pChannel->pKCP()), pChannel->pKCP()->snd_wnd));

			return REASON_RESOURCE_UNAVAILABLE;
		}

		// Only successful admissions need an immediate wakeup. Pending KCP data already
		// owns a scheduler entry, so retries rejected by the cap must not reschedule it.
		// 只有成功入队才需要立即唤醒；已有 KCP 数据已持有调度项，队列上限拒绝的重试不得重复调度。
		pChannel->scheduleKcpUpdate();
		pPacket->sentSize += toUint32Size(pPacket->length());
	}
	else
	{
		EndPoint* pEndpoint = pChannel->pEndPoint();
		EventPoller* pPoller = pChannel->networkInterface().dispatcher().pPoller();
		if (pPoller != NULL && pPoller->supportsCompletion())
		{
			// KCP 最终仍然落到 UDP socket。completion 模式下交给 poller UDP_SEND，避免同步 sendto 阻塞主线程。
			// KCP ultimately uses a UDP socket; completion mode delegates to poller UDP_SEND so synchronous sendto cannot block the main thread.
			int sendSize = toIntSize(pPacket->length());
			if (!pPoller->queueUdpSend(static_cast<KBESOCKET>(*pEndpoint), pPacket->data(), sendSize, pEndpoint->addr()))
			{
				return checkSocketErrors(pEndpoint);
			}

			pPacket->sentSize += toUint32Size(pPacket->length());
			pChannel->onPacketSent(sendSize, true);
			return REASON_SUCCESS;
		}
		int retlen = pEndpoint->sendto((void*)(pPacket->data()), toIntSize(pPacket->length()));
		bool sentCompleted = (retlen == (int)pPacket->length());

		if (retlen > 0)
		{
			pPacket->sentSize += retlen;
			//DEBUG_MSG(fmt::format("KCPPacketSender::processFilterPacket: sent={}, sentTotalSize={}.\n", retlen, pPacket->sentSize));
		}

		pChannel->onPacketSent(retlen, sentCompleted);

		if (!sentCompleted)
		{
			// UDP 数据报必须原子发送，部分发送同样视为失败。
			// A UDP datagram must be sent atomically, so a partial send is also a failure.
			return checkSocketErrors(pEndpoint);
		}
	}

	return REASON_SUCCESS;
}

//-------------------------------------------------------------------------------------
void KCPPacketSender::onSent(Packet* pPacket)
{
	RECLAIM_PACKET(pPacket->isTCPPacket(), pPacket);
}

//-------------------------------------------------------------------------------------
int KCPPacketSender::kcp_output(const char *buf, int len, ikcpcb *kcp, Channel* pChannel)
{
	//KBE_ASSERT(kcp == pChannel->pKCP());

	EndPoint* pEndpoint = pChannel->pEndPoint();
	EventPoller* pPoller = pChannel->networkInterface().dispatcher().pPoller();
	if (pPoller != NULL && pPoller->supportsCompletion())
	{
		// UDP socket 已设为非阻塞；无 pending completion 时直接复制一个 datagram 进内核。
		// 一旦存在 overlapped 发送，后续报文必须沿用同一队列，避免同步 sendto 越过未完成报文而扩大乱序窗口。
		// UDP sockets are nonblocking; a normal sendto only copies one datagram into the kernel and never waits for wire transmission.
		// Once an overlapped send is pending, later datagrams stay on that queue so synchronous sendto cannot widen reordering by bypassing it.
		if (!pPoller->hasPendingSend(*pEndpoint))
		{
			const int directResult = pEndpoint->sendto(const_cast<char*>(buf), len);
			if (directResult == len)
			{
				pChannel->onPacketSent(len, true);
				return 0;
			}
		}

		if (pPoller->queueUdpSend(static_cast<KBESOCKET>(*pEndpoint), buf, len, pEndpoint->addr()))
		{
			pChannel->onPacketSent(len, true);
			return 0;
		}

		return -1;
	}
	int retlen = pEndpoint->sendto((void*)buf, len);

	bool sentCompleted = retlen == len;
	pChannel->onPacketSent(retlen, sentCompleted);

	//DEBUG_MSG(fmt::format("KCPPacketSender::kcp_output: kcp={:p}, pChannel={:p} sent={}\n", (void*)kcp, (void*)pChannel, len));
	return sentCompleted ? 0 : -1;
}

//-------------------------------------------------------------------------------------
}
}
