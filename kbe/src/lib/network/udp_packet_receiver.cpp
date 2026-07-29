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


#include "udp_packet_receiver.h"
#ifndef CODE_INLINE
#include "udp_packet_receiver.inl"
#endif

#include "network/address.h"
#include "network/bundle.h"
#include "network/channel.h"
#include "network/endpoint.h"
#include "network/event_dispatcher.h"
#include "network/network_interface.h"
#include "network/event_poller.h"
#include "network/error_reporter.h"

namespace KBEngine { 
namespace Network
{

//-------------------------------------------------------------------------------------
static ObjectPool<UDPPacketReceiver> _g_objPool("UDPPacketReceiver");
ObjectPool<UDPPacketReceiver>& UDPPacketReceiver::ObjPool()
{
	return _g_objPool;
}

//-------------------------------------------------------------------------------------
UDPPacketReceiver* UDPPacketReceiver::createPoolObject(const std::string& logPoint)
{
	return _g_objPool.createObject(logPoint);
}

//-------------------------------------------------------------------------------------
void UDPPacketReceiver::reclaimPoolObject(UDPPacketReceiver* obj)
{
	_g_objPool.reclaimObject(obj);
}

//-------------------------------------------------------------------------------------
void UDPPacketReceiver::destroyObjPool()
{
	DEBUG_MSG(fmt::format("UDPPacketReceiver::destroyObjPool(): size {}.\n", 
		_g_objPool.size()));

	_g_objPool.destroy();
}

//-------------------------------------------------------------------------------------
UDPPacketReceiver::SmartPoolObjectPtr UDPPacketReceiver::createSmartPoolObj(const std::string& logPoint)
{
	return SmartPoolObjectPtr(new SmartPoolObject<UDPPacketReceiver>(ObjPool().createObject(logPoint), _g_objPool));
}

//-------------------------------------------------------------------------------------
UDPPacketReceiver::UDPPacketReceiver(EndPoint & endpoint,
	   NetworkInterface & networkInterface	) :
	PacketReceiver(endpoint, networkInterface)
{
}

//-------------------------------------------------------------------------------------
UDPPacketReceiver::~UDPPacketReceiver()
{
}

//-------------------------------------------------------------------------------------
Channel* UDPPacketReceiver::findChannel(const Address& address)
{
	return pNetworkInterface_->findChannel(address);
}

//-------------------------------------------------------------------------------------
bool UDPPacketReceiver::processRecv(bool expectingPacket)
{	
	Address	srcAddr;
	UDPPacket* pChannelReceiveWindow = UDPPacket::createPoolObject(OBJECTPOOL_POINT);
	EventPoller* pPoller = this->dispatcher().pPoller();
	if (pPoller != NULL && pPoller->supportsCompletion())
	{
		// completion 后端已经保留来源地址和报文边界，不能退回 recvfrom 读取同一个数据报。
		// A completion backend preserves the source address and datagram boundary, so recvfrom must not read the same datagram again.
		std::vector<char> data;
		int errorCode = 0;
		if (!pPoller->takeUdpReceivedData(*pEndpoint_, data, srcAddr, errorCode))
		{
			UDPPacket::reclaimPoolObject(pChannelReceiveWindow);
			return false;
		}

		if (errorCode != 0)
		{
#if KBE_PLATFORM == PLATFORM_WIN32
			WSASetLastError(errorCode);
#else
			errno = errorCode;
#endif
			UDPPacket::reclaimPoolObject(pChannelReceiveWindow);
			PacketReceiver::RecvState rstate = this->checkSocketErrors(-1, expectingPacket);
			return rstate == PacketReceiver::RECV_STATE_CONTINUE;
		}

		if (data.empty())
		{
			UDPPacket::reclaimPoolObject(pChannelReceiveWindow);
			return false;
		}

		// append 保持 UDP 报文的完整边界，并复用原有的 Channel 查找和协议解析流程。
		// append preserves the complete UDP datagram boundary while reusing the existing channel lookup and protocol parsing flow.
		pChannelReceiveWindow->append(data.data(), data.size());
	}
	else
	{
	int len = pChannelReceiveWindow->recvFromEndPoint(*pEndpoint_, &srcAddr);


	if (len <= 0)
	{
		UDPPacket::reclaimPoolObject(pChannelReceiveWindow);
		PacketReceiver::RecvState rstate = this->checkSocketErrors(len, expectingPacket);
		return rstate == PacketReceiver::RECV_STATE_CONTINUE;
	}
	

	}

	Channel* pSrcChannel = findChannel(srcAddr);

	if(pSrcChannel == NULL) 
	{
		EndPoint* pNewEndPoint = EndPoint::createPoolObject(OBJECTPOOL_POINT);
		pNewEndPoint->addr(srcAddr.port, srcAddr.ip);
		// 每个远端地址拥有独立的 Channel 状态，但底层 UDP listener socket 由监听器统一持有。
		// Each remote address owns independent Channel state while the underlying UDP listener socket remains owned by the listener.
		pNewEndPoint->setSocketRef(static_cast<KBESOCKET>(*pEndpoint_));

		pSrcChannel = Network::Channel::createPoolObject(OBJECTPOOL_POINT);
		bool ret = pSrcChannel->initialize(*pNetworkInterface_, pNewEndPoint, Channel::EXTERNAL,
			PROTOCOL_UDP, NULL, CHANNEL_ID_NULL, protocolSubType());
		if(!ret)
		{
			ERROR_MSG(fmt::format("UDPPacketReceiver::processRecv: initialize({}) is failed!\n",
				pSrcChannel->c_str()));

			pSrcChannel->destroy();
			Network::Channel::reclaimPoolObject(pSrcChannel);
			UDPPacket::reclaimPoolObject(pChannelReceiveWindow);
			return false;
		}

		if(!pNetworkInterface_->registerChannel(pSrcChannel))
		{
			ERROR_MSG(fmt::format("UDPPacketReceiver::processRecv: registerChannel({}) is failed!\n",
				pSrcChannel->c_str()));

			UDPPacket::reclaimPoolObject(pChannelReceiveWindow);
			pSrcChannel->destroy();
			Network::Channel::reclaimPoolObject(pSrcChannel);
			return false;
		}
	}
	
	KBE_ASSERT(pSrcChannel != NULL);

	if (pSrcChannel->isDestroyed())
	{
		UDPPacket::reclaimPoolObject(pChannelReceiveWindow);
		return false;
	}

	if (pSrcChannel->condemn() > 0)
	{
		UDPPacket::reclaimPoolObject(pChannelReceiveWindow);
		return false;
	}

	PacketReceiver* pPacketReceiver = pSrcChannel->pPacketReceiver();
	if (pPacketReceiver == NULL || pPacketReceiver->type() != PacketReceiver::UDP_PACKET_RECEIVER)
	{
		ERROR_MSG(fmt::format("UDPPacketReceiver::processRecv: invalid packet receiver on channel {}.\n",
			pSrcChannel->c_str()));
		UDPPacket::reclaimPoolObject(pChannelReceiveWindow);
		return false;
	}

	// listener 只负责按来源地址找到 Channel；协议解析必须交给该 Channel 的 receiver，才能保留各自的 KCP 控制块与过滤器状态。
	// The listener only resolves the source Channel; protocol parsing belongs to that Channel's receiver so each KCP control block and filter state remains isolated.
	return static_cast<UDPPacketReceiver*>(pPacketReceiver)->processRecv(pChannelReceiveWindow);
}

//-------------------------------------------------------------------------------------
bool UDPPacketReceiver::processRecv(UDPPacket* pReceiveWindow)
{
	Channel* pChannel = getChannel();
	if (pChannel == NULL || pChannel->isDestroyed() || pChannel->condemn() > 0)
	{
		UDPPacket::reclaimPoolObject(pReceiveWindow);
		return false;
	}

	Reason ret = this->processPacket(pChannel, pReceiveWindow);

	if(ret != REASON_SUCCESS)
		this->dispatcher().errorReporter().reportException(ret, pEndpoint_->addr());
	
	return true;
}

//-------------------------------------------------------------------------------------
Reason UDPPacketReceiver::processFilteredPacket(Channel* pChannel, Packet * pPacket)
{
	// 如果为None， 则可能是被过滤器过滤掉了(过滤器正在按照自己的规则组包解密)
	if(pPacket)
	{
		pChannel->addReceiveWindow(pPacket);
	}

	return REASON_SUCCESS;
}

//-------------------------------------------------------------------------------------
PacketReceiver::RecvState UDPPacketReceiver::checkSocketErrors(int len, bool expectingPacket)
{
	if (len == 0)
	{
		WARNING_MSG(fmt::format("PacketReceiver::processPendingEvents: "
			"Throwing REASON_GENERAL_NETWORK (1)- {}\n",
			strerror( errno )));

		this->dispatcher().errorReporter().reportException(
				REASON_GENERAL_NETWORK );

		return RECV_STATE_CONTINUE;
	}
	
#ifdef _WIN32
	DWORD wsaErr = WSAGetLastError();
#endif //def _WIN32

	if (
#ifdef _WIN32
		wsaErr == WSAEWOULDBLOCK && !expectingPacket
#else
		errno == EAGAIN && !expectingPacket
#endif
		)
	{
		return RECV_STATE_BREAK;
	}

#if KBE_PLATFORM == PLATFORM_UNIX
	if (errno == EAGAIN ||
		errno == ECONNREFUSED ||
		errno == EHOSTUNREACH)
	{
		Network::Address offender;

		if (pEndpoint_->getClosedPort(offender))
		{
			// If we got a NO_SUCH_PORT error and there is an internal
			// channel to this address, mark it as remote failed.  The logic
			// for dropping external channels that get NO_SUCH_PORT
			// exceptions is built into BaseApp::onClientNoSuchPort().
			if (errno == ECONNREFUSED)
			{
				// 未实现
			}

			this->dispatcher().errorReporter().reportException(
					REASON_NO_SUCH_PORT, offender);

			return RECV_STATE_CONTINUE;
		}
		else
		{
			WARNING_MSG("UDPPacketReceiver::processPendingEvents: "
				"getClosedPort() failed\n");
		}
	}
#else
	if (wsaErr == WSAECONNRESET)
	{
		return RECV_STATE_CONTINUE;
	}
#endif // unix

#ifdef _WIN32
	WARNING_MSG(fmt::format("UDPPacketReceiver::processPendingEvents: "
				"Throwing REASON_GENERAL_NETWORK - {}\n",
				wsaErr));
#else
	WARNING_MSG(fmt::format("UDPPacketReceiver::processPendingEvents: "
				"Throwing REASON_GENERAL_NETWORK - {}\n",
			kbe_strerror()));
#endif
	this->dispatcher().errorReporter().reportException(
			REASON_GENERAL_NETWORK);

	return RECV_STATE_CONTINUE;
}

//-------------------------------------------------------------------------------------
}
}
