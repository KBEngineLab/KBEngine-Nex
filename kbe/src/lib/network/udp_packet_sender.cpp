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
namespace
{
// completion 队列短时拥塞不代表客户端失联；仅在五秒完全无发送进展后才关闭外部连接。
// A short completion-queue burst does not mean the client is gone; close an external peer only after five seconds without send progress.
const uint64 UDP_SEND_BACKPRESSURE_TIMEOUT_STAMPS = 5 * stampsPerSecond();
}

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
	sendBackpressure_ = UdpSendBackpressure();
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
	sendBackpressure_()
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
	bool noticed = pChannel == NULL;

	// Output notification callbacks do not carry a Channel. Resolve it by endpoint
	// just like TCPPacketSender; if the channel has already gone away, report and
	// return instead of asserting inside the network thread.
	// 输出通知回调不会携带 Channel，需和 TCPPacketSender 一样通过 endpoint 找回。
	// 如果连接已经被销毁，只记录错误并返回，避免网络线程因生命周期竞态断言退出。
	if(noticed)
		pChannel = getChannel();

	if (pChannel == NULL)
	{
		ERROR_MSG(fmt::format("UDPPacketSender::processSend: channel not found, endpoint={}\n",
			pEndpoint_ != NULL ? pEndpoint_->addr().c_str() : "null"));
		return false;
	}

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
			{
				onSent(pPacket);
				sendBackpressure_.recordProgress();
			}
		}

		if (reason == REASON_SUCCESS)
		{
			pakcets.clear();
			Network::Bundle::reclaimPoolObject((*iter));
		}
		else
		{
			pakcets.erase(pakcets.begin(), iter1);
			bundles.erase(bundles.begin(), iter);

			if (reason == REASON_RESOURCE_UNAVAILABLE)
			{
				const bool backpressureExpired = sendBackpressure_.recordBlocked(
					timestamp(), UDP_SEND_BACKPRESSURE_TIMEOUT_STAMPS);
				if (pChannel->isExternal() && closeOnSustainedBackpressure())
				{
					if (backpressureExpired)
					{
						WARNING_MSG(fmt::format("UDPPacketSender::processSend: closing external udp/kcp channel after sustained send backpressure, rejections={}, addr={}\n",
							sendBackpressure_.rejectionCount(), pEndpoint_->addr().c_str()));

						onGetError(pChannel, "UDPPacketSender::processSend: sustained send backpressure");
					}
				}
				else if (pChannel->isInternal())
				{
					// 资源不足是可恢复状态；记录错误会再次走内部网络，形成背压反馈环。
					// Resource exhaustion is recoverable; reporting it over the internal network would create a backpressure feedback loop.
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

	if(noticed)
		pChannel->onSendCompleted();

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
