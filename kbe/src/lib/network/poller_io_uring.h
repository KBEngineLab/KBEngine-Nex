// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

#ifndef KBE_IO_URING_POLLER_H
#define KBE_IO_URING_POLLER_H

#include "poller_completion.h"
#include "completion_context_pool.h"
#include "completion_udp_receive_depth.h"

#if defined(__linux__)

#include <linux/io_uring.h>
#include <deque>
#include <map>
#include <set>
#include <sys/uio.h>

namespace KBEngine {
namespace Network
{

class IoUringPoller : public CompletionPoller
{
public:
	// 创建 io_uring 队列并初始化共享 completion 状态。
	explicit IoUringPoller(uint32 entries = 4096);

	// 注销 ring 映射和 fd，释放 outstanding context。
	~IoUringPoller() override;

	// 返回 io_uring fd，便于诊断工具识别当前 poller。
	int getFileDescriptor() const override { return ringFd_; }

	// 等待并处理一批 io_uring CQE。
	int processPendingEvents(double maxWait) override;

	// TCP 空闲时立即投递首包，outstanding 期间合并后续小包；UDP/KCP 保持立即投递。
	bool queueTcpSend(KBESOCKET fd, const void* data, int len, size_t maxPendingBytes = 0) override;
	bool queueUdpSend(KBESOCKET fd, const void* data, int len, const Address& dstAddr) override;
	uint64 contextAllocationCount() const override;
	uint64 contextReuseCount() const override;
	uint64 contextOutstandingCount() const override;
	uint64 contextCachedCount() const override;
	uint64 contextPeakOutstandingCount() const override;
	uint64 contextOutstandingBytes() const override;
	uint64 contextCachedBytes() const override;
	uint64 completionDequeueCallCount() const override;
	uint64 completionDequeuedCount() const override;
	uint64 completionMaxDequeuedBatchCount() const override;
	uint64 completionPendingLocalCount() const override;
	uint64 tcpSendSubmissionCount() const override;
	uint64 tcpSendSubmittedBytes() const override;
	uint64 tcpSendMaxSubmissionBytes() const override;
	uint64 ioUringSubmitCallCount() const override;
	uint64 ioUringSubmitFailureCount() const override;
	uint64 ioUringSubmitPartialCount() const override;
	uint64 ioUringSqCapacityExhaustionCount() const override;
	uint64 ioUringSqDroppedCount() const override;
	uint64 ioUringCqOverflowCount() const override;
	uint64 ioUringCancelRequestCount() const override;
	uint64 ioUringCancelCompletionCount() const override;
	uint64 ioUringStaleCompletionCount() const override;
	uint64 ioUringUdpReceiveDepthDeficitCount() const override;
	uint64 ioUringUdpReceiveWouldBlockCount() const override;
	uint64 ioUringSqEntryCount() const override;
	uint64 ioUringCqEntryCount() const override;

protected:
	// 注册读侧时投递 accept/recv/recvmsg completion。
	bool doRegisterForRead(KBESOCKET fd) override;

	// 注册写侧只保存 handler，真实发送由 queueTcpSend/queueUdpSend 驱动。
	bool doRegisterForWrite(KBESOCKET fd) override;

	// 注销读侧时让迟到 completion 通过 generation 自动丢弃。
	bool doDeregisterForRead(KBESOCKET fd) override;

	// 注销写侧时清空发送队列并让迟到 send completion 自动丢弃。
	bool doDeregisterForWrite(KBESOCKET fd) override;

private:
	enum Operation
	{
		OP_ACCEPT = 0,
		OP_TCP_RECV,
		OP_UDP_RECV,
		OP_TCP_SEND,
		OP_UDP_SEND
	};

	struct IoUringContext
	{
		// 每个 SQE 绑定一个 context，CQE 回来前所有缓冲和 msghdr 必须保持有效。
		IoUringContext();
		void reset(KBESOCKET fdArg, KBESOCKET socketArg, SocketKind kindArg, Operation operationArg,
			uint64 generationArg, uint64 requestIdArg);
		size_t retainedBytes() const;

