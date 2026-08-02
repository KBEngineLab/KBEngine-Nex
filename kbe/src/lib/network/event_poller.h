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

#ifndef KBE_EVENT_POLLER_H
#define KBE_EVENT_POLLER_H

#include "common/common.h"
#include "common/timestamp.h"
#include "network/interfaces.h"
#include "thread/concurrency.h"
#include "network/common.h"
#include "network/address.h"
#include <map>
#include <vector>

namespace KBEngine { 
namespace Network
{
	
class InputNotificationHandler;
class Address;
// 事件表必须以原生 socket 宽度为键，Windows x64 不能把 SOCKET 截断成 int。
// Event tables must use native socket-width keys because Windows x64 SOCKET values cannot be truncated to int.
typedef std::map<KBESOCKET, InputNotificationHandler *> FDReadHandlers;
typedef std::map<KBESOCKET, OutputNotificationHandler *> FDWriteHandlers;

struct TcpCompletionData
{
	std::vector<char> payload;
	bool disconnected;
	int errorCode;

	// Store one TCP receive completion so upper layers consume data without probing socket readiness again.
	// 保存一次 TCP 接收完成结果，让上层消费数据时不再重复探测 socket readiness。
};

struct UdpCompletionData
{
	std::vector<char> payload;
	Address srcAddr;
	int errorCode;

	// Store one UDP datagram completion together with its source address.
	// 保存一次 UDP 数据报完成结果及其来源地址。
};

class EventPoller
{
public:
	EventPoller();
	virtual ~EventPoller();

	bool registerForRead(KBESOCKET fd, InputNotificationHandler * handler);
	bool registerForWrite(KBESOCKET fd, OutputNotificationHandler * handler);

	bool deregisterForRead(KBESOCKET fd);
	bool deregisterForWrite(KBESOCKET fd);


	virtual int processPendingEvents(double maxWait) = 0;
	virtual int getFileDescriptor() const;

	// 返回当前事件后端的稳定名称，供启动日志、诊断和测试识别实际 IO 模型。
	// Return a stable name for the active event backend so startup logs, diagnostics, and tests can identify the actual IO model.
	virtual const char* ioModelName() const;

	// readiness 后端返回 false；完成模型接入后由具体后端返回 true。
	// Readiness backends return false; a completion backend returns true after it is integrated.
	virtual bool supportsCompletion() const;

	// 从完成队列取出一个已经完成的 accept；readiness 后端保持 false，避免改变旧调用链。
	// Take one completed accept from the completion queue; readiness backends return false to preserve the old call path.
	virtual bool takeAcceptedSocket(KBESOCKET fd, KBESOCKET& acceptedSocket);

	// 从完成队列取出一段 TCP 数据或终止状态；数据所有权在调用方接收后转移。
	// Take TCP data or a terminal state from the completion queue; ownership transfers to the caller on success.
	virtual bool takeTcpReceivedData(KBESOCKET fd, std::vector<char>& data, bool& disconnected, int& errorCode);

	// 从完成队列取出一个 UDP 数据报及其来源地址。
	// Take one UDP datagram and its source address from the completion queue.
	virtual bool takeUdpReceivedData(KBESOCKET fd, std::vector<char>& data, Address& srcAddr, int& errorCode);

	// 将 TCP 数据交给完成后端排队；旧后端返回 false，继续使用原有 PacketSender 发送路径。
	// Queue TCP data in a completion backend; legacy backends return false and keep the original PacketSender path.
	virtual bool queueTcpSend(KBESOCKET fd, const void* data, int len);

	// 将 UDP 数据报交给完成后端排队；旧后端返回 false，保持原有 UDP 发送语义。
	// Queue a UDP datagram in a completion backend; legacy backends return false and preserve existing UDP send semantics.
	virtual bool queueUdpSend(KBESOCKET fd, const void* data, int len, const Address& dstAddr);

	// 查询指定 socket 是否仍有完成后端待发送数据，用于避免重复注册写事件。
	// Report whether a socket still has pending completion-backend sends to avoid duplicate write registration.
	virtual bool hasPendingSend(KBESOCKET fd) const;

	// 返回 completion 后端重新投递队列的诊断指标；readiness 后端始终返回 0。
	// Return diagnostic counters for the completion rearm queue; readiness backends always report zero.
	virtual uint32 pendingRearmCount() const;
	virtual uint64 rearmAttemptCount() const;
	virtual uint64 rearmRetryCount() const;
	virtual uint64 contextAllocationCount() const;
	virtual uint64 contextReuseCount() const;
	virtual uint64 contextOutstandingCount() const;
	virtual uint64 contextCachedCount() const;
	virtual uint64 contextPeakOutstandingCount() const;
	virtual uint64 contextOutstandingBytes() const;
	virtual uint64 contextCachedBytes() const;
	virtual uint64 tcpSendOwnershipTransferCount() const;
	virtual uint64 tcpSendBatchCopyCount() const;
	virtual uint64 tcpSendBatchCopiedBytes() const;
	virtual uint64 tcpSendBacklogBytes() const;
	virtual uint64 tcpSendBacklogPeakBytes() const;
	virtual uint64 tcpSendBackpressureCount() const;
	virtual uint64 tcpSendOversizedRejectCount() const;
	virtual uint64 tcpPartialSendCount() const;
	virtual uint64 receiveOwnershipTransferCount() const;
	virtual uint64 receiveOwnershipTransferredBytes() const;
	virtual uint64 udpSendBacklogBytes() const;
	virtual uint64 udpSendBacklogPeakBytes() const;
	virtual uint64 udpSendBackpressureCount() const;
	virtual uint64 completionProcessRounds() const;
	virtual uint64 completionProcessedCount() const;
	virtual uint64 completionLastBatchCount() const;
	virtual uint64 completionMaxBatchCount() const;
	virtual uint64 completionBudgetExhaustionCount() const;
	virtual uint64 completionConsecutiveBudgetExhaustions() const;
	virtual uint64 completionMaxConsecutiveBudgetExhaustions() const;

	void clearSpareTime()		{spareTime_ = 0;}
	uint64 spareTime() const	{return spareTime_;}

	static EventPoller * create();

	// 根据当前编译平台返回 1.x 基线默认后端名称，不改变后端选择逻辑。
	// Return the 1.x baseline backend name for the build platform without changing backend selection logic.
	static const char* defaultIOModelName();

	InputNotificationHandler* findForRead(KBESOCKET fd);
	OutputNotificationHandler* findForWrite(KBESOCKET fd);

protected:
	virtual bool doRegisterForRead(KBESOCKET fd) = 0;
	virtual bool doRegisterForWrite(KBESOCKET fd) = 0;

	virtual bool doDeregisterForRead(KBESOCKET fd) = 0;
	virtual bool doDeregisterForWrite(KBESOCKET fd) = 0;

	bool triggerRead(KBESOCKET fd);
	bool triggerWrite(KBESOCKET fd);
	bool triggerError(KBESOCKET fd);
	
	bool isRegistered(KBESOCKET fd, bool isForRead) const;

	KBESOCKET maxFD() const;

private:
	FDReadHandlers fdReadHandlers_;
	FDWriteHandlers fdWriteHandlers_;

protected:
	uint64 spareTime_;
};

}
}
#endif // KBE_EVENT_POLLER_H
