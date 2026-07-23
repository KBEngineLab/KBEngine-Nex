// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

#ifndef KBE_IOCP_POLLER_H
#define KBE_IOCP_POLLER_H

#include "poller_completion.h"

#if KBE_PLATFORM == PLATFORM_WIN32

#include "network/address.h"

namespace KBEngine {
namespace Network
{

class IocpPoller : public CompletionPoller
{
public:
	// 创建 IOCP 完成端口并初始化共享 completion 状态。
	IocpPoller();

	// 取消 outstanding IO 并释放 IOCP 相关资源。
	~IocpPoller() override;

	// 等待并处理一批 IOCP completion。
	int processPendingEvents(double maxWait) override;

	// IOCP 入队后立即尝试投递 WSASend，保留发送失败的同步反馈语义。
	bool queueTcpSend(int fd, const void* data, int len) override;

	// IOCP 入队后立即尝试投递 WSASendTo，避免 UDP/KCP 发送只滞留在队列里。
	bool queueUdpSend(int fd, const void* data, int len, const Address& dstAddr) override;

protected:
	// 将 fd 绑定到 IOCP 并投递读侧 completion。
	bool doRegisterForRead(int fd) override;

	// 写侧注册只保存 handler，真实发送由发送队列驱动。
	bool doRegisterForWrite(int fd) override;

	// 注销读侧并取消 outstanding read/accept 操作。
	bool doDeregisterForRead(int fd) override;

	// 注销写侧并取消 outstanding send 操作。
	bool doDeregisterForWrite(int fd) override;

private:
	enum Operation
	{
		// IOCP 本身只返回 OVERLAPPED 完成事件，这里用 operation 标记
		// 本次完成到底属于 accept/recv/send 的哪条路径，避免再回到
		// select/readiness 模型里二次判断 socket 是否可读可写。
		OP_ACCEPT = 0,
		OP_TCP_RECV,
		OP_UDP_RECV,
		OP_TCP_SEND,
		OP_UDP_SEND
	};

	struct IocpContext
	{
		IocpContext(int fdArg, KBESOCKET socketArg, SocketKind kindArg, Operation operationArg, uint64 generationArg);

		// 每一次异步调用都拥有独立的 OVERLAPPED 和数据缓冲。
		// 完成回调回来前，buffer 必须一直有效，所以不能使用栈内存。
		OVERLAPPED overlapped;
		int fd;
		KBESOCKET socket;
		SocketKind kind;
		Operation operation;
		// generation 用来识别“旧 socket 的迟到完成事件”。
		// fd 在 Windows 上可能被复用，如果 channel 已经销毁又创建了新连接，
		// 旧 completion 不能误投递到新连接上。
		uint64 generation;
		WSABUF buffer;
		DWORD flags;
		std::vector<char> data;
		KBESOCKET acceptSocket;
		char acceptBuffer[(sizeof(sockaddr_in) + 16) * 2];
		sockaddr_in udpAddr;
		int udpAddrLen;
	};

	// 确保 fd 已关联到 IOCP completion port。
	bool ensureAssociated(SocketState& state, int fd);
	// 根据 socket 类型投递 accept/recv/recvfrom。
	bool ensureReadArmed(int fd, SocketState& state);
	// 投递一次 TCP WSARecv。
	bool armTcpRead(int fd, SocketState& state);
	// 投递一次 UDP WSARecvFrom。
	bool armUdpRead(int fd, SocketState& state);
	// 投递一次 TCP WSASend。
	bool armTcpSend(int fd, SocketState& state);
	// 投递一次 UDP WSASendTo。
	bool armUdpSend(int fd, SocketState& state);
	// 投递一次 AcceptEx。
	bool armAccept(int fd, SocketState& state);
	// 加载 listener socket 对应的 AcceptEx 函数指针。
	bool loadAcceptEx(SocketState& state);
	// 清理一次完成上下文持有的 pending 指针。
	void cleanupContext(IocpContext& context);
	// 处理一个 IOCP 返回的 OVERLAPPED 完成结果。
	void handleCompletion(ULONG_PTR completionKey, LPOVERLAPPED overlapped, DWORD bytesTransferred, bool success, DWORD errorCode);

	HANDLE completionPort_;
	uint64 lastCompletionBudgetWarningTime_;
};

}
}

#endif // KBE_PLATFORM == PLATFORM_WIN32

#endif // KBE_IOCP_POLLER_H
