// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

#ifndef KBE_KQUEUE_POLLER_H
#define KBE_KQUEUE_POLLER_H

#include "poller_completion.h"

#if KBE_PLATFORM == PLATFORM_APPLE
#define HAS_KQUEUE
#endif

namespace KBEngine {
namespace Network
{

#ifdef HAS_KQUEUE
class KqueuePoller : public CompletionPoller
{
public:
	// 创建 kqueue fd 并初始化共享 completion 状态。
	KqueuePoller();

	// 关闭 kqueue fd 并释放 adapter 状态。
	virtual ~KqueuePoller();

	// 返回底层 kqueue fd，便于诊断和平台集成。
	int getFileDescriptor() const override { return kqfd_; }

	// 将 TCP 发送数据入队，并启用 kqueue 写唤醒来驱动一次 send completion。
	bool queueTcpSend(int fd, const void* data, int len) override;

	// 将 UDP 发送数据入队，并启用 kqueue 写唤醒来驱动一次 sendto completion。
	bool queueUdpSend(int fd, const void* data, int len, const Address& dstAddr) override;

	// 消费 completion 后按低水位恢复被背压暂停的读事件。
	bool takeAcceptedSocket(int fd, KBESOCKET& acceptedSocket) override;
	bool takeTcpReceivedData(int fd, std::vector<char>& data, bool& disconnected, int& errorCode) override;
	bool takeUdpReceivedData(int fd, std::vector<char>& data, Address& srcAddr, int& errorCode) override;

protected:
	// 注册读侧 completion adapter，kqueue 只负责唤醒，数据由 adapter 读入队列。
	bool doRegisterForRead(int fd) override;

	// 写侧在 macOS 下仍使用 EVFILT_WRITE readiness，避免 adapter 缓存放大内存。
	bool doRegisterForWrite(int fd) override;

	// 注销读侧时删除 kqueue 事件并清理 completion 队列。
	bool doDeregisterForRead(int fd) override;

	// 注销写侧时删除 kqueue 写事件并清空发送队列。
	bool doDeregisterForWrite(int fd) override;

	// 处理 kqueue 唤醒并转换成 completion 队列事件。
	int processPendingEvents(double maxWait) override;

	// 对 kqueue 注册或删除一个底层过滤器。
	bool doRegister(int fd, bool isRead, bool isRegister);

	// 临时启停 read filter，用 readiness 层背压 completion 队列。
	bool setReadEnabled(int fd, SocketState& state, bool enabled);
	void updateReadBackpressure(int fd, SocketState& state);

	// 检查 kevent 是否仍属于当前读生命周期，并刷新 socket 类型。
	bool refreshSocketKind(int fd, SocketState& state);
	bool isReadEventCurrent(int fd, SocketState& state);

	// 根据 fd 类型挂起或执行一次读侧 adapter 操作。
	bool ensureReadArmed(int fd, SocketState& state);

	// 从 listener 接受一个连接，并把结果转成 accept completion。
	int drainAccept(int fd, SocketState& state);

	// 从 TCP socket 读取一次，并把结果转成 TCP completion。
	int drainTcpRead(int fd, SocketState& state);

	// 从 UDP socket 读取一个 datagram，并把结果转成 UDP completion。
	int drainUdpRead(int fd, SocketState& state);

	// 尝试发送 TCP/UDP 队列，并在完成后触发写 completion。
	int flushPendingSends(int fd, SocketState& state);

private:
	int kqfd_;
};
#endif // HAS_KQUEUE

}
}
#endif // KBE_KQUEUE_POLLER_H
