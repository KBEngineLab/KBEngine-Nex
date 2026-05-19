// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

#ifndef KBE_EVENT_POLLER_H
#define KBE_EVENT_POLLER_H

#include "common/common.h"
#include "common/timestamp.h"
#include "network/interfaces.h"
#include "thread/concurrency.h"
#include "network/common.h"
#include "network/address.h"
#include <deque>
#include <map>
#include <vector>

namespace KBEngine { 
namespace Network
{
	
class InputNotificationHandler;
typedef std::map<int, InputNotificationHandler *> FDReadHandlers;
typedef std::map<int, OutputNotificationHandler *> FDWriteHandlers;

struct TcpCompletionData
{
	// 保存一次 TCP 接收完成结果，上层只从这里取数据，不再直接探测 socket readiness。
	std::vector<char> data;
	bool disconnected;
	int errorCode;
};

struct UdpCompletionData
{
	// 保存一次 UDP 接收完成结果，同时带上 datagram 的来源地址。
	std::vector<char> data;
	Address srcAddr;
	int errorCode;
};

class EventPoller
{
public:
	// 初始化基础 poller 的 fd-handler 映射和空闲时间统计。
	EventPoller();

	// 基类析构保证通过基类指针释放平台 poller 时调用正确析构链。
	virtual ~EventPoller();

	// 注册 fd 的读侧通知 handler，平台实现负责真正挂到内核 poller。
	bool registerForRead(int fd, InputNotificationHandler * handler);

	// 注册 fd 的写侧通知 handler，completion 模型下通常只保存 handler。
	bool registerForWrite(int fd, OutputNotificationHandler * handler);

	// 注销 fd 的读侧通知 handler，并让平台实现清理对应状态。
	bool deregisterForRead(int fd);

	// 注销 fd 的写侧通知 handler，并让平台实现清理对应状态。
	bool deregisterForWrite(int fd);

	// 等待并处理一批平台事件，返回已触发的上层事件数量。
	virtual int processPendingEvents(double maxWait) = 0;

	// 返回底层 poller fd/handle 的整数形式，默认没有可暴露 fd。
	virtual int getFileDescriptor() const;

	// 取出一个已完成的 accept 结果；非 completion poller 默认没有结果。
	virtual bool takeAcceptedSocket(int fd, KBESOCKET& acceptedSocket);

	// 取出一个已完成的 TCP 接收结果；返回 false 表示当前 fd 没有完成事件。
	virtual bool takeTcpReceivedData(int fd, std::vector<char>& data, bool& disconnected, int& errorCode);

	// 取出一个已完成的 UDP 接收结果；返回 false 表示当前 fd 没有完成事件。
	virtual bool takeUdpReceivedData(int fd, std::vector<char>& data, Address& srcAddr, int& errorCode);

	// 将 TCP 发送请求交给 completion poller；非 completion poller 默认不接管发送。
	virtual bool queueTcpSend(int fd, const void* data, int len);

	// 将 UDP 发送请求交给 completion poller；非 completion poller 默认不接管发送。
	virtual bool queueUdpSend(int fd, const void* data, int len, const Address& dstAddr);

	// 查询 fd 是否还有未完成的发送请求，用于保持 Channel 发送状态不提前完成。
	virtual bool hasPendingSend(int fd) const;

	// 表示当前 poller 是否完整接管 socket IO completion 路径。
	virtual bool supportsCompletion() const;

	// 清空本轮空闲时间统计。
	void clearSpareTime()		{spareTime_ = 0;}

	// 返回累计的 poll 等待空闲时间。
	uint64 spareTime() const	{return spareTime_;}

	// 按当前平台创建默认 completion poller。
	static EventPoller * create();

	// 返回当前平台默认使用的 IO completion 模型名称。
	static const char* defaultIOModelName();

	// 查找 fd 的读侧 handler。
	InputNotificationHandler* findForRead(int fd);

	// 查找 fd 的写侧 handler。
	OutputNotificationHandler* findForWrite(int fd);

protected:
	// 平台实现真正注册读侧 fd。
	virtual bool doRegisterForRead(int fd) = 0;

	// 平台实现真正注册写侧 fd。
	virtual bool doRegisterForWrite(int fd) = 0;

	// 平台实现真正注销读侧 fd。
	virtual bool doDeregisterForRead(int fd) = 0;

	// 平台实现真正注销写侧 fd。
	virtual bool doDeregisterForWrite(int fd) = 0;

	// 触发 fd 的读侧 handler。
	bool triggerRead(int fd);

	// 触发 fd 的写侧 handler。
	bool triggerWrite(int fd);

	// 触发 fd 的错误处理路径。
	bool triggerError(int fd);
	
	// 判断 fd 是否已经注册到读侧或写侧表。
	bool isRegistered(int fd, bool isForRead) const;

	// 返回当前注册表中的最大 fd，保留给少量诊断/兼容代码使用。
	int maxFD() const;

	// 返回读侧 handler 表的只读引用。
	const FDReadHandlers& fdReadHandlers() const { return fdReadHandlers_; }

	// 返回写侧 handler 表的只读引用。
	const FDWriteHandlers& fdWriteHandlers() const { return fdWriteHandlers_; }

private:
	FDReadHandlers fdReadHandlers_;
	FDWriteHandlers fdWriteHandlers_;

protected:
	uint64 spareTime_;
};

}
}
#endif // KBE_EVENT_POLLER_H
