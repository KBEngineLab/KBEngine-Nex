// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

#ifndef KBE_COMPLETION_POLLER_H
#define KBE_COMPLETION_POLLER_H

#include "event_poller.h"
#include "completion_rearm_queue.h"
#include "completion_tcp_send_queue.h"
#include "completion_udp_send_budget.h"

#include <deque>
#include <memory>

namespace KBEngine {
namespace Network
{

// Completion processing is bounded per tick so a burst cannot starve timers and application work.
// 每个 tick 限制 completion 处理量，避免网络突发饿死定时器和应用逻辑。
static const uint32 COMPLETION_MAX_COMPLETIONS_PER_TICK = 256;

// Zero keeps the time budget disabled until a runtime watcher/configuration is added.
// 零值表示暂不启用时间预算，待运行时 watcher/配置接入后再开放动态调节。
static const uint32 COMPLETION_MAX_PROCESSING_TIME_MS = 0;

class CompletionPoller : public EventPoller
{
public:
	// 构造 completion poller 的共享队列状态。
	CompletionPoller();

	// 析构时清理已经 accept 但尚未交给上层的 socket。
	~CompletionPoller() override;

	// completion poller 会完整接管 accept/recv/send 的完成结果。
	bool supportsCompletion() const override;

	// 从共享 accept 队列取出一个完成的连接。
	bool takeAcceptedSocket(KBESOCKET fd, KBESOCKET& acceptedSocket) override;

	// 从共享 TCP 队列取出一段完成的接收数据或错误。
	bool takeTcpReceivedData(KBESOCKET fd, std::vector<char>& data, bool& disconnected, int& errorCode) override;

	// 从共享 UDP 队列取出一个完成的 datagram。
	bool takeUdpReceivedData(KBESOCKET fd, std::vector<char>& data, Address& srcAddr, int& errorCode) override;

	// 将 TCP 发送数据排入 completion poller 的发送队列。
	bool queueTcpSend(KBESOCKET fd, const void* data, int len) override;

	// 将 UDP 发送数据排入 completion poller 的发送队列。
	bool queueUdpSend(KBESOCKET fd, const void* data, int len, const Address& dstAddr) override;

	// 查询指定 fd 是否还有未完成或待投递的发送数据。
	bool hasPendingSend(KBESOCKET fd) const override;
	uint32 pendingRearmCount() const override;
	uint64 rearmAttemptCount() const override;
	uint64 rearmRetryCount() const override;
	uint64 tcpSendOwnershipTransferCount() const override;
	uint64 tcpSendBatchCopyCount() const override;
	uint64 tcpSendBatchCopiedBytes() const override;
	uint64 tcpSendBacklogBytes() const override;
	uint64 tcpSendBacklogPeakBytes() const override;
	uint64 tcpSendBackpressureCount() const override;
	uint64 tcpSendOversizedRejectCount() const override;
	uint64 tcpPartialSendCount() const override;
	uint64 receiveOwnershipTransferCount() const override;
	uint64 receiveOwnershipTransferredBytes() const override;
	uint64 udpSendBacklogBytes() const override;
	uint64 udpSendBacklogPeakBytes() const override;
	uint64 udpSendBackpressureCount() const override;
	uint64 completionProcessRounds() const override;
	uint64 completionProcessedCount() const override;
	uint64 completionLastBatchCount() const override;
	uint64 completionMaxBatchCount() const override;
	uint64 completionBudgetExhaustionCount() const override;
	uint64 completionConsecutiveBudgetExhaustions() const override;
	uint64 completionMaxConsecutiveBudgetExhaustions() const override;

protected:
	enum SocketKind
	{
		SOCKET_KIND_UNKNOWN = 0,
		SOCKET_KIND_TCP,
		SOCKET_KIND_UDP,
		SOCKET_KIND_LISTENER
	};

	struct PendingUdpSend
	{
		// 保存一次 UDP sendto 的数据和目的地址。
		std::vector<char> data;
		sockaddr_in dstAddr;
	};

	struct SocketState
	{
		// 为一个 fd 保存 completion 生命周期和发送队列。
		explicit SocketState(KBESOCKET socketArg);

