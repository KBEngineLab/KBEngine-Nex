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


#include "event_poller.h"
#include "poller_epoll.h"
#include "poller_iocp.h"
#include "poller_kqueue.h"
#include "poller_io_uring.h"
#include "helper/profile.h"

namespace KBEngine { 
namespace Network
{
	
//-------------------------------------------------------------------------------------
EventPoller::EventPoller() : 
	fdReadHandlers_(), 
	fdWriteHandlers_(), 
	spareTime_(0)
{
}

//-------------------------------------------------------------------------------------
EventPoller::~EventPoller()
{
}

//-------------------------------------------------------------------------------------
bool EventPoller::registerForRead(KBESOCKET fd,
		InputNotificationHandler * handler)
{
	if (!this->doRegisterForRead(fd))
	{
		return false;
	}

	fdReadHandlers_[ fd ] = handler;

	return true;
}

//-------------------------------------------------------------------------------------
bool EventPoller::registerForWrite(KBESOCKET fd,
		OutputNotificationHandler * handler)
{
	if (!this->doRegisterForWrite(fd))
	{
		return false;
	}

	fdWriteHandlers_[ fd ] = handler;

	return true;
}

//-------------------------------------------------------------------------------------
bool EventPoller::deregisterForRead(KBESOCKET fd)
{
	fdReadHandlers_.erase(fd);

	return this->doDeregisterForRead(fd);
}

//-------------------------------------------------------------------------------------
bool EventPoller::deregisterForWrite(KBESOCKET fd)
{
	fdWriteHandlers_.erase(fd);

	return this->doDeregisterForWrite(fd);
}

//-------------------------------------------------------------------------------------
bool EventPoller::triggerRead(KBESOCKET fd)
{
	FDReadHandlers::iterator iter = fdReadHandlers_.find(fd);

	if (iter == fdReadHandlers_.end())
	{
		return false;
	}

	iter->second->handleInputNotification(fd);

	return true;
}

//-------------------------------------------------------------------------------------
bool EventPoller::triggerWrite(KBESOCKET fd)
{
	FDWriteHandlers::iterator iter = fdWriteHandlers_.find(fd);

	if (iter == fdWriteHandlers_.end())
	{
		return false;
	}

	iter->second->handleOutputNotification(fd);

	return true;
}

//-------------------------------------------------------------------------------------
bool EventPoller::triggerError(KBESOCKET fd)
{
	if (!this->triggerRead(fd))
	{
		return this->triggerWrite(fd);
	}

	return true;
}

//-------------------------------------------------------------------------------------
bool EventPoller::isRegistered(KBESOCKET fd, bool isForRead) const
{
	return isForRead ? (fdReadHandlers_.find(fd) != fdReadHandlers_.end()) : 
		(fdWriteHandlers_.find(fd) != fdWriteHandlers_.end());
}

//-------------------------------------------------------------------------------------
InputNotificationHandler* EventPoller::findForRead(KBESOCKET fd)
{
	FDReadHandlers::iterator iter = fdReadHandlers_.find(fd);
	
	if(iter == fdReadHandlers_.end())
		return NULL;

	return iter->second;
}

//-------------------------------------------------------------------------------------
OutputNotificationHandler* EventPoller::findForWrite(KBESOCKET fd)
{
	FDWriteHandlers::iterator iter = fdWriteHandlers_.find(fd);
	
	if(iter == fdWriteHandlers_.end())
		return NULL;

	return iter->second;
}

//-------------------------------------------------------------------------------------
int EventPoller::getFileDescriptor() const
{
	return -1;
}

//-------------------------------------------------------------------------------------
const char* EventPoller::ioModelName() const
{
	return EventPoller::defaultIOModelName();
}

//-------------------------------------------------------------------------------------
bool EventPoller::supportsCompletion() const
{
	return false;
}

// 这些默认实现刻意保持无操作，确保旧 select/epoll 后端在完成模型接入期间可以继续编译和运行。
// These default implementations are intentionally no-ops so legacy select/epoll backends keep compiling and running while completion support is introduced.

//-------------------------------------------------------------------------------------
bool EventPoller::takeAcceptedSocket(KBESOCKET fd, KBESOCKET& acceptedSocket)
{
	(void)fd;
	(void)acceptedSocket;
	return false;
}

//-------------------------------------------------------------------------------------
bool EventPoller::takeTcpReceivedData(KBESOCKET fd, std::vector<char>& data, bool& disconnected, int& errorCode)
{
	(void)fd;
	(void)data;
	disconnected = false;
	errorCode = 0;
	return false;
}

//-------------------------------------------------------------------------------------
bool EventPoller::takeUdpReceivedData(KBESOCKET fd, std::vector<char>& data, Address& srcAddr, int& errorCode)
{
	(void)fd;
	(void)data;
	(void)srcAddr;
	errorCode = 0;
	return false;
}

//-------------------------------------------------------------------------------------
bool EventPoller::queueTcpSend(KBESOCKET fd, const void* data, int len)
{
	(void)fd;
	(void)data;
	(void)len;
	return false;
}

//-------------------------------------------------------------------------------------
bool EventPoller::queueUdpSend(KBESOCKET fd, const void* data, int len, const Address& dstAddr)
{
	(void)fd;
	(void)data;
	(void)len;
	(void)dstAddr;
	return false;
}

//-------------------------------------------------------------------------------------
bool EventPoller::hasPendingSend(KBESOCKET fd) const
{
	(void)fd;
	return false;
}

//-------------------------------------------------------------------------------------
uint32 EventPoller::pendingRearmCount() const
{
	return 0;
}

//-------------------------------------------------------------------------------------
uint64 EventPoller::rearmAttemptCount() const
{
	return 0;
}

//-------------------------------------------------------------------------------------
uint64 EventPoller::rearmRetryCount() const
{
	return 0;
}

//-------------------------------------------------------------------------------------
uint64 EventPoller::contextAllocationCount() const { return 0; }
uint64 EventPoller::contextReuseCount() const { return 0; }
uint64 EventPoller::contextOutstandingCount() const { return 0; }
uint64 EventPoller::contextCachedCount() const { return 0; }
uint64 EventPoller::contextPeakOutstandingCount() const { return 0; }
uint64 EventPoller::contextOutstandingBytes() const { return 0; }
uint64 EventPoller::contextCachedBytes() const { return 0; }
uint64 EventPoller::tcpSendOwnershipTransferCount() const { return 0; }
uint64 EventPoller::tcpSendBatchCopyCount() const { return 0; }
uint64 EventPoller::tcpSendBatchCopiedBytes() const { return 0; }
uint64 EventPoller::tcpSendBacklogBytes() const { return 0; }
uint64 EventPoller::tcpSendBacklogPeakBytes() const { return 0; }
uint64 EventPoller::tcpSendBackpressureCount() const { return 0; }
uint64 EventPoller::tcpSendOversizedRejectCount() const { return 0; }
uint64 EventPoller::tcpPartialSendCount() const { return 0; }
uint64 EventPoller::receiveOwnershipTransferCount() const { return 0; }
uint64 EventPoller::receiveOwnershipTransferredBytes() const { return 0; }
uint64 EventPoller::udpSendBacklogBytes() const { return 0; }
uint64 EventPoller::udpSendBacklogPeakBytes() const { return 0; }
uint64 EventPoller::udpSendBackpressureCount() const { return 0; }

//-------------------------------------------------------------------------------------
const char* EventPoller::defaultIOModelName()
{
#if KBE_PLATFORM == PLATFORM_WIN32
	return "iocp completion";
#elif defined(__linux__)
	return "io_uring completion";
#elif defined(HAS_KQUEUE)
	return "kqueue completion adapter";
#elif defined(HAS_EPOLL)
	return "epoll readiness";
#else
	return "unsupported IO backend";
#endif // KBE_PLATFORM == PLATFORM_WIN32
}

//-------------------------------------------------------------------------------------
KBESOCKET EventPoller::maxFD() const
{
	KBESOCKET readMaxFD = 0;

	FDReadHandlers::const_iterator iFDReadHandler = fdReadHandlers_.begin();
	while (iFDReadHandler != fdReadHandlers_.end())
	{
		if (iFDReadHandler->first > readMaxFD)
		{
			readMaxFD = iFDReadHandler->first;
		}

		++iFDReadHandler;
	}

	KBESOCKET writeMaxFD = 0;

	FDWriteHandlers::const_iterator iFDWriteHandler = fdWriteHandlers_.begin();
	while (iFDWriteHandler != fdWriteHandlers_.end())
	{
		if (iFDWriteHandler->first > writeMaxFD)
		{
			writeMaxFD = iFDWriteHandler->first;
		}

		++iFDWriteHandler;
	}

	return std::max(readMaxFD, writeMaxFD);
}

//-------------------------------------------------------------------------------------
EventPoller * EventPoller::create()
{
#if KBE_PLATFORM == PLATFORM_WIN32
	return new IocpPoller();
#elif defined(__linux__)
	return new IoUringPoller();
#elif defined(HAS_KQUEUE)
	return new KqueuePoller();
#elif defined(HAS_EPOLL)
	return new EpollPoller();
#else
	// 没有完成模型后端时明确失败，避免悄悄回退到已移除的 select。
	// Fail explicitly when no completion backend exists instead of silently falling back to the removed select path.
	return NULL;
#endif // KBE_PLATFORM == PLATFORM_WIN32
}

}
}
