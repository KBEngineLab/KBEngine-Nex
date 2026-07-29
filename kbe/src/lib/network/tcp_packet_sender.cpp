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


#include "tcp_packet_sender.h"
#ifndef CODE_INLINE
#include "tcp_packet_sender.inl"
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
#include <limits>
#include "network/udp_packet.h"

namespace KBEngine { 
namespace Network
{

//-------------------------------------------------------------------------------------
static ObjectPool<TCPPacketSender> _g_objPool("TCPPacketSender");
ObjectPool<TCPPacketSender>& TCPPacketSender::ObjPool()
{
	return _g_objPool;
}

//-------------------------------------------------------------------------------------
TCPPacketSender* TCPPacketSender::createPoolObject(const std::string& logPoint)
{
	return _g_objPool.createObject(logPoint);
}

//-------------------------------------------------------------------------------------
void TCPPacketSender::reclaimPoolObject(TCPPacketSender* obj)
{
	_g_objPool.reclaimObject(obj);
}

//-------------------------------------------------------------------------------------
void TCPPacketSender::onReclaimObject()
{
	sendfailCount_ = 0;
}

//-------------------------------------------------------------------------------------
void TCPPacketSender::destroyObjPool()
{
	DEBUG_MSG(fmt::format("TCPPacketSender::destroyObjPool(): size {}.\n", 
		_g_objPool.size()));

	_g_objPool.destroy();
}

//-------------------------------------------------------------------------------------
TCPPacketSender::SmartPoolObjectPtr TCPPacketSender::createSmartPoolObj(const std::string& logPoint)
{
	return SmartPoolObjectPtr(new SmartPoolObject<TCPPacketSender>(ObjPool().createObject(logPoint), _g_objPool));
}

//-------------------------------------------------------------------------------------
TCPPacketSender::TCPPacketSender(EndPoint & endpoint,
	   NetworkInterface & networkInterface	) :
	PacketSender(endpoint, networkInterface),
	sendfailCount_(0)
{
}

//-------------------------------------------------------------------------------------
TCPPacketSender::~TCPPacketSender()
{
	//DEBUG_MSG("TCPPacketSender::~TCPPacketSender()\n");
}

//-------------------------------------------------------------------------------------
void TCPPacketSender::onGetError(Channel* pChannel, const std::string& err)
{
	pChannel->condemn(err);
	
	// 此处不必立即销毁，可能导致bufferedReceives_内部遍历迭代器破坏
	// 交给TCPPacketReceiver处理即可
	//pChannel->networkInterface().deregisterChannel(pChannel);
	//pChannel->destroy();
}

//-------------------------------------------------------------------------------------
bool TCPPacketSender::processSend(Channel* pChannel, int userarg)
{
	bool noticed = pChannel == NULL;

	// 如果是由poller通知的，我们需要通过地址找到channel
	if(noticed)
		pChannel = getChannel();

	KBE_ASSERT(pChannel != NULL);
	
	if(pChannel->condemn() == Channel::FLAG_CONDEMN_AND_DESTROY)
	{
		return false;
	}
	
	Channel::Bundles& bundles = pChannel->bundles();
	Reason reason = REASON_SUCCESS;

	Channel::Bundles::iterator iter = bundles.begin();
	for(; iter != bundles.end(); ++iter)
	{
		Bundle::Packets& pakcets = (*iter)->packets();
		Bundle::Packets::iterator iter1 = pakcets.begin();
		for (; iter1 != pakcets.end(); ++iter1)
		{
			reason = processPacket(pChannel, (*iter1), userarg);
			if(reason != REASON_SUCCESS)
				break; 
			else
				RECLAIM_PACKET((*iter)->isTCPPacket(), (*iter1));
		}

		if(reason == REASON_SUCCESS)
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
				/* 此处输出可能会造成debugHelper处死锁
					WARNING_MSG(fmt::format("TCPPacketSender::processSend: "
						"Transmit queue full, waiting for space(kbengine.xml->channelCommon->writeBufferSize->{})...\n",
						(pChannel->isInternal() ? "internal" : "external")));
				*/

				// 连续超过10次则通知出错
				if (++sendfailCount_ >= 10 && pChannel->isExternal())
				{
					onGetError(pChannel, "TCPPacketSender::processSend: sendfailCount >= 10");

					this->dispatcher().errorReporter().reportException(reason, pEndpoint_->addr(),
						fmt::format("TCPPacketSender::processSend(external, sendfailCount({}) >= 10)", (int)sendfailCount_).c_str());
				}
				else
				{
					this->dispatcher().errorReporter().reportException(reason, pEndpoint_->addr(),
						fmt::format("TCPPacketSender::processSend({}, {})", (pChannel->isInternal() ? "internal" : "external"), (int)sendfailCount_).c_str());
				}
			}
			else
			{
				if (pChannel->isExternal())
				{
#if KBE_PLATFORM == PLATFORM_UNIX
					this->dispatcher().errorReporter().reportException(reason, pEndpoint_->addr(), "TCPPacketSender::processSend(external)",
						fmt::format(", errno: {}", errno).c_str());
#else
					this->dispatcher().errorReporter().reportException(reason, pEndpoint_->addr(), "TCPPacketSender::processSend(external)",
						fmt::format(", errno: {}", WSAGetLastError()).c_str());
#endif
				}
				else
				{
#if KBE_PLATFORM == PLATFORM_UNIX
					this->dispatcher().errorReporter().reportException(reason, pEndpoint_->addr(), "TCPPacketSender::processSend(internal)",
						fmt::format(", errno: {}, {}", errno, pChannel->c_str()).c_str());
#else
					this->dispatcher().errorReporter().reportException(reason, pEndpoint_->addr(), "TCPPacketSender::processSend(internal)",
						fmt::format(", errno: {}, {}", WSAGetLastError(), pChannel->c_str()).c_str());
#endif
				}

				onGetError(pChannel, fmt::format("TCPPacketSender::processSend: errno={}", kbe_lasterror()));
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
Reason TCPPacketSender::processFilterPacket(Channel* pChannel, Packet * pPacket, int userarg)
{
	if(pChannel->condemn() == Channel::FLAG_CONDEMN_AND_DESTROY)
	{
		return REASON_CHANNEL_CONDEMN;
	}

	EndPoint* pEndpoint = pChannel->pEndPoint();
	const size_t packetLength = pPacket->length();
	if(pPacket->sentSize > packetLength)
	{
		ERROR_MSG(fmt::format(
			"TCPPacketSender::processFilterPacket: invalid send state, sentSize={}, packetLength={}, channel={}.\n",
			pPacket->sentSize, packetLength, pChannel->c_str()));
		return REASON_GENERAL_NETWORK;
	}

	const size_t remainingSize = packetLength - pPacket->sentSize;
	if(remainingSize > static_cast<size_t>(std::numeric_limits<int>::max()))
	{
		// EndPoint 与完成模型的单次发送长度均为 int；拒绝异常超大 Packet，避免截断后错发或错误完成。
		// EndPoint and completion backends use int-sized sends; reject an abnormal oversized Packet instead of truncating or completing it incorrectly.
		ERROR_MSG(fmt::format(
			"TCPPacketSender::processFilterPacket: remaining packet length exceeds socket API limit, remaining={}, channel={}.\n",
			remainingSize, pChannel->c_str()));
		return REASON_GENERAL_NETWORK;
	}

	const int remaining = static_cast<int>(remainingSize);
	EventPoller* pPoller = this->dispatcher().pPoller();
	if (pPoller != NULL && pPoller->supportsCompletion())
	{
		// Completion backends own a copy of the packet until WSASend completes, so the packet can leave the Channel queue now.
		// 完成模型会在 WSASend 完成前持有 packet 副本，因此此处可以立即移出 Channel 队列。
		if (pEndpoint->usesSSLMemoryBIO())
		{
			// TLS record 序列号会在 SSL_write 时推进，因此加密或密文入队失败必须关闭连接，不能重放同一明文。
			// TLS record sequence numbers advance during SSL_write, so encryption or ciphertext enqueue failure must close rather than replay plaintext.
			if (!pEndpoint->encryptSSLNetworkData(pPacket->data() + pPacket->sentSize, remaining) ||
				!pChannel->flushSSLNetworkOutput())
			{
				return REASON_GENERAL_NETWORK;
			}
		}
		else if (!pPoller->queueTcpSend(*pEndpoint, pPacket->data() + pPacket->sentSize, remaining))
		{
			// 队列上限会设置 would-block 并保持可重试；无效或已关闭 socket 必须沿用同步发送的错误分类并关闭 Channel。
			// Queue backpressure sets would-block and remains retryable; an invalid or closed socket follows synchronous-send error mapping and closes the Channel.
			return checkSocketErrors(pEndpoint);
		}

		pPacket->sentSize += remaining;
		pChannel->onPacketSent(remaining, true);
		return REASON_SUCCESS;
	}

	int len = pEndpoint->send(pPacket->data() + pPacket->sentSize, remaining);

	if(len > 0)
	{
		pPacket->sentSize += len;
		// DEBUG_MSG(fmt::format("TCPPacketSender::processFilterPacket: sent={}, sentTotalSize={}.\n", len, pPacket->sentSize));
	}

	bool sentCompleted = pPacket->sentSize == packetLength;
	pChannel->onPacketSent(len, sentCompleted);

	if (sentCompleted)
	{
		return REASON_SUCCESS;
	}
	else
	{
		// 如果只发送了一部分数据，则认为是REASON_RESOURCE_UNAVAILABLE
		if (len > 0)
			return REASON_RESOURCE_UNAVAILABLE;
	}

	return checkSocketErrors(pEndpoint);
}

//-------------------------------------------------------------------------------------
}
}