		KBESOCKET socket;
		SocketKind kind;
		// associated 表示平台 poller 已经把 fd 绑定到 completion 后端。
		bool associated;
		bool registeredRead;
		// readBackpressured 只用于 readiness-adapter 类后端（目前是 kqueue）。
		// IOCP/io_uring 是真正的 completion 模型，不用用户态队列水位去停读；
		// kqueue 是 readiness adapter，如果队列满时不显式 EV_DISABLE，
		// 内核会反复报告同一个可读 fd，导致主循环醒来却无法做有效工作。
		bool readBackpressured;
		// readArmed/writeArmed 用于 io_uring/kqueue adapter 标记 outstanding 操作。
		bool readArmed;
		bool writeArmed;
		// TCP 读侧已经收到 EOF/错误这类终止 completion。
		// 这个标记不是背压：它只表示这个 fd 的 TCP 字节流生命周期已经结束，
		// 后端不能再继续投递 recv，否则断开的 socket 会不断返回 0 字节/错误 completion。
		bool tcpReadTerminated;
		uint64 generation;
		// 平台私有 pending context，IOCP 用它保存 OVERLAPPED context 指针。
		void* pPendingReadContext;
		void* pPendingWriteContext;
		// TCP 发送队列保存上层已交给 poller、但尚未完成发送的数据。
		CompletionTcpSendQueue pendingTcpSends;
		std::deque<PendingUdpSend> pendingUdpSends;
		size_t pendingUdpSendBytes;
		CompletionUdpSendBudget pendingUdpSendBudget;
		// 接收 completion 队列字节数用于 kqueue readiness adapter 背压和诊断。
		// 注意断开、错误这类 completion 可能没有 data，不能只靠 bytes 判断队列为空；
		// cleanupStateIfUnused 还会同时检查 map/deque 里的 item 数。
		size_t pendingTcpReceiveBytes;
		size_t pendingUdpReceiveBytes;
#if KBE_PLATFORM == PLATFORM_WIN32
		// AcceptEx 函数指针随监听 socket 缓存在共享状态里。
		LPFN_ACCEPTEX acceptExFn;
#endif
	};

	enum RearmFlags
	{
		REARM_NONE = CompletionRearmQueue::NONE,
		REARM_READ = CompletionRearmQueue::READ,
		REARM_WRITE = CompletionRearmQueue::WRITE,
		REARM_ALL = CompletionRearmQueue::ALL
	};

	typedef std::unique_ptr<SocketState> SocketStatePtr;
	typedef std::map<KBESOCKET, SocketStatePtr> SocketStates;
	typedef std::deque<KBESOCKET> AcceptedSockets;
	typedef std::map<KBESOCKET, AcceptedSockets> AcceptedSocketMap;
	typedef std::deque<TcpCompletionData> TcpReceivedQueue;
	typedef std::map<KBESOCKET, TcpReceivedQueue> TcpReceivedMap;
	typedef std::deque<UdpCompletionData> UdpReceivedQueue;
	typedef std::map<KBESOCKET, UdpReceivedQueue> UdpReceivedMap;

	// 获取或创建 fd 对应的共享 socket 状态。
	SocketState& socketStateForFd(KBESOCKET fd);

	// 尝试根据 socket 选项识别 TCP/UDP/listener 类型。
	bool tryDetermineSocketKind(KBESOCKET socket, SocketKind& kind) const;

	// completion 到达后将 accepted socket 放入共享队列。
	bool pushAcceptedSocket(KBESOCKET fd, KBESOCKET acceptedSocket);

	// completion 到达后将 TCP 数据或错误放入共享队列。
	bool pushTcpReceivedData(KBESOCKET fd, std::vector<char>& data, bool disconnected, int errorCode);

	// 判断一次 TCP completion 是否表示读侧生命周期结束。
	// EOF、ECONNRESET、发送失败转读侧错误都应该只交给上层一次；
	// 普通 payload completion 不能设置终止标记，否则会错误停止后续 recv。
	bool isTcpTerminalCompletion(const std::vector<char>& data, bool disconnected, int errorCode) const;

	// completion 到达后将 UDP datagram 放入共享队列。
	bool pushUdpReceivedData(KBESOCKET fd, std::vector<char>& data, const sockaddr_in& srcAddr, int errorCode);

	// 判断 TCP 接收 completion 队列是否仍允许继续缓存数据。
	bool canQueueTcpReceivedData(KBESOCKET fd, size_t len) const;

	// 判断 UDP 接收 completion 队列是否仍允许继续缓存数据。
	bool canQueueUdpReceivedData(KBESOCKET fd, size_t len) const;

	// 清空一个 fd 的 TCP/UDP 发送队列，并同步归零 backlog 字节数。
	// 读注销、写注销和 fd 生命周期重置都会走到这类清理路径；集中到基类后，
	// IOCP/io_uring/kqueue 不需要各自维护“clear deque + 清 byte 计数”的重复代码。
	void clearPendingSends(SocketState& state);

	// 从 TCP 发送队列头部取出一个有界 batch。
	// completion 后端都会把多个小包合并成一次系统发送，以减少 completion 数量；
	// 但合并时必须保持字节流顺序，并正确扣减队列内部 pending bytes。
	// 如果队首包只取走一部分，剩余内容继续留在队首，下一次发送接着发。
	bool popTcpSendBatch(SocketState& state, size_t maxBytes, CompletionTcpSendBuffer& batch, bool& copied);

