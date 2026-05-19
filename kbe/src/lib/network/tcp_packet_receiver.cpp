// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com


#include "tcp_packet_receiver.h"
#ifndef CODE_INLINE
#include "tcp_packet_receiver.inl"
#endif

#include "network/address.h"
#include "network/bundle.h"
#include "network/channel.h"
#include "network/endpoint.h"
#include "network/event_dispatcher.h"
#include "network/network_interface.h"
#include "network/event_poller.h"
#include "network/error_reporter.h"
#include <openssl/err.h>

namespace KBEngine { 
namespace Network
{

//-------------------------------------------------------------------------------------
static ObjectPool<TCPPacketReceiver> _g_objPool("TCPPacketReceiver");
ObjectPool<TCPPacketReceiver>& TCPPacketReceiver::ObjPool()
{
	return _g_objPool;
}

//-------------------------------------------------------------------------------------
TCPPacketReceiver* TCPPacketReceiver::createPoolObject(const std::string& logPoint)
{
	return _g_objPool.createObject(logPoint);
}

//-------------------------------------------------------------------------------------
void TCPPacketReceiver::reclaimPoolObject(TCPPacketReceiver* obj)
{
	_g_objPool.reclaimObject(obj);
}

//-------------------------------------------------------------------------------------
void TCPPacketReceiver::destroyObjPool()
{
	DEBUG_MSG(fmt::format("TCPPacketReceiver::destroyObjPool(): size {}.\n", 
		_g_objPool.size()));

	_g_objPool.destroy();
}

//-------------------------------------------------------------------------------------
TCPPacketReceiver::SmartPoolObjectPtr TCPPacketReceiver::createSmartPoolObj(const std::string& logPoint)
{
	return SmartPoolObjectPtr(new SmartPoolObject<TCPPacketReceiver>(ObjPool().createObject(logPoint), _g_objPool));
}

//-------------------------------------------------------------------------------------
TCPPacketReceiver::TCPPacketReceiver(EndPoint & endpoint,
	   NetworkInterface & networkInterface	) :
	PacketReceiver(endpoint, networkInterface)
{
}

//-------------------------------------------------------------------------------------
TCPPacketReceiver::~TCPPacketReceiver()
{
	//DEBUG_MSG("TCPPacketReceiver::~TCPPacketReceiver()\n");
}

//-------------------------------------------------------------------------------------
bool TCPPacketReceiver::processRecv(bool expectingPacket)
{
	Channel* pChannel = getChannel();
	KBE_ASSERT(pChannel != NULL);

	if(pChannel->condemn() > 0)
	{
		return false;
	}

	TCPPacket* pReceiveWindow = TCPPacket::createPoolObject(OBJECTPOOL_POINT);

	EventPoller* pPoller = this->dispatcher().pPoller();
	if (pPoller != NULL && pPoller->supportsCompletion())
	{
		// completion 模式下，socket 数据已经由 poller 接收并放入队列。
		// 这里不能再调用 recv，否则会把 completion 模型退回 readiness 模型，
		// 并可能在登录/断线 burst 时读错时序或阻塞主线程。
		std::vector<char> data;
		bool disconnected = false;
		int errorCode = 0;
		if (!pPoller->takeTcpReceivedData(static_cast<int>(*pEndpoint_), data, disconnected, errorCode))
		{
			TCPPacket::reclaimPoolObject(pReceiveWindow);
			return false;
		}

		if (errorCode != 0)
		{
			// 发送 completion 失败也会通过读侧错误队列进入这里。
			// 这样 channel 的关闭/注销仍然走 TCPPacketReceiver 原来的错误路径。
			TCPPacket::reclaimPoolObject(pReceiveWindow);
#if KBE_PLATFORM == PLATFORM_WIN32
			WSASetLastError(errorCode);
#else
			errno = errorCode;
#endif
			PacketReceiver::RecvState rstate = this->checkSocketErrors(-1, expectingPacket);
			if (rstate == PacketReceiver::RECV_STATE_INTERRUPT)
			{
				onGetError(pChannel, fmt::format("TCPPacketReceiver::processRecv(): error={}\n", kbe_lasterror()));
				return false;
			}

			return rstate == PacketReceiver::RECV_STATE_CONTINUE;
		}

		if (disconnected)
		{
			TCPPacket::reclaimPoolObject(pReceiveWindow);
			onGetError(pChannel, "disconnected");
			return false;
		}

		if (data.empty())
		{
			TCPPacket::reclaimPoolObject(pReceiveWindow);
			return false;
		}

		memcpy(pReceiveWindow->data(), data.data(), data.size());
		pReceiveWindow->wpos(static_cast<uint32>(data.size()));

		Reason ret = this->processPacket(pChannel, pReceiveWindow);

		if (ret != REASON_SUCCESS)
			this->dispatcher().errorReporter().reportException(ret, pEndpoint_->addr());

		return true;
	}

	int len = pReceiveWindow->recvFromEndPoint(*pEndpoint_);

	if (len < 0)
	{
		TCPPacket::reclaimPoolObject(pReceiveWindow);

		PacketReceiver::RecvState rstate = this->checkSocketErrors(len, expectingPacket);

		if(rstate == PacketReceiver::RECV_STATE_INTERRUPT)
		{
			onGetError(pChannel, fmt::format("TCPPacketReceiver::processRecv(): error={}\n", kbe_lasterror()));
			return false;
		}

		return rstate == PacketReceiver::RECV_STATE_CONTINUE;
	}
	else if(len == 0) // 客户端正常退出
	{
		TCPPacket::reclaimPoolObject(pReceiveWindow);
		onGetError(pChannel, "disconnected");
		return false;
	}
	
	Reason ret = this->processPacket(pChannel, pReceiveWindow);

	if(ret != REASON_SUCCESS)
		this->dispatcher().errorReporter().reportException(ret, pEndpoint_->addr());
	
	return true;
}

//-------------------------------------------------------------------------------------
void TCPPacketReceiver::onGetError(Channel* pChannel, const std::string& err)
{
	pChannel->condemn(err);
	pChannel->networkInterface().deregisterChannel(pChannel);
	pChannel->destroy();
	Network::Channel::reclaimPoolObject(pChannel);
}

//-------------------------------------------------------------------------------------
Reason TCPPacketReceiver::processFilteredPacket(Channel* pChannel, Packet * pPacket)
{
	// 如果为None， 则可能是被过滤器过滤掉了(过滤器正在按照自己的规则组包解密)
	if(pPacket)
	{
		pChannel->addReceiveWindow(pPacket);
	}

	return REASON_SUCCESS;
}

//-------------------------------------------------------------------------------------
PacketReceiver::RecvState TCPPacketReceiver::checkSocketErrors(int len, bool expectingPacket)
{
	Channel* pChannel = getChannel();

#if KBE_PLATFORM == PLATFORM_WIN32
	DWORD wsaErr = WSAGetLastError();
#endif //def _WIN32

	if (
#if KBE_PLATFORM == PLATFORM_WIN32
		wsaErr == WSAEWOULDBLOCK && !expectingPacket// send出错大概是缓冲区满了, recv出错已经无数据可读了
#else
		errno == EAGAIN && !expectingPacket			// recv缓冲区已经无数据可读了
#endif
		)
	{
		return RECV_STATE_BREAK;
	}

#if KBE_PLATFORM_UNIX_FAMILY
	if (errno == EAGAIN ||							// 已经无数据可读了
		errno == ECONNREFUSED ||					// 连接被服务器拒绝
		errno == EHOSTUNREACH)						// 目的地址不可到达
	{
		this->dispatcher().errorReporter().reportException(
				REASON_NO_SUCH_PORT);

		return RECV_STATE_BREAK;
	}
#else
	/*
	存在的连接被远程主机强制关闭。通常原因为：远程主机上对等方应用程序突然停止运行，或远程主机重新启动，
	或远程主机在远程方套接字上使用了“强制”关闭（参见setsockopt(SO_LINGER)）。
	另外，在一个或多个操作正在进行时，如果连接因“keep-alive”活动检测到一个失败而中断，也可能导致此错误。
	此时，正在进行的操作以错误码WSAENETRESET失败返回，后续操作将失败返回错误码WSAECONNRESET
	*/
	switch(wsaErr)
	{
	case WSAECONNRESET:
		WARNING_MSG(fmt::format("TCPPacketReceiver::processPendingEvents({}): "
			"Throwing REASON_GENERAL_NETWORK - WSAECONNRESET\n", (pEndpoint_ ? pEndpoint_->addr().c_str() : "")));
		return RECV_STATE_INTERRUPT;
	case WSAECONNABORTED:
		if (pChannel != NULL &&
			pChannel->isInternal() &&
			pChannel->hasHandshake() &&
			pChannel->componentID() == 0 &&
			pChannel->numPacketsReceived() == 1 &&
			pChannel->numBytesReceived() == NETWORK_MESSAGE_ID_SIZE)
		{
			INFO_MSG(fmt::format("TCPPacketReceiver::processPendingEvents({}): "
				"internal fixed-size probe disconnected after first request "
				"(likely lookApp/queryLoad/reqClose, WSAECONNABORTED).\n",
				(pEndpoint_ ? pEndpoint_->addr().c_str() : "")));
			return RECV_STATE_INTERRUPT;
		}

		WARNING_MSG(fmt::format("TCPPacketReceiver::processPendingEvents({}): "
			"Throwing REASON_GENERAL_NETWORK - WSAECONNABORTED\n", (pEndpoint_ ? pEndpoint_->addr().c_str() : "")));
		return RECV_STATE_INTERRUPT;
	default:
		break;

	};

#endif // unix

#if KBE_PLATFORM == PLATFORM_WIN32
	if (wsaErr == 0
#else
	if (errno == 0
#endif
		&& pEndPoint()->isSSL())
	{
		long sslerr = ERR_get_error();
		if (sslerr > 0)
		{
			WARNING_MSG(fmt::format("TCPPacketReceiver::processPendingEvents({}): "
				"Throwing SSL - {}\n",
				(pEndpoint_ ? pEndpoint_->addr().c_str() : ""), ERR_error_string(sslerr, NULL)));

			return RECV_STATE_INTERRUPT;
		}
	}

#if KBE_PLATFORM == PLATFORM_WIN32
	WARNING_MSG(fmt::format("TCPPacketReceiver::processPendingEvents({}): "
				"Throwing REASON_GENERAL_NETWORK - {}\n",
				(pEndpoint_ ? pEndpoint_->addr().c_str() : ""), wsaErr));
#else
	WARNING_MSG(fmt::format("TCPPacketReceiver::processPendingEvents({}): "
				"Throwing REASON_GENERAL_NETWORK - {}\n",
				(pEndpoint_ ? pEndpoint_->addr().c_str() : ""), kbe_strerror()));
#endif

	//this->dispatcher().errorReporter().reportException(
	//		REASON_GENERAL_NETWORK);

	return RECV_STATE_CONTINUE;
}

//-------------------------------------------------------------------------------------
}
}

