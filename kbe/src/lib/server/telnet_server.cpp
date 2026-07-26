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


#include "telnet_server.h"
#include "telnet_handler.h"
#include "network/bundle.h"
#include "network/endpoint.h"
#include "network/event_poller.h"
#include "network/network_interface.h"

#ifndef CODE_INLINE
#include "telnet_server.inl"
#endif

namespace KBEngine { 

//-------------------------------------------------------------------------------------
TelnetServer::TelnetServer(Network::EventDispatcher* pDispatcher, Network::NetworkInterface* networkInterface):
handlers_(),
listener_(),
pDispatcher_(pDispatcher),
passwd_(),
deflayer_(TelnetHandler::TELNET_STATE_ROOT),
pNetworkInterface_(networkInterface),
port_(0)
{
}

//-------------------------------------------------------------------------------------
TelnetServer::~TelnetServer(void)
{
}

//-------------------------------------------------------------------------------------
bool TelnetServer::start(std::string passwd, std::string deflayer, u_int16_t port, u_int32_t ip)
{
	listener_.socket(SOCK_STREAM);
	listener_.setnonblocking(true);
	
	passwd_ = passwd;

	if(deflayer == "python")
		deflayer_ = TelnetHandler::TELNET_STATE_PYTHON;

	int tryn = 0;
	while(true)
	{
		port_ = port;
		if (listener_.bind(htons(port), htonl(ip)) == -1)
		{
			port++;
			tryn++;

			if(tryn > 65535)
			{
				tryn = 0;
				break;
			}

			continue;
		}
		else
		{
			tryn = 1;
			break;
		}
	};
	
	if(tryn == 0)
	{
		if (listener_.bind(htons(0), ip) == -1)
		{
			ERROR_MSG(fmt::format("TelnetServer::start:: bind port({}) is failed! ip={}\n", 
				0, ip));

			return false;
		}
	}

	if(listener_.listen(1) == -1)
	{
		ERROR_MSG(fmt::format("TelnetServer::start:: listen is failed! addr={}\n", 
			listener_.c_str()));

		return false;
	}

	if(!pDispatcher_->registerReadFileDescriptor(listener_, this))
	{
		ERROR_MSG(fmt::format("TelnetServer::start:: registerReadFileDescriptor is failed! addr={}\n", 
			listener_.c_str()));

		return false;
	}

	INFO_MSG(fmt::format("TelnetServer server is running on port {}\n", port));

#if KBE_PLATFORM == PLATFORM_WIN32
	printf("[INFO]: %s", fmt::format("TelnetServer server is running on port {}\n", port).c_str());
#endif

	return true;
}

//-------------------------------------------------------------------------------------
void TelnetServer::onTelnetHandlerClosed(KBESOCKET fd, TelnetHandler* pTelnetHandler)
{
	INFO_MSG(fmt::format("TelnetServer::onTelnetHandlerClosed: del handler({})!\n",
		pTelnetHandler->pEndPoint()->c_str()));

	KBE_ASSERT(fd == (*pTelnetHandler->pEndPoint()));
	pDispatcher_->deregisterReadFileDescriptor(fd);
	handlers_.erase(fd);
}

//-------------------------------------------------------------------------------------
bool TelnetServer::stop()
{
	pDispatcher_->deregisterReadFileDescriptor(listener_);
	listener_.close();
	return true;
}

//-------------------------------------------------------------------------------------
void TelnetServer::closeHandler(KBESOCKET fd, TelnetHandler* pTelnetHandler)
{
	TelnetHandlers::iterator iter = handlers_.find(fd);
	if(iter == handlers_.end() || iter->second.get() != pTelnetHandler)
	{
		ERROR_MSG(fmt::format("TelnetServer::closeHandler: not found fd({})!\n", fd));
		return;
	}

	pDispatcher_->deregisterReadFileDescriptor(fd);
	handlers_.erase(iter);
	// TelnetHandler 析构会关闭并回收其 endpoint，避免这里先关闭裸 fd 后对象池再次关闭已被复用的描述符。
	// TelnetHandler destruction closes and reclaims its endpoint, avoiding a second close on a descriptor that may already have been reused.
}

//-------------------------------------------------------------------------------------
int	TelnetServer::handleInputNotification(KBESOCKET fd)
{
	KBE_ASSERT(listener_ == fd);

	int tickcount = 0;

	while(tickcount ++ < 1024)
	{
		Network::EndPoint* pNewEndPoint = NULL;
		Network::EventPoller* pPoller = pDispatcher_->pPoller();
		if(pPoller != NULL && pPoller->supportsCompletion())
		{
			// 完成后端已经执行 accept，此处只取得已完成 socket，不能再次调用同步 accept。
			// A completion backend has already performed accept, so consume its completed socket instead of calling synchronous accept again.
#if KBE_PLATFORM == PLATFORM_WIN32
			KBESOCKET acceptedSocket = INVALID_SOCKET;
#else
			KBESOCKET acceptedSocket = -1;
#endif
			if(!pPoller->takeAcceptedSocket(fd, acceptedSocket))
				break;

			pNewEndPoint = Network::EndPoint::createPoolObject(OBJECTPOOL_POINT);
			pNewEndPoint->setFileDescriptor(acceptedSocket);
			pNewEndPoint->setnonblocking(true);
			pNewEndPoint->setnodelay(true);

			u_int16_t networkPort = 0;
			u_int32_t networkAddr = 0;
			if(pNewEndPoint->getremoteaddress(&networkPort, &networkAddr) == 0)
			{
				pNewEndPoint->addr(networkPort, networkAddr);
			}
			else
			{
				WARNING_MSG(fmt::format("TelnetServer::handleInputNotification: getremoteaddress({}) failed: {}\n",
					fd, kbe_strerror()));
			}
		}
		else
		{
			pNewEndPoint = listener_.accept();
		}

		if(pNewEndPoint == NULL){

			if(tickcount == 1)
			{
				WARNING_MSG(fmt::format("TelnetServer::handleInputNotification: accept endpoint({}) {}! channelSize={}\n",
					fd, kbe_strerror(), pNetworkInterface_->channels().size()));
			}

			break;
		}
		else
		{
			TelnetHandler* pTelnetHandler = new TelnetHandler(pNewEndPoint, this, pNetworkInterface_, passwd_.size() > 0 ? 
				TelnetHandler::TELNET_STATE_PASSWD : (TelnetHandler::TELNET_STATE)this->deflayer());

			if(!pDispatcher_->registerReadFileDescriptor((*pNewEndPoint), pTelnetHandler))
			{
				ERROR_MSG(fmt::format("TelnetServer::start:: registerReadFileDescriptor(pTelnetHandler) is failed! addr={}\n", 
					pNewEndPoint->c_str()));
				
				delete pTelnetHandler;
				continue;
			}

			INFO_MSG(fmt::format("TelnetServer::handleInputNotification: new handler({})!\n",
				pNewEndPoint->c_str()));

			handlers_[(*pNewEndPoint)].reset(pTelnetHandler);

			std::string s;

			if(passwd_.size() > 0)
			{
				s = "password:";
			}
			else
			{
				s = pTelnetHandler->getWelcome();
			}

			pNewEndPoint->send(s.c_str(), (int)s.size());
		}
	}

	return 0;
}

//-------------------------------------------------------------------------------------
}

