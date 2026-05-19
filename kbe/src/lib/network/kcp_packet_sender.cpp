// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com


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
		//DEBUG_MSG(fmt::format("KCPPacketSender::processFilterPacket: kcp_sent={}, kcp={:p}, channel={:p}, this={:p}\n", 
		//	pPacket->length(), (void*)pChannel->pKCP(), (void*)pChannel, (void*)this));

		pChannel->addKcpUpdate();


		if (ikcp_waitsnd(pChannel->pKCP()) > (int)(pChannel->pKCP()->snd_wnd * 2)/* 发送队列超出发送窗口2倍则提示资源不足 */ || 
			ikcp_send(pChannel->pKCP(), (const char*)pPacket->data(), toIntSize(pPacket->length())) < 0)
		{
			if (pChannel->isInternal())
			{
				ERROR_MSG(fmt::format("KCPPacketSender::ikcp_send: send error! currPacketSize={}, ikcp_waitsnd={}, snd_wndsize={}\n", 
					pPacket->length(), ikcp_waitsnd(pChannel->pKCP()), pChannel->pKCP()->snd_wnd));
			}

			return REASON_RESOURCE_UNAVAILABLE;
		}

		pPacket->sentSize += toUint32Size(pPacket->length());
	}
	else
	{
		EndPoint* pEndpoint = pChannel->pEndPoint();
		EventPoller* pPoller = pChannel->networkInterface().dispatcher().pPoller();
		if (pPoller != NULL && pPoller->supportsCompletion())
		{
			// KCP 最终仍然落到 UDP socket。completion 模式下统一交给 poller UDP_SEND，
			// 保证 TCP/UDP/KCP 都是 completion 驱动，避免同步 sendto 卡住主线程。
			int sendSize = toIntSize(pPacket->length());
			if (!pPoller->queueUdpSend(static_cast<int>(*pEndpoint), pPacket->data(), sendSize, pEndpoint->addr()))
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
			// 如果只发送了一部分数据，则发送出错了
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
		// ikcp_output 可能在一次 tick 中被多次调用，queueUdpSend 会按调用顺序入队，
		// 每次只挂一个 sendto completion，completion 后继续发送下一包。
		if (pPoller->queueUdpSend(static_cast<int>(*pEndpoint), buf, len, pEndpoint->addr()))
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

