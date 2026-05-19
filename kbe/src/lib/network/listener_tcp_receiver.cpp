// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com


#include "listener_tcp_receiver.h"
#ifndef CODE_INLINE
#include "listener_tcp_receiver.inl"
#endif

#include "network/address.h"
#include "network/bundle.h"
#include "network/endpoint.h"
#include "network/event_dispatcher.h"
#include "network/network_interface.h"
#include "network/packet_receiver.h"
#include "network/event_poller.h"
#include "network/error_reporter.h"

namespace KBEngine { 
namespace Network
{
//-------------------------------------------------------------------------------------
ListenerTcpReceiver::ListenerTcpReceiver(EndPoint & endpoint,
								   Channel::Traits traits, 
									NetworkInterface & networkInterface	):
	ListenerReceiver(endpoint, traits, networkInterface)
{
}

//-------------------------------------------------------------------------------------
ListenerTcpReceiver::~ListenerTcpReceiver()
{
}

//-------------------------------------------------------------------------------------
int ListenerTcpReceiver::handleInputNotification(int fd)
{
	int tickcount = 0;

	while(tickcount ++ < 256)
	{
		EndPoint* pNewEndPoint = NULL;

		EventPoller* pPoller = this->dispatcher().pPoller();
		if (pPoller != NULL && pPoller->supportsCompletion())
		{
			// completion listener 的 accept socket 只来自 poller completion。
			// 如果队列里没有完成的连接，直接退出本轮；不能再 fallback 到 accept()，
			// 否则会在 completion/readiness 两套模型之间制造重复接受或阻塞风险。
#if KBE_PLATFORM == PLATFORM_WIN32
			KBESOCKET acceptedSocket = INVALID_SOCKET;
#else
			KBESOCKET acceptedSocket = -1;
#endif
			if (pPoller->takeAcceptedSocket(fd, acceptedSocket))
			{
				pNewEndPoint = EndPoint::createPoolObject(OBJECTPOOL_POINT);
				pNewEndPoint->setFileDescriptor(acceptedSocket);
				pNewEndPoint->setnonblocking(true);
				pNewEndPoint->setnodelay(true);

				u_int16_t networkPort = 0;
				u_int32_t networkAddr = 0;
				if (pNewEndPoint->getremoteaddress(&networkPort, &networkAddr) == 0)
				{
					pNewEndPoint->addr(networkPort, networkAddr);
				}
				else
				{
#if KBE_PLATFORM == PLATFORM_WIN32
					WARNING_MSG(fmt::format("ListenerTcpReceiver::handleInputNotification: getremoteaddress({}) failed: {}\n",
						fd, kbe_strerror(WSAGetLastError())));
#else
					WARNING_MSG(fmt::format("ListenerTcpReceiver::handleInputNotification: getremoteaddress({}) failed: {}\n",
						fd, kbe_strerror()));
#endif
				}
			}

			if (pNewEndPoint == NULL)
			{
				break;
			}
		}
		else
		{
			pNewEndPoint = endpoint_.accept();
		}

		if(pNewEndPoint == NULL){

			if(tickcount == 1)
			{
				WARNING_MSG(fmt::format("ListenerTcpReceiver::handleInputNotification: accept endpoint({}) {}! channelSize={}\n",
					fd, kbe_strerror(), networkInterface_.channels().size()));
				
				this->dispatcher().errorReporter().reportException(
						REASON_GENERAL_NETWORK);
			}

			break;
		}
		else
		{
			Channel* pChannel = Network::Channel::createPoolObject(OBJECTPOOL_POINT);
			bool ret = pChannel->initialize(networkInterface_, pNewEndPoint, traits_);
			if(!ret)
			{
				ERROR_MSG(fmt::format("ListenerTcpReceiver::handleInputNotification: initialize({}) is failed!\n",
					pChannel->c_str()));

				pChannel->destroy();
				Network::Channel::reclaimPoolObject(pChannel);
				return 0;
			}

			if(!networkInterface_.registerChannel(pChannel))
			{
				ERROR_MSG(fmt::format("ListenerTcpReceiver::handleInputNotification: registerChannel({}) is failed!\n",
					pChannel->c_str()));

				pChannel->destroy();
				Network::Channel::reclaimPoolObject(pChannel);
			}
		}
	}

	return 0;
}

//-------------------------------------------------------------------------------------
}
}
