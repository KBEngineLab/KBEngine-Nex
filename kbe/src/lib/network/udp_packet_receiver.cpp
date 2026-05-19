// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com


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
Channel* UDPPacketReceiver::findChannel(const Address& addr)
{
	return pNetworkInterface_->findChannel(addr);
}

//-------------------------------------------------------------------------------------
bool UDPPacketReceiver::processRecv(bool expectingPacket)
{	
	Address	srcAddr;
	UDPPacket* pChannelReceiveWindow = UDPPacket::createPoolObject(OBJECTPOOL_POINT);

	EventPoller* pPoller = this->dispatcher().pPoller();
	if (pPoller != NULL && pPoller->supportsCompletion())
	{
		// UDP/KCP 在 completion 模式下消费 poller 队列。
		// srcAddr 来自 completion context，避免这里再次 recvfrom。
		std::vector<char> data;
		int errorCode = 0;
		if (!pPoller->takeUdpReceivedData(static_cast<int>(*pEndpoint_), data, srcAddr, errorCode))
		{
			UDPPacket::reclaimPoolObject(pChannelReceiveWindow);
			return false;
		}

		if (errorCode != 0)
		{
			UDPPacket::reclaimPoolObject(pChannelReceiveWindow);
#if KBE_PLATFORM == PLATFORM_WIN32
			WSASetLastError(errorCode);
#else
			errno = errorCode;
#endif
			PacketReceiver::RecvState rstate = this->checkSocketErrors(-1, expectingPacket);
			return rstate == PacketReceiver::RECV_STATE_CONTINUE;
		}

		if (data.empty())
		{
			UDPPacket::reclaimPoolObject(pChannelReceiveWindow);
			return false;
		}

		memcpy(pChannelReceiveWindow->data(), data.data(), data.size());
		pChannelReceiveWindow->wpos(static_cast<uint32>(data.size()));
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
		pNewEndPoint->addr(srcAddr);
		pNewEndPoint->setSocketRef(pEndpoint_->socket());

		pSrcChannel = Network::Channel::createPoolObject(OBJECTPOOL_POINT);
		bool ret = pSrcChannel->initialize(*pNetworkInterface_, pNewEndPoint, Channel::EXTERNAL, PROTOCOL_UDP, protocolSubType());
		if(!ret)
		{
			ERROR_MSG(fmt::format("UDPPacketReceiver::processRecv: initialize({}) is failed!\n",
				pSrcChannel->c_str()));

			if (pSrcChannel->pEndPoint() != pNewEndPoint)
				EndPoint::reclaimPoolObject(pNewEndPoint);

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

	return static_cast<UDPPacketReceiver*>(pPacketReceiver)->processRecv(pChannelReceiveWindow);
}

//-------------------------------------------------------------------------------------
bool UDPPacketReceiver::processRecv(UDPPacket* pReceiveWindow)
{
	Reason ret = this->processPacket(getChannel(), pReceiveWindow);

	if (ret != REASON_SUCCESS)
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

#if KBE_PLATFORM_UNIX_FAMILY
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

	if (wsaErr == ERROR_PORT_UNREACHABLE)
	{
		// Windows UDP/KCP 在对端关闭或端口不可达时可能返回
		// ERROR_PORT_UNREACHABLE(1234)。这是 ICMP 反馈，不代表本地
		// UDP socket 坏掉；旧逻辑依赖 KCP 发送失败/超时来清理 channel，
		// 所以这里保持静默继续，避免大量 REASON_GENERAL_NETWORK。
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
