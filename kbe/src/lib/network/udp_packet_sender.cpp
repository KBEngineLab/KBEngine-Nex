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

#include "udp_packet_sender.h"
#ifndef CODE_INLINE
#include "udp_packet_sender.inl"
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

namespace KBEngine {
namespace Network
{

//-------------------------------------------------------------------------------------
static ObjectPool<UDPPacketSender> _g_objPool("UDPPacketSender");
ObjectPool<UDPPacketSender>& UDPPacketSender::ObjPool()
{
	return _g_objPool;
}

//-------------------------------------------------------------------------------------
UDPPacketSender* UDPPacketSender::createPoolObject(const std::string& logPoint)
{
	return _g_objPool.createObject(logPoint);
}

//-------------------------------------------------------------------------------------
void UDPPacketSender::reclaimPoolObject(UDPPacketSender* obj)
{
	_g_objPool.reclaimObject(obj);
}

//-------------------------------------------------------------------------------------
void UDPPacketSender::onReclaimObject()
{
	sendfailCount_ = 0;
}

//-------------------------------------------------------------------------------------
void UDPPacketSender::destroyObjPool()
{
	DEBUG_MSG(fmt::format("UDPPacketSender::destroyObjPool(): size {}.\n",
		_g_objPool.size()));

	_g_objPool.destroy();
}

//-------------------------------------------------------------------------------------
UDPPacketSender::SmartPoolObjectPtr UDPPacketSender::createSmartPoolObj(const std::string& logPoint)
{
	return SmartPoolObjectPtr(new SmartPoolObject<UDPPacketSender>(ObjPool().createObject(logPoint), _g_objPool));
}

//-------------------------------------------------------------------------------------
UDPPacketSender::UDPPacketSender(EndPoint & endpoint,
	   NetworkInterface & networkInterface	) :
	PacketSender(endpoint, networkInterface),
	sendfailCount_(0)
{
}

//-------------------------------------------------------------------------------------
UDPPacketSender::~UDPPacketSender()
{
	//DEBUG_MSG("UDPPacketSender::~UDPPacketSender()\n");
}

//-------------------------------------------------------------------------------------
void UDPPacketSender::onGetError(Channel* pChannel, const std::string& err)
{
	pChannel->condemn(err);
	// 延迟到 dispatcher 的正常 Channel 清理阶段销毁，避免破坏正在遍历的接收队列。
	// Defer destruction to the dispatcher's normal Channel cleanup phase so an active receive iteration is not invalidated.
}

//-------------------------------------------------------------------------------------
void UDPPacketSender::onSent(Packet* pPacket)
{
	RECLAIM_PACKET(pPacket->isTCPPacket(), pPacket);
}

//-------------------------------------------------------------------------------------
bool UDPPacketSender::processSend(Channel* pChannel, int userarg)
{
	KBE_ASSERT(pChannel != NULL);

	if (pChannel->condemn() == Channel::FLAG_CONDEMN_AND_DESTROY)
	{
		return false;
	}

	Channel::Bundles& bundles = pChannel->bundles();
	Reason reason = REASON_SUCCESS;

	Channel::Bundles::iterator iter = bundles.begin();
	for (; iter != bundles.end(); ++iter)
	{
		Bundle::Packets& pakcets = (*iter)->packets();
		Bundle::Packets::iterator iter1 = pakcets.begin();
		for (; iter1 != pakcets.end(); ++iter1)
		{
			Packet* pPacket = (*iter1);
			reason = processPacket(pChannel, pPacket, userarg);
			if (reason != REASON_SUCCESS)
				break;
			else
				onSent(pPacket);
		}

		if (reason == REASON_SUCCESS)
		{
			pakcets.clear();
			Network::Bundle::reclaimPoolObject((*iter));
			sendfailCount_ = 0;
		}
		else
		{
			pakcets.erase(pakcets.begin(), iter1);
			bundles.erase(bundles.begin(), iter);

			if (reason == REASON_RESOURCE_UNAVAILABLE)
			{
				// 外部连接采用有界重试，防止失联客户端永久占用队列；内部连接保留错误报告供运维定位。
				// External peers use bounded retries to avoid retaining queues forever; internal peers keep error reports for operations diagnosis.
				++sendfailCount_;
				if (pChannel->isExternal())
				{
					if (sendfailCount_ >= 10)
					{
						WARNING_MSG(fmt::format("UDPPacketSender::processSend: closing external udp/kcp channel after {} send retries, addr={}\n",
							(int)sendfailCount_, pEndpoint_->addr().c_str()));

						onGetError(pChannel, "UDPPacketSender::processSend: sendfailCount >= 10");
					}
				}
				else
				{
					this->dispatcher().errorReporter().reportException(reason, pEndpoint_->addr(),
						fmt::format("UDPPacketSender::processSend(internal, {})", (int)sendfailCount_).c_str());
				}
			}
			else
			{
				if (pChannel->isExternal())
				{
#if KBE_PLATFORM_UNIX_FAMILY
					this->dispatcher().errorReporter().reportException(reason, pEndpoint_->addr(), "UDPPacketSender::processSend(external)",
						fmt::format(", errno: {}", errno).c_str());
#else
					this->dispatcher().errorReporter().reportException(reason, pEndpoint_->addr(), "UDPPacketSender::processSend(external)",
						fmt::format(", errno: {}", WSAGetLastError()).c_str());
#endif
			}
				else
				{
#if KBE_PLATFORM_UNIX_FAMILY
					this->dispatcher().errorReporter().reportException(reason, pEndpoint_->addr(), "UDPPacketSender::processSend(internal)",
						fmt::format(", errno: {}, {}", errno, pChannel->c_str()).c_str());
#else
					this->dispatcher().errorReporter().reportException(reason, pEndpoint_->addr(), "UDPPacketSender::processSend(internal)",
						fmt::format(", errno: {}, {}", WSAGetLastError(), pChannel->c_str()).c_str());
#endif
				}

				onGetError(pChannel, fmt::format("UDPPacketSender::processSend: errno={}", kbe_lasterror()));
			}

			return false;
		}
	}

	bundles.clear();

	return true;
}

//-------------------------------------------------------------------------------------
Reason UDPPacketSender::processFilterPacket(Channel* pChannel, Packet * pPacket, int userarg)
{
	if (pChannel->condemn() == Channel::FLAG_CONDEMN_AND_DESTROY)
	{
		return REASON_CHANNEL_CONDEMN;
	}

	EndPoint* pEndpoint = pChannel->pEndPoint();
	EventPoller* pPoller = pChannel->networkInterface().dispatcher().pPoller();
	const int packetLength = static_cast<int>(pPacket->length());
	if (pPoller && pPoller->supportsCompletion())
	{
		// completion 后端统一拥有 UDP 发送队列，保证一个 socket 同时只有一个挂起的 sendto。
		// The completion backend owns the UDP send queue so one socket has at most one pending sendto operation.
		if (!pPoller->queueUdpSend(static_cast<KBESOCKET>(*pEndpoint), pPacket->data(), packetLength, pEndpoint->addr()))
			return checkSocketErrors(pEndpoint);

		pPacket->sentSize += static_cast<uint32>(packetLength);
		pChannel->onPacketSent(packetLength, true);
		return REASON_SUCCESS;
	}

	const int sent = pEndpoint->sendto(pPacket->data(), packetLength);
	const bool completed = sent == packetLength;
	if (sent > 0)
		pPacket->sentSize += static_cast<uint32>(sent);
	pChannel->onPacketSent(sent, completed);
	if (!completed)
		return checkSocketErrors(pEndpoint);

	return REASON_SUCCESS;
}

//-------------------------------------------------------------------------------------
}
}
