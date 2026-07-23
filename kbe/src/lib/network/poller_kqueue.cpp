// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com


#include "poller_kqueue.h"
#include "helper/profile.h"
#ifdef HAS_KQUEUE
#include <sys/event.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#endif

namespace KBEngine {

#ifdef HAS_KQUEUE
namespace
{
ProfileVal g_kqueueIdleProfile("Idle");
const int KQUEUE_ACCEPT_DRAIN_BUDGET = 32;
const int KQUEUE_TCP_DRAIN_BUDGET = 32;
const int KQUEUE_UDP_DRAIN_BUDGET = 64;
const size_t KQUEUE_TCP_DRAIN_BYTES_BUDGET = 256 * 1024;
}

namespace Network
{

//-------------------------------------------------------------------------------------
KqueuePoller::KqueuePoller() :
	kqfd_(kqueue())
{
	if (kqfd_ == -1)
	{
		ERROR_MSG(fmt::format("KqueuePoller::KqueuePoller: kqueue create failed: {}\n",
				kbe_strerror()));
	}
}

//-------------------------------------------------------------------------------------
KqueuePoller::~KqueuePoller()
{
	if (kqfd_ != -1)
	{
		close(kqfd_);
	}
}

//-------------------------------------------------------------------------------------
bool KqueuePoller::queueTcpSend(int fd, const void* data, int len)
{
	// 发送入队后启用 EVFILT_WRITE，真正 send 只在 kqueue 报可写时执行。
	if (!CompletionPoller::queueTcpSend(fd, data, len))
	{
		return false;
	}

	return doRegister(fd, false, true);
}

//-------------------------------------------------------------------------------------
bool KqueuePoller::queueUdpSend(int fd, const void* data, int len, const Address& dstAddr)
{
	// UDP/KCP 发送同样由写唤醒驱动，避免 processPendingEvents 主动全局 flush。
	if (!CompletionPoller::queueUdpSend(fd, data, len, dstAddr))
	{
		return false;
	}

	return doRegister(fd, false, true);
}

//-------------------------------------------------------------------------------------
bool KqueuePoller::takeAcceptedSocket(int fd, KBESOCKET& acceptedSocket)
{
	// listener 消费掉 accepted socket 后，accept 队列可能从高水位回落。
	// kqueue 的背压是通过 EV_DISABLE 停读实现的，所以消费路径必须负责尝试恢复读事件。
	bool ret = CompletionPoller::takeAcceptedSocket(fd, acceptedSocket);
	auto iter = socketStates_.find(fd);
	if (iter != socketStates_.end())
	{
		updateReadBackpressure(fd, *iter->second);
	}

	return ret;
}

//-------------------------------------------------------------------------------------
bool KqueuePoller::takeTcpReceivedData(int fd, std::vector<char>& data, bool& disconnected, int& errorCode)
{
	// TCPPacketReceiver 每次 take 都会释放一段 completion 队列容量。
	// 如果之前因为队列满暂停了 EVFILT_READ，这里按低水位决定是否重新打开。
	bool ret = CompletionPoller::takeTcpReceivedData(fd, data, disconnected, errorCode);
	auto iter = socketStates_.find(fd);
	if (iter != socketStates_.end())
	{
		updateReadBackpressure(fd, *iter->second);
	}

	return ret;
}

//-------------------------------------------------------------------------------------
bool KqueuePoller::takeUdpReceivedData(int fd, std::vector<char>& data, Address& srcAddr, int& errorCode)
{
	// UDP/KCP 小包 burst 更容易触发 item 上限；消费后也要及时恢复 read filter。
	bool ret = CompletionPoller::takeUdpReceivedData(fd, data, srcAddr, errorCode);
	auto iter = socketStates_.find(fd);
	if (iter != socketStates_.end())
	{
		updateReadBackpressure(fd, *iter->second);
	}

	return ret;
}

//-------------------------------------------------------------------------------------
bool KqueuePoller::doRegister(int fd, bool isRead, bool isRegister)
{
	struct kevent change;
	EV_SET(&change,
		static_cast<uintptr_t>(fd),
		isRead ? EVFILT_READ : EVFILT_WRITE,
		isRegister ? (EV_ADD | EV_ENABLE) : EV_DELETE,
		0,
		0,
		NULL);

	if (kevent(kqfd_, &change, 1, NULL, 0, NULL) < 0)
	{
		// EV_DELETE on an unregistered fd is not fatal for caller semantics.
		if (!isRegister && errno == ENOENT)
		{
			return true;
		}

		const char* MESSAGE = "KqueuePoller::doRegister: Failed to {} {} file "
				"descriptor {} ({})\n";
		if (errno == EBADF)
		{
			WARNING_MSG(fmt::format(MESSAGE,
					(isRegister ? "add" : "remove"),
					(isRead ? "read" : "write"),
					fd,
					kbe_strerror()));
		}
		else
		{
			ERROR_MSG(fmt::format(MESSAGE,
					(isRegister ? "add" : "remove"),
					(isRead ? "read" : "write"),
					fd,
					kbe_strerror()));
		}

		return false;
	}

	return true;
}

//-------------------------------------------------------------------------------------
bool KqueuePoller::setReadEnabled(int fd, SocketState& state, bool enabled)
{
	// kqueue 是 readiness，不是真正的内核 completion。
	// 当用户态 completion 队列已满时，如果继续保持 EVFILT_READ enabled，
	// kevent 会持续返回同一个可读 fd，主线程会反复空转。
	// 因此 adapter 用 EV_DISABLE/EV_ENABLE 把“用户态队列水位”反馈给 readiness 层。
	if (!state.registeredRead)
	{
		state.readBackpressured = false;
		return true;
	}

	struct kevent change;
	EV_SET(&change,
		static_cast<uintptr_t>(fd),
		EVFILT_READ,
		enabled ? EV_ENABLE : EV_DISABLE,
		0,
		0,
		NULL);

	if (kevent(kqfd_, &change, 1, NULL, 0, NULL) < 0)
	{
		// fd 已关闭或事件已被删除时，通常说明上层注销和 stale event 交错发生。
		// 这种情况不再升级为致命错误，但仍保留 warning 便于定位生命周期问题。
		if (errno == EBADF || errno == ENOENT)
		{
			WARNING_MSG(fmt::format("KqueuePoller::setReadEnabled: failed to {} read fd {}: {}\n",
				enabled ? "enable" : "disable", fd, kbe_strerror()));
		}
		else
		{
			ERROR_MSG(fmt::format("KqueuePoller::setReadEnabled: failed to {} read fd {}: {}\n",
				enabled ? "enable" : "disable", fd, kbe_strerror()));
		}
		return false;
	}

	state.readBackpressured = !enabled;
	state.readArmed = enabled;
	return true;
}

//-------------------------------------------------------------------------------------
void KqueuePoller::updateReadBackpressure(int fd, SocketState& state)
{
	// 根据不同 socket 类型选择对应队列水位：
	// listener 看 accepted socket 数，TCP/UDP 看 recv bytes + item。
	// pause 使用高水位 canQueue/canArm 判断，resume 使用低水位 shouldResume 判断，
	// 这样不会在“刚好差一个 item”的边缘反复切换 EVFILT_READ。
	if (!state.registeredRead)
	{
		state.readBackpressured = false;
		return;
	}

	if (!refreshSocketKind(fd, state))
	{
		return;
	}

	if (state.kind == SOCKET_KIND_TCP && state.tcpReadTerminated)
	{
		// TCP EOF/错误已经进入 handoff 队列后，读侧生命周期就结束了。
		// 即使上层 take 掉这个终止 completion，也不能按低水位重新打开 EVFILT_READ；
		// 否则 kqueue 会在已断开的 fd 上继续报告 EOF，形成 0 字节 completion 风暴。
		if (!state.readBackpressured)
		{
			setReadEnabled(fd, state, false);
		}
		return;
	}

	bool shouldPause = false;
	bool shouldResume = true;
	if (state.kind == SOCKET_KIND_TCP)
	{
		shouldPause = !canArmTcpReceive(fd);
		shouldResume = shouldResumeTcpReceive(fd);
	}
	else if (state.kind == SOCKET_KIND_UDP)
	{
		shouldPause = !canArmUdpReceive(fd);
		shouldResume = shouldResumeUdpReceive(fd);
	}
	else if (state.kind == SOCKET_KIND_LISTENER)
	{
		shouldPause = !canQueueAcceptedSocket(fd);
		shouldResume = canQueueAcceptedSocket(fd);
	}

	if (!state.readBackpressured && shouldPause)
	{
		setReadEnabled(fd, state, false);
	}
	else if (state.readBackpressured && shouldResume)
	{
		setReadEnabled(fd, state, true);
	}
}

//-------------------------------------------------------------------------------------
bool KqueuePoller::refreshSocketKind(int fd, SocketState& state)
{
	// kqueue event 只给 fd/filter，不告诉我们它是 listener、TCP stream 还是 UDP。
	// fd 复用或 listen 状态刚建立时，kind 可能还没识别；这里集中刷新，避免各分支
	// 在 UNKNOWN 时默认走 TCP，重新制造 listener fd 的 TCP completion 积压。
	if (state.kind != SOCKET_KIND_UNKNOWN)
	{
		return true;
	}

	(void)fd;
	return tryDetermineSocketKind(state.socket, state.kind);
}

//-------------------------------------------------------------------------------------
bool KqueuePoller::isReadEventCurrent(int fd, SocketState& state)
{
	// kevent 返回的是一次快照。处理这一批事件的过程中，上层 handler 可能已经注销 fd、
	// 销毁 channel，甚至 fd 被系统复用。任何读侧 drain/错误转换前都要确认：
	// 1) 读侧仍注册；2) 没被背压暂停；3) handler 仍存在；4) socket kind 可识别。
	return state.registeredRead && !state.readBackpressured &&
		this->findForRead(fd) != NULL && refreshSocketKind(fd, state);
}

//-------------------------------------------------------------------------------------
bool KqueuePoller::doRegisterForRead(int fd)
{
	// 读注册挂 EVFILT_READ，唤醒后 adapter 只执行一次 accept/recv completion。
	SocketState& state = socketStateForFd(fd);
	state.registeredRead = true;
	state.readBackpressured = false;
	state.readArmed = false;
	++state.generation;
	clearReceivedData(fd);

	if (!tryDetermineSocketKind(state.socket, state.kind))
	{
		return false;
	}

	return doRegister(fd, true, true);
}

//-------------------------------------------------------------------------------------
bool KqueuePoller::doRegisterForWrite(int fd)
{
	// 写注册挂 EVFILT_WRITE，等待内核确认 socket 可写后再发送队列数据。
	return doRegister(fd, false, true);
}

//-------------------------------------------------------------------------------------
bool KqueuePoller::doDeregisterForRead(int fd)
{
	// 删除读事件并丢弃还没消费的接收 completion。
	auto iter = socketStates_.find(fd);
	if (iter != socketStates_.end())
	{
		iter->second->registeredRead = false;
		iter->second->readBackpressured = false;
		iter->second->readArmed = false;
		++iter->second->generation;
	}

	clearReceivedData(fd);
	bool ret = doRegister(fd, true, false);
	cleanupStateIfUnused(fd);
	return ret;
}

//-------------------------------------------------------------------------------------
bool KqueuePoller::doDeregisterForWrite(int fd)
{
	// 写注销只清理发送队列，不能清掉接收 completion。
	auto iter = socketStates_.find(fd);
	if (iter == socketStates_.end())
	{
		return true;
	}

	SocketState& state = *iter->second;
	clearPendingSends(state);
	state.writeArmed = false;
	bool ret = doRegister(fd, false, false);
	cleanupStateIfUnused(fd);
	return ret;
}

//-------------------------------------------------------------------------------------
bool KqueuePoller::ensureReadArmed(int fd, SocketState& state)
{
	// kqueue adapter 没有真正 outstanding read，armed 表示等待下一次 kqueue 唤醒。
	if (!state.registeredRead || state.readBackpressured)
	{
		return true;
	}

	if (!refreshSocketKind(fd, state))
	{
		return false;
	}

	state.readArmed = true;
	(void)fd;
	return true;
}

//-------------------------------------------------------------------------------------
int KqueuePoller::drainAccept(int fd, SocketState& state)
{
	// 一次唤醒只接受有界数量的连接，避免 readiness burst 在 poller 内堆积大量 completion。
	// triggerRead 会同步进入 ListenerTcpReceiver，并可能注销 listener 或改变 socketStates_。
	// 因此函数尾部恢复背压前要用 originalState 检查当前 state 是否仍是同一个对象。
	SocketState* originalState = &state;
	int count = 0;
	for (; count < KQUEUE_ACCEPT_DRAIN_BUDGET; ++count)
	{
		if (!canQueueAcceptedSocket(fd))
		{
			// 队列满时不要继续 accept。继续 accept 只会把内核 backlog 转移到用户态，
			// 放大 fd/内存压力；暂停 read filter 后，让内核 listen backlog 自然承担背压。
			updateReadBackpressure(fd, state);
			break;
		}

		sockaddr_in sin;
		socklen_t sinLen = sizeof(sin);
		int accepted = static_cast<int>(::accept(state.socket, reinterpret_cast<sockaddr*>(&sin), &sinLen));
		if (accepted < 0)
		{
			if (errno != EAGAIN && errno != EWOULDBLOCK)
			{
				WARNING_MSG(fmt::format("KqueuePoller::drainAccept: accept failed on fd {}: {}\n",
					fd, kbe_strerror()));
			}

			break;
		}

		if (!pushAcceptedSocket(fd, accepted))
		{
			updateReadBackpressure(fd, state);
			break;
		}
	}

	if (count > 0)
	{
		this->triggerRead(fd);
	}

	auto currentIter = socketStates_.find(fd);
	if (currentIter != socketStates_.end() && currentIter->second.get() == originalState)
	{
		updateReadBackpressure(fd, *currentIter->second);
	}
	return count;
}

//-------------------------------------------------------------------------------------
int KqueuePoller::drainTcpRead(int fd, SocketState& state)
{
	// TCP 一次唤醒读取有界小批量数据，随后只触发一次上层消费。
	// 这里是 readiness -> completion 的转换点：recv 出来的数据必须先进入
	// tcpReceived_，上层 TCPPacketReceiver 只从 completion 队列取，不能再直接 recv。
	SocketState* originalState = &state;
	int count = 0;
	size_t bytesRead = 0;
	bool shouldTrigger = false;

	for (; count < KQUEUE_TCP_DRAIN_BUDGET && bytesRead < KQUEUE_TCP_DRAIN_BYTES_BUDGET; ++count)
	{
		if (!canQueueTcpReceivedData(fd, PACKET_MAX_SIZE_TCP))
		{
			// 用 PACKET_MAX_SIZE_TCP 预估下一次 recv 的最大缓存需求。
			// 如果连一个最大 TCP packet 都放不下，就暂停 read，等上层消费后再恢复。
			updateReadBackpressure(fd, state);
			break;
		}

		std::vector<char> data(PACKET_MAX_SIZE_TCP);
		int ret = static_cast<int>(::recv(state.socket, data.data(), data.size(), 0));
		if (ret > 0)
		{
			data.resize(static_cast<size_t>(ret));
			if (!pushTcpReceivedData(fd, data, false, 0))
			{
				break;
			}

			bytesRead += static_cast<size_t>(ret);
			shouldTrigger = true;
			continue;
		}

		if (ret == 0)
		{
			setReadEnabled(fd, state, false);
			data.clear();
			if (pushTcpReceivedData(fd, data, true, 0))
			{
				shouldTrigger = true;
				++count;
			}
			break;
		}

		if (errno != EAGAIN && errno != EWOULDBLOCK)
		{
			int errorCode = errno;
			if (errorCode == ENOTCONN || errorCode == EINVAL || errorCode == ENOTSOCK)
			{
				// macOS 上如果 listener 类型识别失败，recv(listener) 会走到这里。
				// 改按 accept 尝试一次，避免给 listener fd 堆无人消费的 TCP 错误 completion。
				int accepted = static_cast<int>(::accept(state.socket, NULL, NULL));
				if (accepted >= 0)
				{
					state.kind = SOCKET_KIND_LISTENER;
					if (pushAcceptedSocket(fd, accepted))
					{
						shouldTrigger = true;
						++count;
					}
					break;
				}

				if (errno == EAGAIN || errno == EWOULDBLOCK)
				{
					state.kind = SOCKET_KIND_LISTENER;
					break;
				}
			}

			data.clear();
			setReadEnabled(fd, state, false);
			if (pushTcpReceivedData(fd, data, false, errorCode))
			{
				shouldTrigger = true;
				++count;
			}
			break;
		}

		break;
	}

	if (shouldTrigger)
	{
		this->triggerRead(fd);
	}

	auto currentIter = socketStates_.find(fd);
	if (currentIter != socketStates_.end() && currentIter->second.get() == originalState)
	{
		updateReadBackpressure(fd, *currentIter->second);
	}
	return count;
}

//-------------------------------------------------------------------------------------
int KqueuePoller::drainUdpRead(int fd, SocketState& state)
{
	// UDP 一次唤醒读取有界数量 datagram，防止内部组件启动 burst 放大内存。
	// UDP 每个 datagram 都是独立 completion，item 上限通常比 bytes 上限更敏感。
	SocketState* originalState = &state;
	int count = 0;
	for (; count < KQUEUE_UDP_DRAIN_BUDGET; ++count)
	{
		if (!canQueueUdpReceivedData(fd, PACKET_MAX_SIZE_UDP))
		{
			updateReadBackpressure(fd, state);
			break;
		}

		std::vector<char> data(PACKET_MAX_SIZE_UDP);
		sockaddr_in srcAddr;
		memset(&srcAddr, 0, sizeof(srcAddr));
		socklen_t srcAddrLen = sizeof(srcAddr);
		int ret = static_cast<int>(::recvfrom(state.socket, data.data(), data.size(), 0,
			reinterpret_cast<sockaddr*>(&srcAddr), &srcAddrLen));
		if (ret > 0)
		{
			data.resize(static_cast<size_t>(ret));
			if (!pushUdpReceivedData(fd, data, srcAddr, 0))
			{
				break;
			}

			continue;
		}

		if (ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
		{
			WARNING_MSG(fmt::format("KqueuePoller::drainUdpRead: recvfrom failed on fd {}: {}\n",
				fd, kbe_strerror()));
		}

		break;
	}

	if (count > 0)
	{
		this->triggerRead(fd);
	}

	auto currentIter = socketStates_.find(fd);
	if (currentIter != socketStates_.end() && currentIter->second.get() == originalState)
	{
		updateReadBackpressure(fd, *currentIter->second);
	}
	return count;
}

//-------------------------------------------------------------------------------------
int KqueuePoller::flushPendingSends(int fd, SocketState& state)
{
	// adapter 发送队列完成后再触发写通知，保持 Channel::onSendCompleted 时序。
	// kqueue 写侧仍然使用 EVFILT_WRITE readiness 驱动真实 send/sendto，
	// 这样不会在主循环每一 tick 主动扫描所有 fd 的发送队列。
	int count = 0;
	while (!state.pendingTcpSends.empty())
	{
		std::vector<char>& front = state.pendingTcpSends.front();
		int ret = static_cast<int>(::send(state.socket, front.data(), front.size(), 0));
		if (ret > 0)
		{
			const size_t sent = static_cast<size_t>(ret);
			state.pendingTcpSendBytes -= sent;
			if (sent == front.size())
			{
				state.pendingTcpSends.pop_front();
			}
			else
			{
				front.erase(front.begin(), front.begin() + sent);
				break;
			}
			++count;
			continue;
		}

		if (errno != EAGAIN && errno != EWOULDBLOCK)
		{
			// TCP 发送失败要回到读侧错误路径处理，让 Channel 关闭逻辑和
			// 原来的 TCPPacketReceiver::checkSocketErrors 保持一致。
			std::vector<char> empty;
			if (!pushTcpReceivedData(fd, empty, false, errno))
			{
				break;
			}
			this->triggerRead(fd);
			++count;
			auto currentIter = socketStates_.find(fd);
			if (currentIter == socketStates_.end() || currentIter->second.get() != &state)
			{
				return count;
			}
		}
		break;
	}

	while (!state.pendingUdpSends.empty())
	{
		PendingUdpSend& pending = state.pendingUdpSends.front();
		int ret = static_cast<int>(::sendto(state.socket, pending.data.data(), pending.data.size(), 0,
			reinterpret_cast<sockaddr*>(&pending.dstAddr), sizeof(pending.dstAddr)));
		if (ret < 0)
		{
			if (errno != EAGAIN && errno != EWOULDBLOCK)
			{
				WARNING_MSG(fmt::format("KqueuePoller::flushPendingSends: udp sendto failed on fd {}: {}\n",
					fd, kbe_strerror()));
				state.pendingUdpSendBytes -= pending.data.size();
				state.pendingUdpSends.pop_front();
				++count;
			}
			break;
		}

		state.pendingUdpSendBytes -= pending.data.size();
		state.pendingUdpSends.pop_front();
		++count;
	}

	if (state.pendingTcpSends.empty() && state.pendingUdpSends.empty())
	{
		state.writeArmed = false;
		if (this->findForWrite(fd) != NULL)
		{
			this->triggerWrite(fd);
		}
	}
	else
	{
		state.writeArmed = true;
	}

	return count;
}

//-------------------------------------------------------------------------------------
int KqueuePoller::processPendingEvents(double maxWait)
{
	const int MAX_EVENTS = 10;
	struct kevent events[MAX_EVENTS];
	struct timespec timeout;
	timeout.tv_sec = static_cast<time_t>(maxWait);
	timeout.tv_nsec = static_cast<long>((maxWait - static_cast<double>(timeout.tv_sec)) * 1000000000.0);

#if ENABLE_WATCHERS
	g_kqueueIdleProfile.start();
#else
	uint64 startTime = timestamp();
#endif

	KBEConcurrency::onStartMainThreadIdling();
	int nfds = kevent(kqfd_, NULL, 0, events, MAX_EVENTS, &timeout);
	KBEConcurrency::onEndMainThreadIdling();


#if ENABLE_WATCHERS
	g_kqueueIdleProfile.stop();
	spareTime_ += g_kqueueIdleProfile.lastTime_;
#else
	spareTime_ += timestamp() - startTime;
#endif

	if (nfds < 0)
	{
		WARNING_MSG(fmt::format("KqueuePoller::processPendingEvents: "
			"error in kevent(): {}\n", kbe_strerror()));
		return nfds;
	}

	int readyCount = 0;
	for (int i = 0; i < nfds; ++i)
	{
		int fd = static_cast<int>(events[i].ident);
		if ((events[i].flags & EV_ERROR) || (events[i].flags & EV_EOF))
		{
			// EV_ERROR/EV_EOF 不能无条件转成 TCP completion。
			// listener/UDP 没有消费者会取 tcpReceived_；stale fd 也可能已经没有 handler。
			// 只有确认当前 fd 仍是有效 TCP read 事件时，才把 EOF/error 放进 TCP 队列。
			auto iter = socketStates_.find(fd);
			if (iter != socketStates_.end())
			{
				SocketState& state = *iter->second;
				if (events[i].filter == EVFILT_READ && isReadEventCurrent(fd, state) &&
					state.kind == SOCKET_KIND_TCP)
				{
					// kqueue 的 EV_EOF/EV_ERROR 也要转换成 TCP completion，
					// 否则 completion receiver 只收到一次空 triggerRead，拿不到断开原因。
					std::vector<char> empty;
					const int errorCode = (events[i].flags & EV_ERROR) ? static_cast<int>(events[i].data) : 0;
					setReadEnabled(fd, state, false);
					if (pushTcpReceivedData(fd, empty, true, errorCode))
					{
						this->triggerRead(fd);
					}
				}
				else
				{
					this->triggerError(fd);
				}
			}
			else
			{
				this->triggerError(fd);
			}
			++readyCount;
		}
		else if (events[i].filter == EVFILT_READ)
		{
			auto iter = socketStates_.find(fd);
			if (iter == socketStates_.end())
			{
				continue;
			}

			SocketState& state = *iter->second;
			if (!isReadEventCurrent(fd, state))
			{
				continue;
			}

			if (state.kind == SOCKET_KIND_LISTENER)
			{
				readyCount += drainAccept(fd, state);
			}
			else if (state.kind == SOCKET_KIND_UDP)
			{
				readyCount += drainUdpRead(fd, state);
			}
			else
			{
				readyCount += drainTcpRead(fd, state);
			}

			auto currentIter = socketStates_.find(fd);
			if (currentIter != socketStates_.end())
			{
				ensureReadArmed(fd, *currentIter->second);
			}
		}
		else if (events[i].filter == EVFILT_WRITE)
		{
			auto iter = socketStates_.find(fd);
			if (iter != socketStates_.end())
			{
				readyCount += flushPendingSends(fd, *iter->second);
			}
		}
	}

	return readyCount;
}

}

#endif // HAS_KQUEUE

}