		KBESOCKET fd;
		KBESOCKET socket;
		SocketKind kind;
		Operation operation;
		uint64 generation;
		uint64 requestId;
		// 注销已发起后，内核仍会返回原操作的 CQE；这类预期迟到完成等价于 IOCP 的 ERROR_OPERATION_ABORTED，不属于 stale 异常。
		// Once deregistration starts, the kernel still returns the original CQE; this expected late completion matches IOCP ERROR_OPERATION_ABORTED and is not a stale anomaly.
		bool expectedLateCompletion;
		std::vector<char> data;
		CompletionTcpSendBuffer tcpSendData;
		sockaddr_in addr;
		socklen_t addrLen;
		iovec iov;
		msghdr msg;
	};

	struct Ring
	{
		// 保存 io_uring SQ/CQ mmap 后的指针。
		Ring();

		unsigned* sqHead;
		unsigned* sqTail;
		unsigned* sqRingMask;
		unsigned* sqRingEntries;
		unsigned* sqFlags;
		unsigned* sqDropped;
		unsigned* sqArray;
		io_uring_sqe* sqes;
		// SQE 使用本地游标预留，只有 submitSqes() 才把完整条目发布给内核。
		// SQEs are reserved with local cursors and published to the kernel only after submitSqes() sees complete entries.
		unsigned sqeHead;
		unsigned sqeTail;
		unsigned* cqHead;
		unsigned* cqTail;
		unsigned* cqRingMask;
		unsigned* cqRingEntries;
		unsigned* cqOverflow;
		io_uring_cqe* cqes;
		void* sqRingPtr;
		size_t sqRingSize;
		void* cqRingPtr;
		size_t cqRingSize;
		void* sqesPtr;
		size_t sqesSize;
	};

	// 将 maxWait 转换成 poll 可使用的毫秒数。
	static int toTimeoutMilliseconds(double maxWait);

	// 初始化 io_uring fd 和 mmap 区域。
	bool setupRing(uint32 entries);

	// 释放 ring 的 mmap 区域和 fd。
	void destroyRing();

	// 从 SQ ring 取一个可写 SQE。
	io_uring_sqe* getSqe();

	// 提交已经填好的 SQE。
	bool submitSqes();
	// 探测当前内核是否支持本后端依赖的 opcode；cancel 不支持时保留 generation 回退。
	bool probeOperations();
	bool operationSupported(uint8 operation) const;

	// 投递指定 fd 的读侧请求。
	bool ensureReadArmed(KBESOCKET fd, SocketState& state);
	bool ensureUdpReadsArmed(KBESOCKET fd, SocketState& state);
	uint32 udpReceiveDepth(const SocketState& state) const;
	uint32 udpReceiveBurstSize(const SocketState& state) const;
	bool isUdpConnected(const SocketState& state) const;
	bool isReadArmComplete(const SocketState& state) const;
	// 处理上一轮因 SQ 容量或暂时性错误未能投递的 fd。
	// Process descriptors that could not be submitted in the previous round because of SQ capacity or transient errors.
	void processRearmRequests();

	// 投递 accept completion。
	bool armAccept(KBESOCKET fd, SocketState& state);

	// 投递 TCP recv completion。
	bool armTcpRead(KBESOCKET fd, SocketState& state);

	// 投递 UDP recvmsg completion。
	bool armUdpRead(KBESOCKET fd, SocketState& state);

	// 投递 TCP send completion。
	bool armTcpSend(KBESOCKET fd, SocketState& state);
	// 在业务回调仍运行时仅收割当前 socket 已成功的 send completion，保持单 outstanding 和字节顺序。
	// Harvest only a successful send completion for the current socket while business code is still running, preserving one outstanding request and byte order.
	bool progressTcpSend(KBESOCKET fd, SocketState& state);

	// 投递 UDP sendmsg completion。
	bool armUdpSend(KBESOCKET fd, SocketState& state);
	// 按 user_data 精确取消仍由内核持有的请求；SQ 暂满时保留到下一轮。
	void requestCancel(IoUringContext* context);
	bool armCancel(uint64 requestId);
	void processCancelRequests();
	void cancelStateContexts(SocketState& state, bool includeReads, bool includeWrite);

	// 处理一个 CQE 并触发对应上层通知。
	void handleCompletion(IoUringContext& context, int result);
	struct PendingCompletion
	{
		PendingCompletion();

