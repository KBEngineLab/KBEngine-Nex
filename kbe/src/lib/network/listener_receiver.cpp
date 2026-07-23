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


#include "listener_receiver.h"
#ifndef CODE_INLINE
#include "listener_receiver.inl"
#endif

#include "network/address.h"
#include "network/bundle.h"
#include "network/endpoint.h"
#include "network/event_dispatcher.h"
#include "network/event_poller.h"
#include "network/network_interface.h"
#include "network/packet_receiver.h"
#include "network/error_reporter.h"

namespace KBEngine { 
namespace Network
{
//-------------------------------------------------------------------------------------
ListenerReceiver::ListenerReceiver(EndPoint & endpoint,
								   Channel::Traits traits, 
									NetworkInterface & networkInterface	) :
	endpoint_(endpoint),
	traits_(traits),
	networkInterface_(networkInterface)
{
}

//-------------------------------------------------------------------------------------
ListenerReceiver::~ListenerReceiver()
{
}

//-------------------------------------------------------------------------------------
int ListenerReceiver::handleInputNotification(int fd)
{
	int tickcount = 0;
	EventPoller* pPoller = this->dispatcher().pPoller();
	const bool completion = pPoller != NULL && pPoller->supportsCompletion();

	while(tickcount ++ < 256)
	{
		EndPoint* pNewEndPoint = NULL;
		if (completion)
		{
			// Completion backends already created the socket asynchronously; consume it without calling accept again.
			// 完成模型已经异步创建 socket，这里直接消费结果，不能再次调用 accept。
			KBESOCKET acceptedSocket = (KBESOCKET)-1;
			if (!pPoller->takeAcceptedSocket(fd, acceptedSocket))
				break;

			pNewEndPoint = EndPoint::createPoolObject(OBJECTPOOL_POINT);
			pNewEndPoint->setFileDescriptor(acceptedSocket);

			u_int16_t networkPort = 0;
			u_int32_t networkAddr = 0;
			if (pNewEndPoint->getremoteaddress(&networkPort, &networkAddr) != 0)
			{
				// Reclaim failed completion results so an invalid peer never leaks its native socket.
				// 回收失败的完成结果，避免无效对端导致原生 socket 泄漏。
				pNewEndPoint->close();
				EndPoint::reclaimPoolObject(pNewEndPoint);
				continue;
			}

			pNewEndPoint->addr(networkPort, networkAddr);
			pNewEndPoint->setnonblocking(true);
			pNewEndPoint->setnodelay(true);
		}
		else
		{
			pNewEndPoint = endpoint_.accept();
		}

		if(pNewEndPoint == NULL){

			if(tickcount == 1)
			{
				WARNING_MSG(fmt::format("ListenerReceiver::handleInputNotification: accept endpoint({}) {}! channelSize={}\n",
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
				ERROR_MSG(fmt::format("ListenerReceiver::handleInputNotification: initialize({}) is failed!\n",
					pChannel->c_str()));

				pChannel->destroy();
				Network::Channel::reclaimPoolObject(pChannel);
				return 0;
			}

			if(!networkInterface_.registerChannel(pChannel))
			{
				ERROR_MSG(fmt::format("ListenerReceiver::handleInputNotification: registerChannel({}) is failed!\n",
					pChannel->c_str()));

				pChannel->destroy();
				Network::Channel::reclaimPoolObject(pChannel);
			}
		}
	}

	return 0;
}

//-------------------------------------------------------------------------------------
EventDispatcher & ListenerReceiver::dispatcher()
{
	return networkInterface_.dispatcher();
}

//-------------------------------------------------------------------------------------
}
}
