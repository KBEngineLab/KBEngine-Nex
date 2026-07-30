// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

#ifndef KBE_IO_URING_POLLER_H
#define KBE_IO_URING_POLLER_H

#include "poller_completion.h"
#include "completion_context_pool.h"

#if defined(__linux__)

#include <linux/io_uring.h>
#include <set>
#include <sys/uio.h>

namespace KBEngine {
namespace Network
{

class IoUringPoller : public CompletionPoller
{
public:
	// 创建 io_uring 队列并初始化共享 completion 状态。
	explicit IoUringPoller(uint32 entries = 256);

	// 注销 ring 映射和 fd，释放 outstanding context。
	~IoUringPoller() override;

	// 返回 io_uring fd，便于诊断工具识别当前 poller。
	int getFileDescriptor() const override { return ringFd_; }

	// 等待并处理一批 io_uring CQE。
	int processPendingEvents(double maxWait) override;

	// 发送入队后立即尝试投递 SQE，减少必须等下一轮 tick 才开始发送的延迟。
	bool queueTcpSend(KBESOCKET fd, const void* data, int len) override;
	bool queueUdpSend(KBESOCKET fd, const void* data, int len, const Address& dstAddr) override;
	uint64 contextAllocationCount() const override;
	uint64 contextReuseCount() const override;
	uint64 contextOutstandingCount() const override;
	uint64 contextCachedCount() const override;
	uint64 contextPeakOutstandingCount() const override;

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
		void reset(KBESOCKET fdArg, KBESOCKET socketArg, SocketKind kindArg, Operation operationArg, uint64 generationArg);

		KBESOCKET fd;
		KBESOCKET socket;
		SocketKind kind;
		Operation operation;
		uint64 generation;
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

	// 投递指定 fd 的读侧请求。
	bool ensureReadArmed(KBESOCKET fd, SocketState& state);
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

	// 投递 UDP sendmsg completion。
	bool armUdpSend(KBESOCKET fd, SocketState& state);

	// 处理一个 CQE 并触发对应上层通知。
	void handleCompletion(IoUringContext& context, int result);

	// 记录/移除一个已交给 io_uring 的 context。
	// SocketState 里的 pPending*Context 只表示“仍属于当前 fd 生命周期”的请求；
	// outstandingContexts_ 则覆盖所有尚未收到 CQE 的请求，包括注销后等待迟到 CQE 的旧请求。
	void trackContext(IoUringContext* context);
	void untrackContext(IoUringContext* context);
	IoUringContext* acquireContext(KBESOCKET fd, KBESOCKET socket, SocketKind kind, Operation operation, uint64 generation);
	void recycleContext(IoUringContext* context);

	int ringFd_;
	Ring ring_;
	std::set<IoUringContext*> outstandingContexts_;
	CompletionContextPool<IoUringContext> contextPool_;
	uint64 lastCompletionBudgetWarningTime_;
	unsigned lastSqDropped_;
	unsigned lastCqOverflow_;
};

}
}

#endif // defined(__linux__)

#endif // KBE_IO_URING_POLLER_H
