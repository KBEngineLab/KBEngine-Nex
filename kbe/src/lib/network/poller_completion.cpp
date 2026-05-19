// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

#include "poller_completion.h"

#if KBE_PLATFORM_UNIX_FAMILY
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <algorithm>
#include <utility>

namespace KBEngine {
namespace Network
{
namespace
{
const size_t COMPLETION_TCP_SEND_BACKLOG_BYTES = 1024 * 1024;
const size_t COMPLETION_UDP_SEND_BACKLOG_BYTES = 4 * 1024 * 1024;
const size_t COMPLETION_TCP_RECV_BACKLOG_BYTES = 1024 * 1024;
const size_t COMPLETION_UDP_RECV_BACKLOG_BYTES = 4 * 1024 * 1024;
// accept 队列也必须有界。登录风暴下 listener 可能比 NetworkInterface 注册 Channel 更快，
// 如果无限缓存 accepted fd，就算数据队列受控，进程 fd/内存仍然会被连接对象拖爆。
const size_t COMPLETION_ACCEPT_BACKLOG_ITEMS = 1024;
const size_t COMPLETION_TCP_RECV_BACKLOG_ITEMS = 1024;
const size_t COMPLETION_UDP_RECV_BACKLOG_ITEMS = 4096;
// 恢复读事件的低水位比例。高水位停读、半水位恢复，避免在临界点频繁抖动。
const size_t COMPLETION_RECV_RESUME_RATIO = 2;
}

//-------------------------------------------------------------------------------------
CompletionPoller::SocketState::SocketState(KBESOCKET socketArg) :
	socket(socketArg),
	kind(SOCKET_KIND_UNKNOWN),
	associated(false),
	registeredRead(false),
	readBackpressured(false),
	readArmed(false),
	writeArmed(false),
	tcpReadTerminated(false),
	generation(0),
	pPendingReadContext(NULL),
	pPendingWriteContext(NULL),
	pendingTcpSends(),
	pendingTcpSendBytes(0),
	pendingUdpSends(),
	pendingUdpSendBytes(0),
	pendingTcpReceiveBytes(0),
	pendingUdpReceiveBytes(0)
#if KBE_PLATFORM == PLATFORM_WIN32
	,
	acceptExFn(NULL)
#endif
{
}

//-------------------------------------------------------------------------------------
CompletionPoller::CompletionPoller() :
	EventPoller(),
	socketStates_(),
	acceptedSockets_(),
	tcpReceived_(),
	udpReceived_()
{
}

//-------------------------------------------------------------------------------------
CompletionPoller::~CompletionPoller()
{
	for (auto& item : acceptedSockets_)
	{
		closeAcceptedSockets(item.second);
	}
}

//-------------------------------------------------------------------------------------
bool CompletionPoller::supportsCompletion() const
{
	// completion poller 对上层声明自己已经接管 accept/recv/send。
	return true;
}

//-------------------------------------------------------------------------------------
bool CompletionPoller::takeAcceptedSocket(int fd, KBESOCKET& acceptedSocket)
{
	// accept completion 由 poller 放入队列，listener 只负责消费。
	auto iter = acceptedSockets_.find(fd);
	if (iter == acceptedSockets_.end() || iter->second.empty())
	{
		return false;
	}

	acceptedSocket = iter->second.front();
	iter->second.pop_front();

	if (iter->second.empty())
	{
		acceptedSockets_.erase(iter);
	}

	// take 后可能已经没有注册、没有 outstanding IO、没有队列数据。
	// 这里主动 cleanup，避免只消费不注销的边界路径让 SocketState 长时间残留。
	cleanupStateIfUnused(fd);
	return true;
}

//-------------------------------------------------------------------------------------
bool CompletionPoller::takeTcpReceivedData(int fd, std::vector<char>& data, bool& disconnected, int& errorCode)
{
	// TCP completion 队列保持到达顺序，PacketReceiver 按旧解析逻辑消费。
	auto iter = tcpReceived_.find(fd);
	if (iter == tcpReceived_.end() || iter->second.empty())
	{
		return false;
	}

	TcpCompletionData& item = iter->second.front();
	auto stateIter = socketStates_.find(fd);
	if (stateIter != socketStates_.end())
	{
		// pendingTcpReceiveBytes 只统计真实 payload 字节。
		// 空的断开/错误 completion 不会改变 bytes，但仍会被 tcpReceivedItemCount 追踪。
		stateIter->second->pendingTcpReceiveBytes -= item.data.size();
	}

	data.swap(item.data);
	disconnected = item.disconnected;
	errorCode = item.errorCode;
	iter->second.pop_front();

	if (iter->second.empty())
	{
		tcpReceived_.erase(iter);
	}

	cleanupStateIfUnused(fd);
	return true;
}

//-------------------------------------------------------------------------------------
bool CompletionPoller::takeUdpReceivedData(int fd, std::vector<char>& data, Address& srcAddr, int& errorCode)
{
	// UDP completion 队列按 datagram 粒度保存来源地址和数据。
	auto iter = udpReceived_.find(fd);
	if (iter == udpReceived_.end() || iter->second.empty())
	{
		return false;
	}

	UdpCompletionData& item = iter->second.front();
	auto stateIter = socketStates_.find(fd);
	if (stateIter != socketStates_.end())
	{
		// UDP 按 datagram 入队，bytes 用于限制总体缓存，item 数用于限制小包风暴。
		stateIter->second->pendingUdpReceiveBytes -= item.data.size();
	}

	data.swap(item.data);
	srcAddr = item.srcAddr;
	errorCode = item.errorCode;
	iter->second.pop_front();

	if (iter->second.empty())
	{
		udpReceived_.erase(iter);
	}

	cleanupStateIfUnused(fd);
	return true;
}

//-------------------------------------------------------------------------------------
bool CompletionPoller::queueTcpSend(int fd, const void* data, int len)
{
	// 上层交来的 TCP 数据只入队，真正发送由具体 poller arm/write 完成。
	if (len <= 0)
	{
		return true;
	}

	SocketState& state = socketStateForFd(fd);
	if (state.kind == SOCKET_KIND_UNKNOWN)
	{
		state.kind = SOCKET_KIND_TCP;
	}

	if (state.pendingTcpSendBytes + static_cast<size_t>(len) > COMPLETION_TCP_SEND_BACKLOG_BYTES)
	{
#if KBE_PLATFORM == PLATFORM_WIN32
		WSASetLastError(WSAEWOULDBLOCK);
#else
		errno = EAGAIN;
#endif
		return false;
	}

	std::vector<char> sendData(static_cast<const char*>(data), static_cast<const char*>(data) + len);
	state.pendingTcpSendBytes += sendData.size();
	state.pendingTcpSends.push_back(std::move(sendData));
	return true;
}

//-------------------------------------------------------------------------------------
bool CompletionPoller::queueUdpSend(int fd, const void* data, int len, const Address& dstAddr)
{
	// UDP 数据同样入队，具体 poller 保证按队列顺序投递 sendto。
	if (len <= 0)
	{
		return true;
	}

	SocketState& state = socketStateForFd(fd);
	state.kind = SOCKET_KIND_UDP;

	PendingUdpSend pending;
	pending.data.assign(static_cast<const char*>(data), static_cast<const char*>(data) + len);
	if (state.pendingUdpSendBytes + pending.data.size() > COMPLETION_UDP_SEND_BACKLOG_BYTES)
	{
#if KBE_PLATFORM == PLATFORM_WIN32
		WSASetLastError(WSAEWOULDBLOCK);
#else
		errno = EAGAIN;
#endif
		return false;
	}

	memset(&pending.dstAddr, 0, sizeof(pending.dstAddr));
	pending.dstAddr.sin_family = AF_INET;
	pending.dstAddr.sin_addr.s_addr = dstAddr.ip;
	pending.dstAddr.sin_port = dstAddr.port;
	state.pendingUdpSendBytes += pending.data.size();
	state.pendingUdpSends.push_back(std::move(pending));
	return true;
}

//-------------------------------------------------------------------------------------
bool CompletionPoller::hasPendingSend(int fd) const
{
	// writeArmed 表示内核或 adapter 已经持有一次发送请求。
	auto iter = socketStates_.find(fd);
	if (iter == socketStates_.end())
	{
		return false;
	}

	const SocketState& state = *iter->second;
	return state.writeArmed || state.pPendingWriteContext != NULL || !state.pendingTcpSends.empty() ||
		state.pendingTcpSendBytes > 0 || !state.pendingUdpSends.empty() || state.pendingUdpSendBytes > 0;
}

//-------------------------------------------------------------------------------------
CompletionPoller::SocketState& CompletionPoller::socketStateForFd(int fd)
{
	// fd 可能先由发送路径创建状态，也可能由读注册创建状态。
	auto iter = socketStates_.find(fd);
	if (iter == socketStates_.end())
	{
		SocketStatePtr state(new SocketState(static_cast<KBESOCKET>(fd)));
		iter = socketStates_.insert(std::make_pair(fd, std::move(state))).first;
	}

	SocketState& state = *iter->second;
	state.socket = static_cast<KBESOCKET>(fd);
	return state;
}

//-------------------------------------------------------------------------------------
bool CompletionPoller::tryDetermineSocketKind(KBESOCKET socket, SocketKind& kind) const
{
	// listener 只能在 listen() 之后识别，否则会被当成普通 TCP。
	int socketType = 0;
	int acceptConn = 0;
#if KBE_PLATFORM == PLATFORM_WIN32
	int socketTypeLen = sizeof(socketType);
	int acceptConnLen = sizeof(acceptConn);
	if (getsockopt(socket, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&socketType), &socketTypeLen) != 0)
#else
	socklen_t socketTypeLen = sizeof(socketType);
	socklen_t acceptConnLen = sizeof(acceptConn);
	if (getsockopt(socket, SOL_SOCKET, SO_TYPE, &socketType, &socketTypeLen) != 0)
#endif
	{
		return false;
	}