	// 把一次部分完成的 TCP send 剩余字节放回队首。
	// WSASend/io_uring send 都可能只完成部分字节；剩余数据必须插回队首，
	// 否则后续排队数据会越过它，破坏 TCP 字节流顺序。
	bool pushTcpSendFront(SocketState& state, CompletionTcpSendBuffer& data, size_t consumedBytes);

	// 统一维护 UDP 队列的总字节数和目标地址字节数，供 IOCP/io_uring/kqueue 共享。
	// Keep aggregate and per-destination UDP queue bytes in one place for IOCP, io_uring, and kqueue.
	void dequeueUdpSend(SocketState& state, PendingUdpSend& pending);
	void restoreUdpSendFront(SocketState& state, PendingUdpSend&& pending);
	uint64 udpDestinationKey(const sockaddr_in& address) const;

	// 判断 accept/recv 队列是否还有容量。
	// 这些水位主要服务 kqueue readiness adapter：kqueue 需要先把 readiness 数据读到
	// 用户态 handoff 队列，再伪装成 completion 交给上层，因此必须能暂停 drain。
	// io_uring/IOCP 本身就是 completion，只使用队列作为 triggerRead 前后的短暂交接，
	// 不用这些 canArm* 水位去停止内核 read 投递。
	bool canQueueAcceptedSocket(KBESOCKET fd) const;
	bool canArmTcpReceive(KBESOCKET fd) const;
	bool canArmUdpReceive(KBESOCKET fd) const;

	// 低水位恢复判断。
	// 暂停读之后不应刚消费 1 个 item 就立刻恢复，否则会在高水位附近来回 enable/disable；
	// 这里用 1/2 队列水位作为滞回区间，让恢复更平滑。
	bool shouldResumeTcpReceive(KBESOCKET fd) const;
	bool shouldResumeUdpReceive(KBESOCKET fd) const;

	// 清理接收 completion 队列和对应计数。
	void clearReceivedData(KBESOCKET fd);

	// 查询队列 item，避免空错误/断开 completion 绕过 bytes 计数。
	size_t acceptedSocketCount(KBESOCKET fd) const;
	size_t tcpReceivedItemCount(KBESOCKET fd) const;
	size_t udpReceivedItemCount(KBESOCKET fd) const;

	// 关闭 accept 队列中尚未被上层接走的 socket。
	void closeAcceptedSockets(AcceptedSockets& acceptedSockets);

	// 关闭一个平台 socket，供基类清理 accepted socket 队列使用。
	void closeSocket(KBESOCKET socket);

	// 返回当前平台的无效 socket 值。
	KBESOCKET invalidSocket() const;

	// 清理一个不再有注册、pending IO 和排队数据的 fd 状态。
	void cleanupStateIfUnused(KBESOCKET fd);

	// 失败的读写投递按 fd 合并后进入 FIFO；轮转重试避免 SQ/驱动资源紧张时低编号 fd 长期抢占。
	// Merge failed read/write submissions by fd into a FIFO so rotation prevents low-numbered descriptors from monopolizing scarce SQ or driver resources.
	void requestRearm(KBESOCKET fd, uint8 flags);
	void cancelRearm(KBESOCKET fd, uint8 flags = REARM_ALL);
	bool takeRearmRequest(KBESOCKET& fd, uint8& flags);
	size_t rearmBatchSize() const;
	void recordRearmAttempt(bool retryRequired);
	// Record one poll round without scanning socket state; a zero-sized round resets a prior exhaustion streak.
	// 记录一轮 poll，无需扫描 socket 状态；空轮次也会终止此前的连续预算耗尽。
	void recordCompletionBatch(uint32 processedCount, bool budgetExhausted);

	SocketStates socketStates_;
	AcceptedSocketMap acceptedSockets_;
	TcpReceivedMap tcpReceived_;
	UdpReceivedMap udpReceived_;
	CompletionRearmQueue rearmQueue_;
	uint64 rearmAttemptCount_;
	uint64 rearmRetryCount_;
	uint64 tcpSendOwnershipTransferCount_;
	uint64 tcpSendBatchCopyCount_;
	uint64 tcpSendBatchCopiedBytes_;
	uint64 tcpSendBacklogPeakBytes_;
	uint64 tcpSendBackpressureCount_;
	uint64 tcpSendOversizedRejectCount_;
	uint64 tcpPartialSendCount_;
	uint64 receiveOwnershipTransferCount_;
	uint64 receiveOwnershipTransferredBytes_;
	uint64 udpSendBacklogPeakBytes_;
	uint64 udpSendBackpressureCount_;
	uint64 completionProcessRounds_;
	uint64 completionProcessedCount_;
	uint64 completionLastBatchCount_;
	uint64 completionMaxBatchCount_;
	uint64 completionBudgetExhaustionCount_;
	uint64 completionConsecutiveBudgetExhaustions_;
	uint64 completionMaxConsecutiveBudgetExhaustions_;
};

}
}

#endif // KBE_COMPLETION_POLLER_H