		uint64 requestId;
		int result;
		KBESOCKET preparedUdpFd;
		KBESOCKET preparedUdpSocket;
		uint64 preparedUdpGeneration;
		bool preparedUdp;
		bool notifyRead;
	};
	size_t dequeueCompletions();
	void prepareUdpCompletions(uint64 maxPendingLocal);
	// 注销 fd 时移除已经从有效 CQE 转换出的本地 UDP 通知；它们不再属于新的 socket generation。
	// Remove local UDP notifications converted from valid CQEs when an fd is deregistered;
	// they must never cross into a new socket generation.
	void discardPreparedUdpCompletions(KBESOCKET fd, KBESOCKET socket, uint64 generation);
	// Outstanding recvmsg requests absorb the first burst like IOCP receive buffers.
	// A bounded synchronous drain then consumes datagrams that arrived beyond that depth
	// without allowing one shared listener to exceed the dispatcher fairness watermark.
	// outstanding recvmsg 请求像 IOCP 接收 buffer 一样先吸收突发；随后仅在 dispatcher
	// 公平性水位内同步搬运超出该深度的数据报，避免共享 listener 无界占用主线程。
	uint32 drainUdpReceiveBurst(KBESOCKET fd, uint32 maxDatagrams);

	// 记录/移除一个已交给 io_uring 的 context。
	// SocketState 里的 pPending*Context 只表示“仍属于当前 fd 生命周期”的请求；
	// outstandingContexts_ 则覆盖所有尚未收到 CQE 的请求，包括注销后等待迟到 CQE 的旧请求。
	void trackContext(IoUringContext* context);
	void untrackContext(IoUringContext* context);
	IoUringContext* acquireContext(KBESOCKET fd, KBESOCKET socket, SocketKind kind, Operation operation, uint64 generation);
	void recycleContext(IoUringContext* context);
	uint64 nextRequestId();
	uint64 nextSocketGeneration();

	int ringFd_;
	Ring ring_;
	std::map<uint64, IoUringContext*> outstandingContexts_;
	std::set<uint64> pendingCancelRequestIds_;
	std::set<uint8> supportedOperations_;
	CompletionContextPool<IoUringContext> contextPool_;
	uint64 lastCompletionBudgetWarningTime_;
	unsigned lastSqDropped_;
	unsigned lastCqOverflow_;
	bool supportsAsyncCancel_;
	// Kernel CQEs stay separate from staged UDP datagrams. Real TCP/control
	// completions must remain visible even while a shared KCP listener has a
	// large user-space receive backlog.
	// 内核 CQE 与已搬入用户态的 UDP 数据报分队列保存。即使共享 KCP listener
	// 存在大量接收积压，真实 TCP/控制 completion 也必须保持可调度。
	std::deque<PendingCompletion> pendingCompletions_;
	// TCP send completion is separated from receive/control work so a burst of
	// synchronous Entity callbacks cannot delay advancing the internal-channel send queue.
	// TCP 发送完成与接收/控制工作分队列，避免同步 Entity 回调突发阻塞内部通道发送队列推进。
	std::deque<PendingCompletion> pendingTcpSendCompletions_;
	std::deque<PendingCompletion> pendingUdpCompletions_;
	// Limit consecutive send completions so receive/control work still progresses.
	// 限制连续发送完成数量，保证接收和控制事件仍能持续推进。
	uint32 consecutiveTcpSendCompletionCount_;
	// 跨 dispatcher 轮次限制 TCP/控制优先 burst，避免最小 completion 预算下 UDP 永久饿饿。
	// Bound TCP/control priority across dispatcher rounds so UDP still progresses with a one-completion budget.
	uint32 consecutiveNonUdpCompletionCount_;
	uint64 nextRequestId_;
	uint64 nextSocketGeneration_;
	uint64 completionDequeueCallCount_;
	uint64 completionDequeuedCount_;
	uint64 completionMaxDequeuedBatchCount_;
	uint64 tcpSendSubmissionCount_;
	uint64 tcpSendSubmittedBytes_;
	uint64 tcpSendMaxSubmissionBytes_;
	uint64 submitCallCount_;
	uint64 submitFailureCount_;
	uint64 submitPartialCount_;
	uint64 sqCapacityExhaustionCount_;
	uint64 cancelRequestCount_;
	uint64 cancelCompletionCount_;
	uint64 staleCompletionCount_;
	uint64 udpReceiveDepthDeficitCount_;
	uint64 udpReceiveWouldBlockCount_;
};

}
}

#endif // defined(__linux__)

#endif // KBE_IO_URING_POLLER_H
