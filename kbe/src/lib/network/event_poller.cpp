// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com


#include "event_poller.h"
#include "poller_iocp.h"
#include "poller_kqueue.h"
#include "poller_io_uring.h"
#include "helper/profile.h"

namespace KBEngine { 
namespace Network
{
	
#if KBE_PLATFORM == PLATFORM_APPLE
#define HAS_KQUEUE
#endif

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
bool EventPoller::registerForRead(int fd,
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
bool EventPoller::registerForWrite(int fd,
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
bool EventPoller::deregisterForRead(int fd)
{
	fdReadHandlers_.erase(fd);

	return this->doDeregisterForRead(fd);
}

//-------------------------------------------------------------------------------------
bool EventPoller::deregisterForWrite(int fd)
{
	fdWriteHandlers_.erase(fd);

	return this->doDeregisterForWrite(fd);
}

//-------------------------------------------------------------------------------------
bool EventPoller::triggerRead(int fd)	
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
bool EventPoller::triggerWrite(int fd)	
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
bool EventPoller::triggerError(int fd)
{
	if (!this->triggerRead(fd))
	{
		return this->triggerWrite(fd);
	}

	return true;
}

//-------------------------------------------------------------------------------------
bool EventPoller::isRegistered(int fd, bool isForRead) const
{
	return isForRead ? (fdReadHandlers_.find(fd) != fdReadHandlers_.end()) : 
		(fdWriteHandlers_.find(fd) != fdWriteHandlers_.end());
}

//-------------------------------------------------------------------------------------
InputNotificationHandler* EventPoller::findForRead(int fd)
{
	FDReadHandlers::iterator iter = fdReadHandlers_.find(fd);
	
	if(iter == fdReadHandlers_.end())
		return NULL;

	return iter->second;
}

//-------------------------------------------------------------------------------------
OutputNotificationHandler* EventPoller::findForWrite(int fd)
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
bool EventPoller::takeAcceptedSocket(int fd, KBESOCKET& acceptedSocket)
{
	// 默认 poller 不保存 accept completion，保留接口给 completion adapter 覆盖。
	(void)fd;
	(void)acceptedSocket;
	return false;
}

//-------------------------------------------------------------------------------------
bool EventPoller::takeTcpReceivedData(int fd, std::vector<char>& data, bool& disconnected, int& errorCode)
{
	// 默认 poller 不保存 TCP completion，调用方会按旧同步路径处理。
	(void)fd;
	(void)data;
	(void)disconnected;
	(void)errorCode;
	return false;
}

//-------------------------------------------------------------------------------------
bool EventPoller::takeUdpReceivedData(int fd, std::vector<char>& data, Address& srcAddr, int& errorCode)
{
	// 默认 poller 不保存 UDP completion，调用方会按旧同步路径处理。
	(void)fd;
	(void)data;
	(void)srcAddr;
	(void)errorCode;
	return false;
}

//-------------------------------------------------------------------------------------
bool EventPoller::queueTcpSend(int fd, const void* data, int len)
{
	// 默认 poller 不接管发送，返回 false 让调用方继续使用 EndPoint::send。
	(void)fd;
	(void)data;
	(void)len;
	return false;
}

//-------------------------------------------------------------------------------------
bool EventPoller::queueUdpSend(int fd, const void* data, int len, const Address& dstAddr)
{
	// 默认 poller 不接管发送，返回 false 让调用方继续使用 EndPoint::sendto。
	(void)fd;
	(void)data;
	(void)len;
	(void)dstAddr;
	return false;
}

//-------------------------------------------------------------------------------------
bool EventPoller::hasPendingSend(int fd) const
{
	// 默认 poller 没有内部发送队列。
	(void)fd;
	return false;
}

//-------------------------------------------------------------------------------------
bool EventPoller::supportsCompletion() const
{
	// 只有 IOCP/io_uring/kqueue adapter 会声明自己完整接管 completion。
	return false;
}

//-------------------------------------------------------------------------------------
const char* EventPoller::defaultIOModelName()
{
	// 统一描述当前平台默认 IO 模型，启动日志和诊断代码都从这里取值。
#if KBE_PLATFORM == PLATFORM_WIN32
	return "IOCP completion";
#elif defined(__linux__)
	return "io_uring completion";
#elif defined(HAS_KQUEUE)
	return "kqueue completion adapter";
#else
	return "unsupported completion";
#endif
}

//-------------------------------------------------------------------------------------
int EventPoller::maxFD() const
{
	int readMaxFD = -1;

	FDReadHandlers::const_iterator iFDReadHandler = fdReadHandlers_.begin();
	while (iFDReadHandler != fdReadHandlers_.end())
	{
		if (iFDReadHandler->first > readMaxFD)
		{
			readMaxFD = iFDReadHandler->first;
		}

		++iFDReadHandler;
	}

	int writeMaxFD = -1;

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
	static bool s_reportedIOModel = false;
	if (!s_reportedIOModel)
	{
		// 每个进程只输出一次当前 IO 模型，避免多 dispatcher 组件重复刷启动日志。
		INFO_MSG(fmt::format("EventPoller::create: using IO model: {}.\n", EventPoller::defaultIOModelName()));
		s_reportedIOModel = true;
	}

#if KBE_PLATFORM == PLATFORM_WIN32
	return new IocpPoller();
#elif defined(__linux__)
	return new IoUringPoller();
#elif defined(HAS_KQUEUE)
	return new KqueuePoller();
#else
	ERROR_MSG("EventPoller::create: unsupported platform without completion poller.\n");
	return NULL;
#endif
}

}
}