	if (socketType == SOCK_DGRAM)
	{
		kind = SOCKET_KIND_UDP;
		return true;
	}

#if KBE_PLATFORM == PLATFORM_WIN32
	if (getsockopt(socket, SOL_SOCKET, SO_ACCEPTCONN, reinterpret_cast<char*>(&acceptConn), &acceptConnLen) == 0 && acceptConn != 0)
#else
	if (getsockopt(socket, SOL_SOCKET, SO_ACCEPTCONN, &acceptConn, &acceptConnLen) == 0 && acceptConn != 0)
#endif
	{
		kind = SOCKET_KIND_LISTENER;
		return true;
	}

	if (socketType == SOCK_STREAM)
	{
		kind = SOCKET_KIND_TCP;
		return true;
	}

	return false;
}

//-------------------------------------------------------------------------------------
bool CompletionPoller::pushAcceptedSocket(int fd, KBESOCKET acceptedSocket)
{
	// accept socket 必须等 listener 消费后才会被包装成 EndPoint。
	if (!canQueueAcceptedSocket(fd))
	{
		WARNING_MSG(fmt::format("CompletionPoller::pushAcceptedSocket: accept backlog full, fd={}, items={}, itemLimit={}\n",
			fd, acceptedSocketCount(fd), COMPLETION_ACCEPT_BACKLOG_ITEMS));
		// 这里已经从内核 accept 出来了；如果不能交给上层，就必须立即关闭，
		// 否则 accepted fd 会泄漏，而且客户端侧会误以为连接仍被服务端持有。
		closeSocket(acceptedSocket);
		return false;
	}

	acceptedSockets_[fd].push_back(acceptedSocket);
	return true;
}

//-------------------------------------------------------------------------------------
bool CompletionPoller::pushTcpReceivedData(int fd, std::vector<char>& data, bool disconnected, int errorCode)
{
	// data 用 swap 转移所有权，避免大包复制。
	SocketState& state = socketStateForFd(fd);
	const bool terminal = isTcpTerminalCompletion(data, disconnected, errorCode);
	if (terminal && state.tcpReadTerminated)
	{
		// 同一个 TCP fd 的 EOF/错误 completion 只需要交给上层一次。
		// 断连后某些后端可能因为迟到 completion、kqueue EOF 重复唤醒或旧逻辑重挂 recv，
		// 继续产生 0 字节 completion；这里直接吞掉重复终止事件，避免把 handoff 队列刷满。
		return false;
	}

	if (!canQueueTcpReceivedData(fd, data.size()))
	{
		if (!terminal)
		{
			// 普通 payload completion 仍然严格遵守队列上限，避免慢消费者把内存拖爆。
			WARNING_MSG(fmt::format("CompletionPoller::pushTcpReceivedData: recv backlog full, fd={}, bytes={}, items={}, byteLimit={}, itemLimit={}\n",
				fd, state.pendingTcpReceiveBytes, tcpReceivedItemCount(fd),
				COMPLETION_TCP_RECV_BACKLOG_BYTES, COMPLETION_TCP_RECV_BACKLOG_ITEMS));
			return false;
		}

		// 终止 completion 是 fd 生命周期的最后通知，不能因为 item 上限刚好满了而丢掉。
		// 它通常是 0 字节，只额外占一个队列 item；上面的 tcpReadTerminated 去重保证
		// 不会出现 1024 个 EOF/错误 completion 把队列刷满的情况。
	}

	state.pendingTcpReceiveBytes += data.size();
	if (terminal)
	{
		// 标记终止后，IOCP/io_uring 的自动补投递逻辑会停住；
		// kqueue 也会保持 read filter disabled。这个标记只随 fd 生命周期清理，
		// 不在 takeTcpReceivedData 时重置，避免上层取走 EOF 后又被重复 EOF 唤醒。
		state.tcpReadTerminated = true;
	}

	TcpCompletionData item;
	item.data.swap(data);
	item.disconnected = disconnected;
	item.errorCode = errorCode;
	tcpReceived_[fd].push_back(std::move(item));
	return true;
}

//-------------------------------------------------------------------------------------
bool CompletionPoller::isTcpTerminalCompletion(const std::vector<char>& data, bool disconnected, int errorCode) const
{
	// TCP payload completion 即使 data 为空也不一定代表终止，所以显式看断开/错误标记。
	// 当前所有调用点都只会用 disconnected 或 errorCode 表达 EOF/错误；
	// 发送失败转读侧错误也会带 errorCode，应该走同一条关闭路径。
	(void)data;
	return disconnected || errorCode != 0;
}

//-------------------------------------------------------------------------------------
bool CompletionPoller::pushUdpReceivedData(int fd, std::vector<char>& data, const sockaddr_in& srcAddr, int errorCode)
{
	// UDP completion 保留源地址，避免上层再调用 recvfrom。
	SocketState& state = socketStateForFd(fd);
	if (!canQueueUdpReceivedData(fd, data.size()))
	{
		WARNING_MSG(fmt::format("CompletionPoller::pushUdpReceivedData: recv backlog full, fd={}, bytes={}, items={}, byteLimit={}, itemLimit={}\n",
			fd, state.pendingUdpReceiveBytes, udpReceivedItemCount(fd),
			COMPLETION_UDP_RECV_BACKLOG_BYTES, COMPLETION_UDP_RECV_BACKLOG_ITEMS));
		return false;
	}

	state.pendingUdpReceiveBytes += data.size();
	UdpCompletionData item;
	item.data.swap(data);
	item.srcAddr.ip = srcAddr.sin_addr.s_addr;
	item.srcAddr.port = srcAddr.sin_port;
	item.errorCode = errorCode;
	udpReceived_[fd].push_back(std::move(item));
	return true;
}

//-------------------------------------------------------------------------------------
bool CompletionPoller::canQueueTcpReceivedData(int fd, size_t len) const
{
	// TCP 接收队列必须同时按 bytes 和 item 数有界：
	// 1) bytes 限制大包/流量 burst；
	// 2) item 限制零字节错误、断开事件和大量小包。
	// 这两个条件任意一个达到上限，都不能继续把 completion 搬进用户态队列。
	auto iter = socketStates_.find(fd);
	if (iter == socketStates_.end())
	{
		return len <= COMPLETION_TCP_RECV_BACKLOG_BYTES;
	}

	auto queueIter = tcpReceived_.find(fd);
	const size_t items = queueIter != tcpReceived_.end() ? queueIter->second.size() : 0;
	return items < COMPLETION_TCP_RECV_BACKLOG_ITEMS &&
		iter->second->pendingTcpReceiveBytes + len <= COMPLETION_TCP_RECV_BACKLOG_BYTES;
}

//-------------------------------------------------------------------------------------
bool CompletionPoller::canQueueUdpReceivedData(int fd, size_t len) const
{
	// UDP/KCP datagram 天然按包入队，小包数量可能比总 bytes 更早成为问题。
	// 因此和 TCP 一样使用 bytes + items 双上限。
	auto iter = socketStates_.find(fd);
	if (iter == socketStates_.end())
	{
		return len <= COMPLETION_UDP_RECV_BACKLOG_BYTES;
	}

	auto queueIter = udpReceived_.find(fd);
	const size_t items = queueIter != udpReceived_.end() ? queueIter->second.size() : 0;
	return items < COMPLETION_UDP_RECV_BACKLOG_ITEMS &&
		iter->second->pendingUdpReceiveBytes + len <= COMPLETION_UDP_RECV_BACKLOG_BYTES;
}

//-------------------------------------------------------------------------------------
void CompletionPoller::clearPendingSends(SocketState& state)
{
	// 发送队列和字节计数必须一起清理。
	// 之前多个后端各自写一遍 clear + 归零，未来一旦新增队列字段很容易漏改；
	// 这里集中处理，保证 TCP/UDP 两条发送路径的状态始终同步。
	state.pendingTcpSends.clear();
	state.pendingTcpSendBytes = 0;
	state.pendingUdpSends.clear();
	state.pendingUdpSendBytes = 0;
}

//-------------------------------------------------------------------------------------
bool CompletionPoller::popTcpSendBatch(SocketState& state, size_t maxBytes, std::vector<char>& batch)
{
	// maxBytes 是一次异步发送允许持有的最大用户态缓冲。
	// 设上限可以减少 completion 数量，同时避免断线/取消时单个 outstanding buffer 过大。
	batch.clear();
	if (maxBytes == 0 || state.pendingTcpSends.empty())
	{
		return false;
	}

	size_t batchBytes = 0;
	while (!state.pendingTcpSends.empty() && batchBytes < maxBytes)
	{
		std::vector<char>& front = state.pendingTcpSends.front();
		const size_t bytesToAppend = std::min(front.size(), maxBytes - batchBytes);
		batch.insert(batch.end(), front.begin(), front.begin() + bytesToAppend);
		batchBytes += bytesToAppend;
		state.pendingTcpSendBytes -= bytesToAppend;

		if (bytesToAppend == front.size())
		{
			state.pendingTcpSends.pop_front();
		}
		else
		{
			// 只取走队首的一部分时，把未发送的尾部留在队首。
			// 这一步仍然是 O(n) 移动，但只发生在 batch 边界，且比复制整个队列简单可靠。
			front.erase(front.begin(), front.begin() + bytesToAppend);
			break;
		}
	}

	return !batch.empty();
}

//-------------------------------------------------------------------------------------
void CompletionPoller::pushTcpSendFront(SocketState& state, std::vector<char>& data)
{
	// 部分发送完成后的剩余字节必须回到队首，保证后续数据不会乱序。
	// 使用 swap/move 语义转移 buffer，避免把可能较大的剩余包再复制一遍。
	if (data.empty())
	{
		return;
	}

	state.pendingTcpSendBytes += data.size();
	state.pendingTcpSends.push_front(std::move(data));
}

//-------------------------------------------------------------------------------------
bool CompletionPoller::canQueueAcceptedSocket(int fd) const
{
	return acceptedSocketCount(fd) < COMPLETION_ACCEPT_BACKLOG_ITEMS;
}

//-------------------------------------------------------------------------------------
bool CompletionPoller::canArmTcpReceive(int fd) const
{
	// 只给 kqueue readiness adapter 使用。
	// IOCP/io_uring 不用这个水位判断阻止 recv/accept 投递，避免 completion 后端
	// 因用户态 handoff 队列短暂堆积而额外引入读延迟。
	return canQueueTcpReceivedData(fd, PACKET_MAX_SIZE_TCP);
}

//-------------------------------------------------------------------------------------
bool CompletionPoller::canArmUdpReceive(int fd) const
{
	return canQueueUdpReceivedData(fd, PACKET_MAX_SIZE_UDP);
}

//-------------------------------------------------------------------------------------
bool CompletionPoller::shouldResumeTcpReceive(int fd) const
{
	// kqueue 使用此低水位判断恢复 EVFILT_READ。
	// io_uring/IOCP 不需要显式 resume 标记，因为它们每轮都会在没有 outstanding read
	// 且队列重新有空间时自然重新投递。
	auto iter = socketStates_.find(fd);
	if (iter == socketStates_.end())
	{
		return true;
	}

	return tcpReceivedItemCount(fd) <= COMPLETION_TCP_RECV_BACKLOG_ITEMS / COMPLETION_RECV_RESUME_RATIO &&
		iter->second->pendingTcpReceiveBytes <= COMPLETION_TCP_RECV_BACKLOG_BYTES / COMPLETION_RECV_RESUME_RATIO;
}

//-------------------------------------------------------------------------------------
bool CompletionPoller::shouldResumeUdpReceive(int fd) const
{
	auto iter = socketStates_.find(fd);
	if (iter == socketStates_.end())
	{
		return true;
	}

	return udpReceivedItemCount(fd) <= COMPLETION_UDP_RECV_BACKLOG_ITEMS / COMPLETION_RECV_RESUME_RATIO &&
		iter->second->pendingUdpReceiveBytes <= COMPLETION_UDP_RECV_BACKLOG_BYTES / COMPLETION_RECV_RESUME_RATIO;
}

//-------------------------------------------------------------------------------------
void CompletionPoller::clearReceivedData(int fd)
{
	// 读侧注销或 fd 生命周期重置时，旧的接收 completion 不应继续被新 channel 消费。
	// 这里只清 recv/udp 队列，不清 acceptedSockets_：accepted socket 是 listener 生命周期的
	// 独立资源，cleanupStateIfUnused 会在 fd 状态真正不用时统一关闭。
	tcpReceived_.erase(fd);
	udpReceived_.erase(fd);

	auto iter = socketStates_.find(fd);
	if (iter != socketStates_.end())
	{
		iter->second->pendingTcpReceiveBytes = 0;
		iter->second->pendingUdpReceiveBytes = 0;
		iter->second->readBackpressured = false;
		iter->second->tcpReadTerminated = false;
	}
}

//-------------------------------------------------------------------------------------
size_t CompletionPoller::acceptedSocketCount(int fd) const
{
	auto iter = acceptedSockets_.find(fd);
	return iter != acceptedSockets_.end() ? iter->second.size() : 0;
}

//-------------------------------------------------------------------------------------
size_t CompletionPoller::tcpReceivedItemCount(int fd) const
{
	auto iter = tcpReceived_.find(fd);
	return iter != tcpReceived_.end() ? iter->second.size() : 0;
}

//-------------------------------------------------------------------------------------
size_t CompletionPoller::udpReceivedItemCount(int fd) const
{
	auto iter = udpReceived_.find(fd);
	return iter != udpReceived_.end() ? iter->second.size() : 0;
}

//-------------------------------------------------------------------------------------
void CompletionPoller::closeAcceptedSockets(AcceptedSockets& acceptedSockets)
{
	// 关闭还没被 listener 消费的连接，避免 poller 析构时泄漏 fd。
	while (!acceptedSockets.empty())
	{
		KBESOCKET acceptedSocket = acceptedSockets.front();
		acceptedSockets.pop_front();
		if (acceptedSocket != invalidSocket())
		{
			closeSocket(acceptedSocket);
		}
	}
}

//-------------------------------------------------------------------------------------
void CompletionPoller::closeSocket(KBESOCKET socket)
{
	// socket 关闭函数在 Windows 和 Unix 上不同，基类统一封装。
#if KBE_PLATFORM == PLATFORM_WIN32
	closesocket(socket);
#else
	::close(socket);
#endif
}

//-------------------------------------------------------------------------------------
KBESOCKET CompletionPoller::invalidSocket() const
{
	// 不同平台的无效 socket 值不同，清理队列时需要统一判断。
#if KBE_PLATFORM == PLATFORM_WIN32
	return INVALID_SOCKET;
#else
	return -1;
#endif
}

//-------------------------------------------------------------------------------------
void CompletionPoller::cleanupStateIfUnused(int fd)
{
	// 只有 fd 完全没有注册、pending IO、发送积压和接收队列时才移除状态。
	// 特别注意：不能只看 pendingTcpReceiveBytes/pendingUdpReceiveBytes，
	// 因为错误/断开 completion 可能是空 payload，但仍然需要被上层消费。
	auto iter = socketStates_.find(fd);
	if (iter == socketStates_.end())
	{
		return;
	}

	SocketState& state = *iter->second;
	if (state.registeredRead || state.readArmed || state.writeArmed ||
		state.pPendingReadContext != NULL || state.pPendingWriteContext != NULL ||
		state.pendingTcpReceiveBytes > 0 || state.pendingUdpReceiveBytes > 0 ||
		tcpReceivedItemCount(fd) > 0 || udpReceivedItemCount(fd) > 0 ||
		!state.pendingTcpSends.empty() || state.pendingTcpSendBytes > 0 ||
		!state.pendingUdpSends.empty() || state.pendingUdpSendBytes > 0 || this->findForWrite(fd) != NULL)
	{
		return;
	}

	auto acceptedIter = acceptedSockets_.find(fd);
	if (acceptedIter != acceptedSockets_.end())
	{
		closeAcceptedSockets(acceptedIter->second);
		acceptedSockets_.erase(acceptedIter);
	}

	socketStates_.erase(iter);
}

}
}
