// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

#ifndef KBE_COMPLETION_POLLER_H
#define KBE_COMPLETION_POLLER_H

#include "event_poller.h"

#include <deque>
#include <memory>

namespace KBEngine {
namespace Network
{

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
	bool takeAcceptedSocket(int fd, KBESOCKET& acceptedSocket) override;

	// 从共享 TCP 队列取出一段完成的接收数据或错误。
	bool takeTcpReceivedData(int fd, std::vector<char>& data, bool& disconnected, int& errorCode) override;

	// 从共享 UDP 队列取出一个完成的 datagram。
	bool takeUdpReceivedData(int fd, std::vector<char>& data, Address& srcAddr, int& errorCode) override;

	// 将 TCP 发送数据排入 completion poller 的发送队列。
	bool queueTcpSend(int fd, const void* data, int len) override;

	// 将 UDP 发送数据排入 completion poller 的发送队列。
	bool queueUdpSend(int fd, const void* data, int len, const Address& dstAddr) override;

	// 查询指定 fd 是否还有未完成或待投递的发送数据。
	bool hasPendingSend(int fd) const override;

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
		std::deque<std::vector<char> > pendingTcpSends;
		size_t pendingTcpSendBytes;
		std::deque<PendingUdpSend> pendingUdpSends;
		size_t pendingUdpSendBytes;
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

	typedef std::unique_ptr<SocketState> SocketStatePtr;
	typedef std::map<int, SocketStatePtr> SocketStates;
	typedef std::deque<KBESOCKET> AcceptedSockets;
	typedef std::map<int, AcceptedSockets> AcceptedSocketMap;
	typedef std::deque<TcpCompletionData> TcpReceivedQueue;
	typedef std::map<int, TcpReceivedQueue> TcpReceivedMap;
	typedef std::deque<UdpCompletionData> UdpReceivedQueue;
	typedef std::map<int, UdpReceivedQueue> UdpReceivedMap;

	// 获取或创建 fd 对应的共享 socket 状态。
	SocketState& socketStateForFd(int fd);

	// 尝试根据 socket 选项识别 TCP/UDP/listener 类型。
	bool tryDetermineSocketKind(KBESOCKET socket, SocketKind& kind) const;

	// completion 到达后将 accepted socket 放入共享队列。
	bool pushAcceptedSocket(int fd, KBESOCKET acceptedSocket);

	// completion 到达后将 TCP 数据或错误放入共享队列。
	bool pushTcpReceivedData(int fd, std::vector<char>& data, bool disconnected, int errorCode);

	// 判断一次 TCP completion 是否表示读侧生命周期结束。
	// EOF、ECONNRESET、发送失败转读侧错误都应该只交给上层一次；
	// 普通 payload completion 不能设置终止标记，否则会错误停止后续 recv。
	bool isTcpTerminalCompletion(const std::vector<char>& data, bool disconnected, int errorCode) const;

	// completion 到达后将 UDP datagram 放入共享队列。
	bool pushUdpReceivedData(int fd, std::vector<char>& data, const sockaddr_in& srcAddr, int errorCode);

	// 判断 TCP 接收 completion 队列是否仍允许继续缓存数据。
	bool canQueueTcpReceivedData(int fd, size_t len) const;

	// 判断 UDP 接收 completion 队列是否仍允许继续缓存数据。
	bool canQueueUdpReceivedData(int fd, size_t len) const;

	// 清空一个 fd 的 TCP/UDP 发送队列，并同步归零 backlog 字节数。
	// 读注销、写注销和 fd 生命周期重置都会走到这类清理路径；集中到基类后，
	// IOCP/io_uring/kqueue 不需要各自维护“clear deque + 清 byte 计数”的重复代码。
	void clearPendingSends(SocketState& state);

	// 从 TCP 发送队列头部取出一个有界 batch。
	// completion 后端都会把多个小包合并成一次系统发送，以减少 completion 数量；
	// 但合并时必须保持字节流顺序，并正确扣减 pendingTcpSendBytes。
	// 如果队首包只取走一部分，剩余内容继续留在队首，下一次发送接着发。
	bool popTcpSendBatch(SocketState& state, size_t maxBytes, std::vector<char>& batch);

	// 把一次部分完成的 TCP send 剩余字节放回队首。
	// WSASend/io_uring send 都可能只完成部分字节；剩余数据必须插回队首，
	// 否则后续排队数据会越过它，破坏 TCP 字节流顺序。
	void pushTcpSendFront(SocketState& state, std::vector<char>& data);

	// 判断 accept/recv 队列是否还有容量。
	// 这些水位主要服务 kqueue readiness adapter：kqueue 需要先把 readiness 数据读到
	// 用户态 handoff 队列，再伪装成 completion 交给上层，因此必须能暂停 drain。
	// io_uring/IOCP 本身就是 completion，只使用队列作为 triggerRead 前后的短暂交接，
	// 不用这些 canArm* 水位去停止内核 read 投递。
	bool canQueueAcceptedSocket(int fd) const;
	bool canArmTcpReceive(int fd) const;
	bool canArmUdpReceive(int fd) const;

	// 低水位恢复判断。
	// 暂停读之后不应刚消费 1 个 item 就立刻恢复，否则会在高水位附近来回 enable/disable；
	// 这里用 1/2 队列水位作为滞回区间，让恢复更平滑。
	bool shouldResumeTcpReceive(int fd) const;
	bool shouldResumeUdpReceive(int fd) const;

	// 清理接收 completion 队列和对应计数。
	void clearReceivedData(int fd);

	// 查询队列 item，避免空错误/断开 completion 绕过 bytes 计数。
	size_t acceptedSocketCount(int fd) const;
	size_t tcpReceivedItemCount(int fd) const;
	size_t udpReceivedItemCount(int fd) const;

	// 关闭 accept 队列中尚未被上层接走的 socket。
	void closeAcceptedSockets(AcceptedSockets& acceptedSockets);

	// 关闭一个平台 socket，供基类清理 accepted socket 队列使用。
	void closeSocket(KBESOCKET socket);

	// 返回当前平台的无效 socket 值。
	KBESOCKET invalidSocket() const;

	// 清理一个不再有注册、pending IO 和排队数据的 fd 状态。
	void cleanupStateIfUnused(int fd);

	SocketStates socketStates_;
	AcceptedSocketMap acceptedSockets_;
	TcpReceivedMap tcpReceived_;
	UdpReceivedMap udpReceived_;
};

}
}

#endif // KBE_COMPLETION_POLLER_H
