// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

#include "poller_iocp.h"

#if KBE_PLATFORM == PLATFORM_WIN32

#include "helper/profile.h"
#include <cmath>

namespace KBEngine {
namespace
{
ProfileVal g_iocpIdleProfile("Idle");
}

namespace Network
{

namespace
{
const size_t IOCP_TCP_SEND_BATCH_BYTES = 64 * 1024;
const size_t COMPLETION_CONTEXT_CACHE_LIMIT = 256;
const size_t COMPLETION_CONTEXT_RETAINED_BUFFER_BYTES = 64 * 1024;
// 析构排空设置有限等待，避免异常驱动或损坏的 socket 让进程永久卡住；超时对象保留到进程退出以保证内核访问安全。
// Bound destructor draining so a faulty driver or socket cannot hang shutdown forever; timed-out storage survives until process exit for kernel safety.
const DWORD IOCP_SHUTDOWN_DRAIN_TIMEOUT_MS = 5000;
// Keep a bounded completion batch until the 1.x watcher configuration exposes runtime budgets.
// 在 1.x watcher 配置提供运行时预算前，先使用有界完成批次。
// 预算告警只是诊断“主循环被 completion 回调拖太久”，不是限流开关。
const uint64 COMPLETION_BUDGET_WARNING_INTERVAL = 10 * stampsPerSecond();
const uint32 COMPLETION_BUDGET_WARNING_MULTIPLIER = 10;

inline DWORD toTimeoutMilliseconds(double maxWait)
{
	double waitSeconds = maxWait;

	if (waitSeconds <= 0.0)
	{
		return 0;
	}

	double milliseconds = std::ceil(waitSeconds * 1000.0);
	if (milliseconds > static_cast<double>(INFINITE - 1))
	{
		return INFINITE - 1;
	}

	return static_cast<DWORD>(milliseconds);
}
}

IocpPoller::IocpContext::IocpContext() :
	overlapped(),
	fd(INVALID_SOCKET),
	socket(INVALID_SOCKET),
	kind(SOCKET_KIND_UNKNOWN),
	operation(OP_ACCEPT),
	generation(0),
	buffer(),
	flags(0),
	data(),
	tcpSendData(),
	acceptSocket(INVALID_SOCKET),
	acceptBuffer(),
	udpAddr(),
	udpAddrLen(sizeof(udpAddr))
{
	memset(&overlapped, 0, sizeof(overlapped));
	memset(&buffer, 0, sizeof(buffer));
	memset(&udpAddr, 0, sizeof(udpAddr));
	memset(acceptBuffer, 0, sizeof(acceptBuffer));
}

//-------------------------------------------------------------------------------------
void IocpPoller::IocpContext::reset(KBESOCKET fdArg, KBESOCKET socketArg, SocketKind kindArg, Operation operationArg, uint64 generationArg)
{
	// Every kernel-visible field is rebuilt before reuse; vector capacity is retained only within the bounded context budget.
	// 每次复用前都重建所有内核可见字段；vector 容量只在有界 context 预算内保留。
	memset(&overlapped, 0, sizeof(overlapped));
	fd = fdArg;
	socket = socketArg;
	kind = kindArg;
	operation = operationArg;
	generation = generationArg;
	memset(&buffer, 0, sizeof(buffer));
	flags = 0;
	data.clear();
	if (data.capacity() > COMPLETION_CONTEXT_RETAINED_BUFFER_BYTES)
	{
		std::vector<char>().swap(data);
	}
	tcpSendData.reset(COMPLETION_CONTEXT_RETAINED_BUFFER_BYTES);
	acceptSocket = INVALID_SOCKET;
	memset(acceptBuffer, 0, sizeof(acceptBuffer));
	memset(&udpAddr, 0, sizeof(udpAddr));
	udpAddrLen = sizeof(udpAddr);
}

size_t IocpPoller::IocpContext::retainedBytes() const
{
	return data.capacity() + tcpSendData.capacity();
}

//-------------------------------------------------------------------------------------
IocpPoller::IocpPoller() :
	CompletionPoller(),
	outstandingContexts_(),
	contextPool_(COMPLETION_CONTEXT_CACHE_LIMIT),
	completionPort_(CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0)),
	lastCompletionBudgetWarningTime_(0),
	pendingCompletions_(),
	completionDequeueCallCount_(0),
	completionDequeuedCount_(0),
	completionMaxDequeuedBatchCount_(0)
{
	if (completionPort_ == NULL)
	{
		ERROR_MSG(fmt::format("IocpPoller::IocpPoller: CreateIoCompletionPort failed: {}\n",
			kbe_strerror(GetLastError())));
	}
}

//-------------------------------------------------------------------------------------
uint64 IocpPoller::contextAllocationCount() const { return contextPool_.allocationCount(); }
uint64 IocpPoller::contextReuseCount() const { return contextPool_.reuseCount(); }
uint64 IocpPoller::contextOutstandingCount() const { return contextPool_.outstandingCount(); }
uint64 IocpPoller::contextCachedCount() const { return contextPool_.cachedCount(); }
uint64 IocpPoller::contextPeakOutstandingCount() const { return contextPool_.peakOutstandingCount(); }
uint64 IocpPoller::contextOutstandingBytes() const
{
	size_t bytes = 0;
	for (const OutstandingContexts::value_type& item : outstandingContexts_)
	{
		if (item.second != NULL)
		{
			bytes += item.second->retainedBytes();
		}
	}
	return static_cast<uint64>(bytes);
}
uint64 IocpPoller::contextCachedBytes() const { return static_cast<uint64>(contextPool_.cachedBytes()); }
uint64 IocpPoller::completionDequeueCallCount() const { return completionDequeueCallCount_; }
uint64 IocpPoller::completionDequeuedCount() const { return completionDequeuedCount_; }
uint64 IocpPoller::completionMaxDequeuedBatchCount() const { return completionMaxDequeuedBatchCount_; }
uint64 IocpPoller::completionPendingLocalCount() const { return static_cast<uint64>(pendingCompletions_.size()); }

//-------------------------------------------------------------------------------------
IocpPoller::~IocpPoller()
{
	cancelAndDrainContexts();

	if (completionPort_ != NULL)
	{
		CloseHandle(completionPort_);
		completionPort_ = NULL;
	}
}

//-------------------------------------------------------------------------------------
bool IocpPoller::queueTcpSend(KBESOCKET fd, const void* data, int len)
{
	if (len <= 0)
	{
		return true;
	}

	// IOCP 关联必须在复制数据之前成功，否则失败数据会留在 pending 队列并在每个 Tick 永久重试。
	// IOCP association must succeed before copying data, or failed bytes remain pending and retry forever on every tick.
	auto iter = socketStates_.find(fd);
	if (iter == socketStates_.end())
	{
		SocketKind detectedKind = SOCKET_KIND_UNKNOWN;
		if (!tryDetermineSocketKind(fd, detectedKind))
		{
			return false;
		}

		if (detectedKind != SOCKET_KIND_TCP)
		{
			WSASetLastError(WSAESOCKTNOSUPPORT);
			return false;
		}
	}

	SocketState& state = socketStateForFd(fd);
	if (state.kind == SOCKET_KIND_UNKNOWN)
	{
		state.kind = SOCKET_KIND_TCP;
	}

	if (!ensureAssociated(state, fd))
	{
		cleanupStateIfUnused(fd);
		return false;
	}

	// 关联成功后共享基类才接管数据所有权，随后首次 WSASend 失败也由 completion 生命周期处理。
	// The shared queue takes ownership only after association; completion lifecycle then handles a failed first WSASend attempt.
	if (!CompletionPoller::queueTcpSend(fd, data, len))
	{
		return false;
	}

	// Once the shared queue accepts bytes, ownership has moved to the poller even if the first WSASend attempt fails.
	// 共享队列接受字节后，数据所有权已经转移给 poller，即使首次 WSASend 投递失败也由后端继续重试。
	if (!armTcpSend(fd, state))
	{
		requestRearm(fd, REARM_WRITE);
	}
	return true;
}

//-------------------------------------------------------------------------------------
bool IocpPoller::queueUdpSend(KBESOCKET fd, const void* data, int len, const Address& dstAddr)
{
	// UDP/KCP 也保持入队后立即投递，避免等待下一次主循环才开始发送。
	if (!CompletionPoller::queueUdpSend(fd, data, len, dstAddr))
	{
		return false;
	}

	SocketState& state = socketStateForFd(fd);
	// Once the shared queue accepts a datagram, later completion rounds own its retry lifecycle.
	// 共享队列接受数据报后，后续 completion 轮次负责其重试生命周期。
	if (!armUdpSend(fd, state))
	{
		requestRearm(fd, REARM_WRITE);
	}
	return true;
}

//-------------------------------------------------------------------------------------
bool IocpPoller::ensureAssociated(SocketState& state, KBESOCKET fd)
{
	if (state.associated)
	{
		return true;
	}

	HANDLE handle = CreateIoCompletionPort(reinterpret_cast<HANDLE>(state.socket), completionPort_, static_cast<ULONG_PTR>(fd), 0);
	if (handle == NULL)
	{
		const DWORD errorCode = GetLastError();
		ERROR_MSG(fmt::format("IocpPoller::ensureAssociated: CreateIoCompletionPort failed for fd {}, socket={}, kind={}, registeredRead={}, tcpPendingBytes={}, udpPendingBytes={}: {}\n",
			fd, static_cast<uint64>(state.socket), static_cast<int>(state.kind), state.registeredRead,
			state.pendingTcpSends.pendingBytes(), state.pendingUdpSendBytes, kbe_strerror(errorCode)));
		// 日志转发可能执行其他 Winsock 调用；恢复原错误码，确保发送器能正确区分背压和失效连接。
		// Log forwarding may execute other Winsock calls; restore the original code so the sender can classify backpressure versus failure.
		WSASetLastError(static_cast<int>(errorCode));
		return false;
	}

	state.associated = true;
	return true;
}

//-------------------------------------------------------------------------------------
bool IocpPoller::loadAcceptEx(SocketState& state)
{
	if (state.acceptExFn != NULL)
	{
		return true;
	}

	DWORD bytes = 0;
	GUID guidAcceptEx = WSAID_ACCEPTEX;
	if (WSAIoctl(state.socket, SIO_GET_EXTENSION_FUNCTION_POINTER,
		&guidAcceptEx, sizeof(guidAcceptEx),
		&state.acceptExFn, sizeof(state.acceptExFn),
		&bytes, NULL, NULL) != 0)
	{
		ERROR_MSG(fmt::format("IocpPoller::loadAcceptEx: WSAIoctl failed: {}\n",
			kbe_strerror(WSAGetLastError())));
		state.acceptExFn = NULL;
		return false;
	}

	return true;
}

//-------------------------------------------------------------------------------------
bool IocpPoller::armTcpRead(KBESOCKET fd, SocketState& state)
{
	IocpContext* pContext = acquireContext(fd, state.socket, SOCKET_KIND_TCP, OP_TCP_RECV, state.generation);
	pContext->data.resize(PACKET_MAX_SIZE_TCP);
	pContext->buffer.buf = pContext->data.data();
	pContext->buffer.len = static_cast<ULONG>(pContext->data.size());

	DWORD bytes = 0;
	DWORD flags = 0;
	int ret = WSARecv(state.socket, &pContext->buffer, 1, &bytes, &flags, &pContext->overlapped, NULL);
	if (ret == 0)
	{
		trackContext(*pContext);
		state.pPendingReadContext = pContext;
		return true;
	}

	int wsaErr = WSAGetLastError();
	if (wsaErr == WSA_IO_PENDING)
	{
		trackContext(*pContext);
		state.pPendingReadContext = pContext;
		return true;
	}

	recycleContext(pContext);
	return false;
}

//-------------------------------------------------------------------------------------
bool IocpPoller::armUdpRead(KBESOCKET fd, SocketState& state)
{
	IocpContext* pContext = acquireContext(fd, state.socket, SOCKET_KIND_UDP, OP_UDP_RECV, state.generation);
	pContext->flags = 0;
	pContext->data.resize(PACKET_MAX_SIZE_UDP);
	pContext->buffer.buf = pContext->data.data();
	pContext->buffer.len = static_cast<ULONG>(pContext->data.size());

	DWORD bytes = 0;
	int ret = WSARecvFrom(state.socket, &pContext->buffer, 1, &bytes, &pContext->flags,
		reinterpret_cast<sockaddr*>(&pContext->udpAddr), &pContext->udpAddrLen,
		&pContext->overlapped, NULL);

	if (ret == 0)
	{
		trackContext(*pContext);
		state.pendingReadContexts.insert(pContext);
		return true;
	}

	int wsaErr = WSAGetLastError();
	if (wsaErr == WSA_IO_PENDING)
	{
		trackContext(*pContext);
		state.pendingReadContexts.insert(pContext);
		return true;
	}

	recycleContext(pContext);
	return false;
}

//-------------------------------------------------------------------------------------
uint32 IocpPoller::udpReceiveDepth(const SocketState& state) const
{
	// Bots connect each UDP socket to exactly one BaseApp, while a BaseApp listener
	// remains unconnected and receives all client datagrams. getpeername therefore
	// gives us a transport-level distinction without coupling network code to component types.
	// Bots 的 UDP socket 会 connect 到单个 BaseApp，而 BaseApp listener 保持未连接并接收
	// 所有客户端数据。使用 getpeername 可在网络层区分两者，不依赖具体组件类型。
	sockaddr_storage peerAddress;
	int peerAddressLength = sizeof(peerAddress);
	memset(&peerAddress, 0, sizeof(peerAddress));
	const bool connected = getpeername(state.socket,
		reinterpret_cast<sockaddr*>(&peerAddress), &peerAddressLength) == 0;
	return iocpUdpReceiveDepth(connected);
}

//-------------------------------------------------------------------------------------
bool IocpPoller::isReadArmComplete(const SocketState& state) const
{
	if (state.kind == SOCKET_KIND_UDP)
		return state.pendingReadContexts.size() >= udpReceiveDepth(state);

	return state.pPendingReadContext != NULL;
}

//-------------------------------------------------------------------------------------
bool IocpPoller::ensureUdpReadsArmed(KBESOCKET fd, SocketState& state)
{
	const uint32 targetDepth = udpReceiveDepth(state);
	while (state.registeredRead && state.pendingReadContexts.size() < targetDepth)
	{
		if (!armUdpRead(fd, state))
			break;
	}

	// One successfully armed receive is enough to keep the registration valid.
	// A partial fill is retried through REARM_READ without discarding live operations.
	// 只要至少一个接收已挂起，注册就是有效的；未补满部分交给 REARM_READ 重试。
	return !state.pendingReadContexts.empty();
}

//-------------------------------------------------------------------------------------
bool IocpPoller::armTcpSend(KBESOCKET fd, SocketState& state)
{
	if (state.pPendingWriteContext != NULL || state.pendingTcpSends.empty())
	{
		return true;
	}

	if (!ensureAssociated(state, fd))
	{
		return false;
	}

	IocpContext* pContext = acquireContext(fd, state.socket, SOCKET_KIND_TCP, OP_TCP_SEND, state.generation);
	// 合并多个小包为一次 WSASend，减少 IOCP completion 数量。
	// 不能无限合并，否则一次 completion 回调可能占用过久，也会增加
	// 断线时被取消的 outstanding buffer 大小。
	bool copied = false;
	if (!popTcpSendBatch(state, IOCP_TCP_SEND_BATCH_BYTES, pContext->tcpSendData, copied))
	{
		recycleContext(pContext);
		return true;
	}
	(void)copied;

	pContext->buffer.buf = pContext->tcpSendData.data();
	pContext->buffer.len = static_cast<ULONG>(pContext->tcpSendData.size());

	DWORD bytes = 0;
	int ret = WSASend(state.socket, &pContext->buffer, 1, &bytes, 0, &pContext->overlapped, NULL);
	if (ret == 0)
	{
		trackContext(*pContext);
		state.pPendingWriteContext = pContext;
		return true;
	}

	int wsaErr = WSAGetLastError();
	if (wsaErr == WSA_IO_PENDING)
	{
		trackContext(*pContext);
		state.pPendingWriteContext = pContext;
		return true;
	}

	// WSASend did not take ownership of the buffer; restore the batch before retrying later.
	// WSASend 未接管缓冲区所有权，失败时先恢复 batch，等待后续轮次重试。
	pushTcpSendFront(state, pContext->tcpSendData, 0);
	recycleContext(pContext);
	return false;
}

//-------------------------------------------------------------------------------------
bool IocpPoller::armUdpSend(KBESOCKET fd, SocketState& state)
{
	if (state.pPendingWriteContext != NULL || state.pendingUdpSends.empty())
	{
		return true;
	}

	if (!ensureAssociated(state, fd))
	{
		return false;
	}

	IocpContext* pContext = acquireContext(fd, state.socket, SOCKET_KIND_UDP, OP_UDP_SEND, state.generation);
	PendingUdpSend pending;
	dequeueUdpSend(state, pending);
	pContext->data.swap(pending.data);
	pContext->udpAddr = pending.dstAddr;
	pContext->udpAddrLen = sizeof(pContext->udpAddr);
	pContext->buffer.buf = pContext->data.data();
	pContext->buffer.len = static_cast<ULONG>(pContext->data.size());

	DWORD bytes = 0;
	int ret = WSASendTo(state.socket, &pContext->buffer, 1, &bytes, 0,
		reinterpret_cast<sockaddr*>(&pContext->udpAddr), pContext->udpAddrLen,
		&pContext->overlapped, NULL);
	if (ret == 0)
	{
		trackContext(*pContext);
		state.pPendingWriteContext = pContext;
		return true;
	}

	int wsaErr = WSAGetLastError();
	if (wsaErr == WSA_IO_PENDING)
	{
		trackContext(*pContext);
		state.pPendingWriteContext = pContext;
		return true;
	}

	// WSASendTo did not take ownership of the datagram; put it back at the queue front.
	// WSASendTo 未接管数据报所有权，将其放回队首等待后续重试。
	pending.data.swap(pContext->data);
	restoreUdpSendFront(state, std::move(pending));
	recycleContext(pContext);
	return false;
}

//-------------------------------------------------------------------------------------
bool IocpPoller::armAccept(KBESOCKET fd, SocketState& state)
{
	if (!loadAcceptEx(state))
	{
		return false;
	}

	IocpContext* pContext = acquireContext(fd, state.socket, SOCKET_KIND_LISTENER, OP_ACCEPT, state.generation);

	pContext->acceptSocket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (pContext->acceptSocket == INVALID_SOCKET)
	{
		ERROR_MSG(fmt::format("IocpPoller::armAccept: WSASocket failed: {}\n",
			kbe_strerror(WSAGetLastError())));
		recycleContext(pContext);
		return false;
	}

	DWORD bytes = 0;
	BOOL ok = state.acceptExFn(state.socket, pContext->acceptSocket,
		pContext->acceptBuffer, 0,
		sizeof(sockaddr_in) + 16,
		sizeof(sockaddr_in) + 16,
		&bytes, &pContext->overlapped);

	if (ok)
	{
		trackContext(*pContext);
		state.pPendingReadContext = pContext;
		return true;
	}

	int wsaErr = WSAGetLastError();
	if (wsaErr == ERROR_IO_PENDING)
	{
		trackContext(*pContext);
		state.pPendingReadContext = pContext;
		return true;
	}

	WARNING_MSG(fmt::format("IocpPoller::armAccept: AcceptEx failed on fd {}: {}\n",
		fd, kbe_strerror(wsaErr)));
	cleanupContext(*pContext);
	recycleContext(pContext);
	return false;
}

//-------------------------------------------------------------------------------------
bool IocpPoller::ensureReadArmed(KBESOCKET fd, SocketState& state)
{
	// TCP and AcceptEx keep one outstanding read. UDP keeps a bounded receive set so
	// a completion waiting in the IOCP queue does not leave the socket without a buffer.
	// TCP/AcceptEx 保持一个 outstanding read；UDP 使用有界接收集合，避免 completion
	// 尚在 IOCP 队列等待时 socket 已无接收缓冲。
	if (!state.registeredRead)
	{
		return true;
	}

	if (state.kind == SOCKET_KIND_UDP)
		return ensureUdpReadsArmed(fd, state);

	if (state.pPendingReadContext != NULL)
		return true;

	SocketKind detectedKind = SOCKET_KIND_UNKNOWN;
	if (!tryDetermineSocketKind(state.socket, detectedKind))
	{
		return false;
	}

	if (detectedKind == SOCKET_KIND_TCP)
	{
		sockaddr_storage peerAddress;
		int peerAddressLength = sizeof(peerAddress);
		memset(&peerAddress, 0, sizeof(peerAddress));

		if (getpeername(state.socket, reinterpret_cast<sockaddr*>(&peerAddress), &peerAddressLength) == SOCKET_ERROR)
		{
			int errorCode = WSAGetLastError();

			// 1.x 会在 connect 或 listen 之前登记读 handler；此时先保留登记，等 socket 就绪后再关联 IOCP。
			// 1.x registers read handlers before connect or listen; retain the registration and defer IOCP association until the socket is ready.
			if (errorCode == WSAENOTCONN || errorCode == WSAEINVAL || errorCode == WSAEWOULDBLOCK)
				return true;

			ERROR_MSG(fmt::format("IocpPoller::ensureReadArmed: getpeername failed for fd {}: {}\n",
				fd, kbe_strerror(errorCode)));
			return false;
		}
	}

	// 只有可立即投递 OVERLAPPED 操作的 socket 才进行永久 IOCP 关联，避免失败后丢失关联状态。
	// Associate with IOCP only when an OVERLAPPED operation can be submitted immediately, avoiding lost association state after an arm failure.
	if (!ensureAssociated(state, fd))
	{
		return false;
	}

	state.kind = detectedKind;

	switch (state.kind)
	{
	case SOCKET_KIND_TCP:
		return armTcpRead(fd, state);
	case SOCKET_KIND_UDP:
		return ensureUdpReadsArmed(fd, state);
	case SOCKET_KIND_LISTENER:
		return armAccept(fd, state);
	default:
		return false;
	}
}

//-------------------------------------------------------------------------------------
void IocpPoller::processRearmRequests()
{
	const size_t requestCount = rearmBatchSize();
	for (size_t i = 0; i < requestCount; ++i)
	{
		KBESOCKET fd = INVALID_SOCKET;
		uint8 flags = REARM_NONE;
		if (!takeRearmRequest(fd, flags))
		{
			break;
		}

		SocketStates::iterator iter = socketStates_.find(fd);
		if (iter == socketStates_.end())
		{
			continue;
		}

		SocketState& state = *iter->second;
		if ((flags & REARM_READ) != 0 && state.registeredRead && !isReadArmComplete(state))
		{
			const bool armed = ensureReadArmed(fd, state);
			const bool complete = armed && isReadArmComplete(state);
			recordRearmAttempt(!complete);
			if (!complete && state.registeredRead)
			{
				requestRearm(fd, REARM_READ);
			}
		}

		if ((flags & REARM_WRITE) != 0 && state.pPendingWriteContext == NULL)
		{
			bool attempted = false;
			bool armed = true;
			if (!state.pendingTcpSends.empty())
			{
				attempted = true;
				armed = armTcpSend(fd, state);
			}
			else if (!state.pendingUdpSends.empty())
			{
				attempted = true;
				armed = armUdpSend(fd, state);
			}

			const bool retryRequired = !armed &&
				(!state.pendingTcpSends.empty() || !state.pendingUdpSends.empty());
			if (attempted)
			{
				recordRearmAttempt(retryRequired);
			}
			if (retryRequired)
			{
				requestRearm(fd, REARM_WRITE);
			}
		}
	}
}

//-------------------------------------------------------------------------------------
bool IocpPoller::doRegisterForRead(KBESOCKET fd)
{
	auto iter = socketStates_.find(fd);
	bool isNewState = false;
	if (iter == socketStates_.end())
	{
		SocketStatePtr state(new SocketState(fd));
		iter = socketStates_.insert(std::make_pair(fd, std::move(state))).first;
		isNewState = true;
	}

	SocketState& state = *iter->second;
	state.socket = fd;
	state.registeredRead = true;

	// 只有新建状态才代表新的 socket 生命周期；发送路径预先创建的状态仍属于同一 socket，不能遗忘其永久 IOCP 关联。
	// Only a newly created state starts a socket lifecycle; a state created by the send path is the same socket and must retain its permanent IOCP association.
	if (isNewState)
	{
		clearReceivedData(fd);
		state.kind = SOCKET_KIND_UNKNOWN;
		state.associated = false;
		++state.generation;
		state.acceptExFn = NULL;
	}

	const bool armed = ensureReadArmed(fd, *iter->second);
	if (!armed)
	{
		// Do not leave a handler registered when the first overlapped operation could not be armed.
		// 首次异步操作投递失败时不能继续保留 handler，避免上层永久等待一个不会到来的事件。
		iter->second->registeredRead = false;
		cleanupStateIfUnused(fd);
	}
	else if (!hasPendingReadContext(fd) || !isReadArmComplete(*iter->second))
	{
		// connect/listen 前的 1.x 注册会被延迟；只登记该 fd，而不是依赖全表轮询发现它。
		// A 1.x registration before connect/listen is deferred; enqueue this fd instead of rediscovering it through a full-table poll.
		requestRearm(fd, REARM_READ);
	}

	return armed;
}

//-------------------------------------------------------------------------------------
bool IocpPoller::doRegisterForWrite(KBESOCKET fd)
{
	(void)fd;
	return true;
}

//-------------------------------------------------------------------------------------
bool IocpPoller::doDeregisterForRead(KBESOCKET fd)
{
	auto iter = socketStates_.find(fd);
	if (iter == socketStates_.end())
	{
		return false;
	}

	SocketState& state = *iter->second;
	state.registeredRead = false;
	cancelRearm(fd, REARM_READ);
	clearReceivedData(fd);

	// 读注销表示 channel/socket 生命周期结束，读写 outstanding IO 都要取消。
	// CancelIoEx 后 completion 仍可能异步返回，所以这里只断开 state 引用，
	// 真正的 context 内存在 handleCompletion 收到 ERROR_OPERATION_ABORTED 后释放。
	if (state.pPendingReadContext != NULL)
	{
		IocpContext* pContext = reinterpret_cast<IocpContext*>(state.pPendingReadContext);
		CancelIoEx(reinterpret_cast<HANDLE>(state.socket), &pContext->overlapped);
		state.pPendingReadContext = NULL;
	}

	for (void* pendingContext : state.pendingReadContexts)
	{
		IocpContext* pContext = reinterpret_cast<IocpContext*>(pendingContext);
		CancelIoEx(reinterpret_cast<HANDLE>(state.socket), &pContext->overlapped);
	}
	state.pendingReadContexts.clear();

	if (state.pPendingWriteContext != NULL)
	{
		IocpContext* pContext = reinterpret_cast<IocpContext*>(state.pPendingWriteContext);
		CancelIoEx(reinterpret_cast<HANDLE>(state.socket), &pContext->overlapped);
		state.pPendingWriteContext = NULL;
	}

	clearPendingSends(state);
	++state.generation;

	cleanupStateIfUnused(fd);

	return true;
}

//-------------------------------------------------------------------------------------
bool IocpPoller::doDeregisterForWrite(KBESOCKET fd)
{
	auto iter = socketStates_.find(fd);
	if (iter == socketStates_.end())
	{
		return true;
	}

	SocketState& state = *iter->second;
	clearPendingSends(state);
	cancelRearm(fd, REARM_WRITE);

	// 写注销只停止发送队列，不能清 tcpReceived_/udpReceived_。
	// completion 线程在 handleCompletion 中会先把 recv 数据入队再 triggerRead；
	// 如果 onSendCompleted/stopSend 清掉接收队列，登录/断线消息会被吞掉。
	if (state.pPendingWriteContext != NULL)
	{
		IocpContext* pContext = reinterpret_cast<IocpContext*>(state.pPendingWriteContext);
		CancelIoEx(reinterpret_cast<HANDLE>(state.socket), &pContext->overlapped);
		state.pPendingWriteContext = NULL;
	}

	cleanupStateIfUnused(fd);
	return true;
}

//-------------------------------------------------------------------------------------
void IocpPoller::trackContext(IocpContext& context)
{
	const std::pair<OutstandingContexts::iterator, bool> inserted =
		outstandingContexts_.insert(std::make_pair(&context.overlapped, &context));
	KBE_ASSERT(inserted.second);
}

//-------------------------------------------------------------------------------------
void IocpPoller::cleanupContext(IocpContext& context)
{
	if (context.acceptSocket != INVALID_SOCKET)
	{
		closesocket(context.acceptSocket);
		context.acceptSocket = INVALID_SOCKET;
	}
}

//-------------------------------------------------------------------------------------
IocpPoller::IocpContext* IocpPoller::acquireContext(KBESOCKET fd, KBESOCKET socket, SocketKind kind, Operation operation, uint64 generation)
{
	IocpContext* context = contextPool_.acquire();
	context->reset(fd, socket, kind, operation, generation);
	return context;
}

//-------------------------------------------------------------------------------------
void IocpPoller::recycleContext(IocpContext* context)
{
	if (context == NULL)
	{
		return;
	}

	cleanupContext(*context);
	contextPool_.release(context);
}

//-------------------------------------------------------------------------------------
void IocpPoller::releaseContext(IocpContext& context)
{
	outstandingContexts_.erase(&context.overlapped);
	recycleContext(&context);
}

//-------------------------------------------------------------------------------------
void IocpPoller::cancelAndDrainContexts()
{
	if (completionPort_ == NULL)
		return;

	// GetQueuedCompletionStatusEx may already have removed these completions from
	// the kernel queue. They are safe to release during shutdown and must not be
	// left outside outstandingContexts_. / 批量 API 已从内核队列取出的完成项在
	// 关停时已经安全可释放，不能遗留在 outstandingContexts_ 之外。
	releasePendingCompletions();
	if (outstandingContexts_.empty())
		return;

	// CancelIoEx 只发起取消；OVERLAPPED 仍由内核持有，必须等对应完成包出队后才能释放。
	// CancelIoEx only requests cancellation; the kernel retains each OVERLAPPED until its completion packet is dequeued.
	for (OutstandingContexts::value_type& item : outstandingContexts_)
	{
		IocpContext* pContext = item.second;
		CancelIoEx(reinterpret_cast<HANDLE>(pContext->socket), &pContext->overlapped);
	}

	const ULONGLONG deadline = GetTickCount64() + IOCP_SHUTDOWN_DRAIN_TIMEOUT_MS;
	while (!outstandingContexts_.empty() && GetTickCount64() < deadline)
	{
		DWORD bytesTransferred = 0;
		ULONG_PTR completionKey = 0;
		LPOVERLAPPED overlapped = NULL;
		const ULONGLONG now = GetTickCount64();
		const DWORD waitMilliseconds = static_cast<DWORD>(std::min<ULONGLONG>(50, deadline - now));
		GetQueuedCompletionStatus(completionPort_, &bytesTransferred, &completionKey, &overlapped, waitMilliseconds);

		OutstandingContexts::iterator iter = outstandingContexts_.find(overlapped);
		if (iter != outstandingContexts_.end())
			releaseContext(*iter->second);
	}

	if (!outstandingContexts_.empty())
	{
		// 超时 context 不能释放，否则迟到的内核写入会形成 UAF；进程退出时由操作系统回收。
		// Timed-out contexts must stay allocated to avoid a late kernel write causing UAF; the operating system reclaims them at process exit.
		ERROR_MSG(fmt::format("IocpPoller::cancelAndDrainContexts: {} contexts did not complete within {} ms; preserving their OVERLAPPED storage.\n",
			outstandingContexts_.size(), IOCP_SHUTDOWN_DRAIN_TIMEOUT_MS));
	}
}

//-------------------------------------------------------------------------------------
void IocpPoller::releasePendingCompletions()
{
	while (!pendingCompletions_.empty())
	{
		const PendingCompletion pending = pendingCompletions_.front();
		pendingCompletions_.pop_front();
		OutstandingContexts::iterator iter = outstandingContexts_.find(pending.overlapped);
		if (iter != outstandingContexts_.end())
			releaseContext(*iter->second);
	}
}

//-------------------------------------------------------------------------------------
DWORD IocpPoller::dequeueCompletions(DWORD timeoutMs)
{
	static const ULONG IOCP_DEQUEUE_BATCH_SIZE = 128;
	OVERLAPPED_ENTRY entries[IOCP_DEQUEUE_BATCH_SIZE];
	ULONG removed = 0;
	memset(entries, 0, sizeof(entries));
	++completionDequeueCallCount_;
	const BOOL ok = GetQueuedCompletionStatusEx(completionPort_, entries,
		IOCP_DEQUEUE_BATCH_SIZE, &removed, timeoutMs, FALSE);
	if (!ok)
		return GetLastError();

	completionDequeuedCount_ += removed;
	completionMaxDequeuedBatchCount_ = std::max<uint64>(completionMaxDequeuedBatchCount_, removed);
	for (ULONG index = 0; index < removed; ++index)
	{
		PendingCompletion pending;
		pending.completionKey = entries[index].lpCompletionKey;
		pending.overlapped = entries[index].lpOverlapped;
		pending.bytesTransferred = entries[index].dwNumberOfBytesTransferred;
		pending.success = entries[index].Internal == 0;
		// Internal is an NTSTATUS, not a Winsock error. handleCompletion will
		// normalize it with WSAGetOverlappedResult before classifying the event.
		// Internal 是 NTSTATUS，不是 Winsock 错误；后续统一由
		// WSAGetOverlappedResult 转换，避免把取消误判成业务错误。
		pending.errorCode = pending.success ? 0 : static_cast<DWORD>(entries[index].Internal);
		pendingCompletions_.push_back(pending);
	}
	return ERROR_SUCCESS;
}

//-------------------------------------------------------------------------------------
void IocpPoller::handleCompletion(ULONG_PTR completionKey, LPOVERLAPPED overlapped, DWORD bytesTransferred, bool success, DWORD errorCode)
{
	if (overlapped == NULL)
	{
		return;
	}

	OutstandingContexts::iterator outstandingIter = outstandingContexts_.find(overlapped);
	if (outstandingIter == outstandingContexts_.end())
	{
		ERROR_MSG("IocpPoller::handleCompletion: received an untracked OVERLAPPED context.\n");
		return;
	}

	IocpContext* pContext = outstandingIter->second;
	const KBESOCKET fd = pContext->fd;

	auto iter = socketStates_.find(fd);
	const bool hasState = (iter != socketStates_.end());
	SocketState* pState = hasState ? iter->second.get() : NULL;
	void** ppCurrentContext = NULL;
	const bool isUdpReadContext = pContext->operation == OP_UDP_RECV;
	if (pState != NULL && !isUdpReadContext)
	{
		ppCurrentContext = (pContext->operation == OP_TCP_SEND || pContext->operation == OP_UDP_SEND) ?
			&pState->pPendingWriteContext : &pState->pPendingReadContext;
	}

	// completion key、context 和当前状态必须指向同一个完整宽度句柄，任一不匹配都表示迟到或错误投递。
	// Completion key, context, and current state must identify the same full-width handle; any mismatch is stale or misrouted work.
	const bool isCurrentContext = (pState != NULL &&
		completionKey == static_cast<ULONG_PTR>(fd) &&
		pState->socket == pContext->socket &&
		pState->generation == pContext->generation &&
		(isUdpReadContext ?
			pState->pendingReadContexts.find(pContext) != pState->pendingReadContexts.end() :
			(ppCurrentContext != NULL && *ppCurrentContext == pContext)));
	// Both GQCS and GQCSEx can expose a non-Winsock completion status. Normalize
	// before classifying cancellation, disconnect, or UDP port-unreachable paths.
	// GQCS/GQCSEx 都可能给出非 Winsock 状态；必须先规范化，再判断取消、断线和 UDP 端口不可达。
	int socketErrorCode = static_cast<int>(errorCode);
	if (!success)
	{
		DWORD transferred = bytesTransferred;
		DWORD flags = pContext->flags;
		if (!WSAGetOverlappedResult(pContext->socket, &pContext->overlapped, &transferred, FALSE, &flags))
		{
			const int overlappedError = WSAGetLastError();
			if (overlappedError != 0)
				socketErrorCode = overlappedError;
		}
	}
	const bool isUdpPortUnreachable = (pContext->kind == SOCKET_KIND_UDP &&
		socketErrorCode == ERROR_PORT_UNREACHABLE);

	if (isCurrentContext)
	{
		if (isUdpReadContext)
			pState->pendingReadContexts.erase(pContext);
		else
			*ppCurrentContext = NULL;
	}

	// 取消 IO 是正常注销路径，不算网络错误。
	// 这里仍然必须释放 context，因为 Windows 会为被取消的 OVERLAPPED
	// 投递一个完成包回来。
	if (!success && socketErrorCode == ERROR_OPERATION_ABORTED)
	{
		releaseContext(*pContext);

		if (isCurrentContext)
		{
			cleanupStateIfUnused(fd);
		}

		return;
	}

	if (!isCurrentContext)
	{
		// 迟到的 completion：fd/socket/generation/context 任一不匹配都丢弃。
		// 这是 IOCP 下避免“旧连接事件打到新连接”的核心保护。
		releaseContext(*pContext);
		return;
	}

	if (pContext->operation == OP_ACCEPT)
	{
		if (success && pContext->acceptSocket != INVALID_SOCKET)
		{
			setsockopt(pContext->acceptSocket, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
				reinterpret_cast<const char*>(&pContext->socket), sizeof(pContext->socket));

			// pushAcceptedSocket 可能因为 accept 队列满而关闭 acceptSocket。
			// 无论是否成功入队，context 都不再拥有这个 socket，避免 cleanupContext 二次关闭。
			bool queued = pushAcceptedSocket(fd, pContext->acceptSocket);
			pContext->acceptSocket = INVALID_SOCKET;
			if (queued)
			{
				this->triggerRead(fd);
			}
		}
		else if (!success)
		{
			WARNING_MSG(fmt::format("IocpPoller::handleCompletion: AcceptEx failed on fd {}, completionError={}, socketError={}: {}\n",
				fd, errorCode, socketErrorCode, kbe_strerror(socketErrorCode)));
		}
	}
		else if (pContext->operation == OP_TCP_RECV)
		{
			// TCP_RECV completion 不直接调用 recv。
			// 数据先进入 tcpReceived_，再 triggerRead，让 TCPPacketReceiver
			// 通过原有 PacketReader/Channel 逻辑解析消息。
			// bytesTransferred==0 是有序断开，也会入队为零字节 completion；
			// 公共层用 item 上限限制这类空 completion，防止错误/断开风暴。
			if (success && bytesTransferred > 0)
			{
				pContext->data.resize(static_cast<size_t>(bytesTransferred));
			}
			else
			{
				pContext->data.clear();
			}

			if (!success && errorCode != 0)
			{
				WARNING_MSG(fmt::format("IocpPoller::handleCompletion: read completion failed on fd {}, completionError={}, socketError={}: {}\n",
					fd, errorCode, socketErrorCode, kbe_strerror(socketErrorCode)));
			}

			const bool terminal = !success || bytesTransferred == 0;
			if (terminal)
			{
				// IOCP 每个 fd 只挂一个读侧 OVERLAPPED。
				// 一旦这个 completion 表示 EOF/错误，就说明 TCP 读生命周期结束；
				// 这里先关掉内部 registeredRead，防止函数尾部自动 ensureReadArmed 再投递一次
				// WSARecv，造成断开的 socket 反复产生 0 字节 completion。
				pState->registeredRead = false;
			}

			const bool queued = pushTcpReceivedData(fd, pContext->data, success && bytesTransferred == 0, success ? 0 : socketErrorCode);
			if (queued)
			{
				this->triggerRead(fd);
			}
			else if (!terminal)
			{
				// Never discard TCP bytes silently; turn a full handoff queue into a deterministic channel error.
				// TCP 字节不能静默丢弃；交接队列满时转成确定性的 channel 错误。
				if (pushTcpReceivedData(fd, pContext->data, false, WSAENOBUFS))
				{
					this->triggerRead(fd);
				}
			}
		}
	else if (pContext->operation == OP_UDP_RECV)
	{
		if (!success && errorCode != 0 && !isUdpPortUnreachable)
		{
			WARNING_MSG(fmt::format("IocpPoller::handleCompletion: udp recv completion failed on fd {}, completionError={}, socketError={}: {}\n",
				fd, errorCode, socketErrorCode, kbe_strerror(socketErrorCode)));
		}

		if (success && bytesTransferred > 0)
		{
			pContext->data.resize(static_cast<size_t>(bytesTransferred));
			if (pushUdpReceivedData(fd, pContext->data, pContext->udpAddr, 0))
			{
				// BaseApp KCP clients share one UDP listener. Packet dispatch can synchronously
				// execute decryption, Python and Entity handlers for hundreds of milliseconds;
				// leaving no WSARecvFrom posted during that work turns the kernel socket buffer
				// into the only receive queue and causes avoidable loss. Post the next receive
				// after transferring ownership but before dispatching the current datagram.
				// BaseApp 的 KCP 客户端共用一个 UDP listener。当前包分发可能同步执行解密、
				// Python 与 Entity handler 数百毫秒；若期间没有挂起 WSARecvFrom，内核 socket
				// 缓冲就成为唯一接收队列并产生可避免的丢包。数据所有权转移后、分发当前包前，
				// 立即挂入下一次接收。主线程仍按 completion 顺序串行解析，不引入并发 handler。
				if (pState->registeredRead &&
					(!ensureUdpReadsArmed(fd, *pState) || !isReadArmComplete(*pState)))
				{
					requestRearm(fd, REARM_READ);
				}
				this->triggerRead(fd);
			}
		}
		else if (!success && isUdpPortUnreachable)
		{
			// Windows 会把 ICMP port unreachable 转成 UDP recv completion 错误。
			// KCP/UDP 客户端断开时这很常见，而且 completion 里没有可靠的
			// 对端地址可用于关闭具体 channel；交给 KCP 发送窗口/超时逻辑处理，
			// 这里不向上层报告，避免刷 REASON_GENERAL_NETWORK。
		}
	}
	else if (pContext->operation == OP_TCP_SEND)
	{
		if (!success && errorCode != 0)
		{
			WARNING_MSG(fmt::format("IocpPoller::handleCompletion: send completion failed on fd {}, completionError={}, socketError={}: {}\n",
				fd, errorCode, socketErrorCode, kbe_strerror(socketErrorCode)));

			// 发送失败仍然转成读侧错误 completion，让 Channel 销毁路径和旧同步 send
			// 保持一致，避免写侧直接销毁打断 buffered receive 遍历。
			std::vector<char> data;
			if (pushTcpReceivedData(fd, data, false, socketErrorCode))
			{
				this->triggerRead(fd);
			}

			releaseContext(*pContext);
			return;
		}

		if (success && bytesTransferred < pContext->tcpSendData.size())
		{
			// WSASend completion 允许只完成部分字节。
			// 未发送完的数据必须放回队首，保持 TCP 字节流顺序。
			pushTcpSendFront(*pState, pContext->tcpSendData, static_cast<size_t>(bytesTransferred));
			++tcpPartialSendCount_;
		}

		if (!pState->pendingTcpSends.empty())
		{
			if (!armTcpSend(fd, *pState))
			{
				requestRearm(fd, REARM_WRITE);
			}
		}
		else if (this->findForWrite(fd) != NULL)
		{
			// 所有 IOCP 发送都完成后再触发写通知，让 TCPPacketSender
			// 调用 Channel::onSendCompleted()，保持旧的 FLAG_SENDING 生命周期。
			this->triggerWrite(fd);
		}
	}
	else if (pContext->operation == OP_UDP_SEND)
	{
		if (!success && errorCode != 0 && !isUdpPortUnreachable)
		{
			WARNING_MSG(fmt::format("IocpPoller::handleCompletion: udp send completion failed on fd {}, completionError={}, socketError={}: {}\n",
				fd, errorCode, socketErrorCode, kbe_strerror(socketErrorCode)));
		}

		if (!pState->pendingUdpSends.empty())
		{
			if (!armUdpSend(fd, *pState))
			{
				requestRearm(fd, REARM_WRITE);
			}
		}
	}

	releaseContext(*pContext);

	auto currentIter = socketStates_.find(fd);
	if (currentIter != socketStates_.end())
	{
		SocketState& currentState = *currentIter->second;
		if (currentState.registeredRead && !isReadArmComplete(currentState))
		{
			// completion 回调可能销毁 channel 或重新注册 fd。
			// 因此重挂 read 前必须重新查当前 state，而不是继续使用旧指针。
			if (!ensureReadArmed(fd, currentState) || !isReadArmComplete(currentState))
			{
				requestRearm(fd, REARM_READ);
			}
		}
		else if (!currentState.registeredRead)
		{
			cleanupStateIfUnused(fd);
		}
	}
}

//-------------------------------------------------------------------------------------
int IocpPoller::processPendingEvents(double maxWait)
{
	// 初始化失败时不能把空 HANDLE 传给 GetQueuedCompletionStatus；
	// When initialization failed, never pass a null HANDLE to GetQueuedCompletionStatus.
	if (completionPort_ == NULL)
	{
		return 0;
	}

	processRearmRequests();

	DWORD timeoutMs = toTimeoutMilliseconds(maxWait);

#if ENABLE_WATCHERS
	g_iocpIdleProfile.start();
#else
	uint64 startTime = timestamp();
#endif

	DWORD dequeueError = ERROR_SUCCESS;
	if (pendingCompletions_.empty())
	{
		KBEConcurrency::onStartMainThreadIdling();
		dequeueError = dequeueCompletions(timeoutMs);
		KBEConcurrency::onEndMainThreadIdling();
	}

#if ENABLE_WATCHERS
	g_iocpIdleProfile.stop();
	spareTime_ += g_iocpIdleProfile.lastTime_;
#else
	spareTime_ += timestamp() - startTime;
#endif

	int readyCount = 0;
	bool completionTimeBudgetExhausted = false;
	if (!pendingCompletions_.empty())
	{
		const uint64 completionProcessingStart = timestamp();
		// completionBudget 是主线程公平性保护：
		// KBEngine 的网络 completion 会同步进入 PacketReader/Entity 逻辑，
		// 如果一次 tick 无限制 drain IOCP，断线或启动 burst 会把 timer、
		// app 心跳和其他 channel 处理饿住。预算到达后保留剩余 completion
		// 在下一轮 tick 继续取，IOCP 队列本身保证完成事件不会丢。
		const uint32 completionProcessingBudgetMs =
			completionProcessingTimeBudgetMs(completionConsecutiveBudgetExhaustions_);
		const uint64 completionProcessingBudget = completionProcessingBudgetMs > 0 ?
			(uint64(completionProcessingBudgetMs) * stampsPerSecond() / 1000) : 0;

		while (!pendingCompletions_.empty() &&
			(readyCount == 0 || shouldProcessAnotherCompletion(static_cast<uint32>(readyCount),
				timestamp() - completionProcessingStart, completionProcessingBudget)))
		{
			const PendingCompletion pending = pendingCompletions_.front();
			pendingCompletions_.pop_front();
			++readyCount;
			handleCompletion(pending.completionKey, pending.overlapped, pending.bytesTransferred,
				pending.success, pending.errorCode);

			// The first kernel batch may have been fully consumed. Refill only while
			// the current tick still has budget; a zero-timeout dequeue never blocks.
			// 首批内核完成项可能已消费完；仅在本 tick 仍有预算时补批次，零超时不会阻塞。
			if (pendingCompletions_.empty() && shouldProcessAnotherCompletion(static_cast<uint32>(readyCount),
				timestamp() - completionProcessingStart, completionProcessingBudget))
			{
				const DWORD refillError = dequeueCompletions(0);
				if (refillError != ERROR_SUCCESS && refillError != WAIT_TIMEOUT)
				{
					dequeueError = refillError;
					break;
				}
			}
		}

		const uint64 completionProcessingElapsed = timestamp() - completionProcessingStart;
		const bool countBudgetExhausted = readyCount >= static_cast<int>(COMPLETION_MAX_COMPLETIONS_PER_TICK);
		const bool timeBudgetExceeded = Network::completionTimeBudgetExhausted(
			static_cast<uint32>(readyCount), completionProcessingElapsed, completionProcessingBudget);
		completionTimeBudgetExhausted = timeBudgetExceeded;
		const bool timeBudgetWarningExceeded = completionProcessingBudget > 0 &&
			completionProcessingElapsed >= completionProcessingBudget * COMPLETION_BUDGET_WARNING_MULTIPLIER;

		// countBudgetExhausted 只代表本 tick 还有 completion 留到下 tick，
		// 对断线/启动 burst 很常见，所以不单独报警。
		// 只有处理时间达到预算多倍时才 warning，避免把正常登录、登出、
		// getCell 这类上层回调耗时误判为 IOCP 异常。
		if (timeBudgetWarningExceeded)
		{
			uint64 now = timestamp();
			if (lastCompletionBudgetWarningTime_ == 0 ||
				now - lastCompletionBudgetWarningTime_ >= COMPLETION_BUDGET_WARNING_INTERVAL)
			{
				lastCompletionBudgetWarningTime_ = now;
				WARNING_MSG(fmt::format("IocpPoller::processPendingEvents: completion processing took too long, count={}, countBudget={}, timeBudget={}, maxCount={}, maxTimeMS={}, elapsedMS={}\n",
					readyCount, countBudgetExhausted, timeBudgetExceeded,
					COMPLETION_MAX_COMPLETIONS_PER_TICK,
					completionProcessingBudgetMs,
					completionProcessingElapsed * 1000 / stampsPerSecond()));
			}
		}
	}
	if (dequeueError != ERROR_SUCCESS && dequeueError != WAIT_TIMEOUT)
	{
		WARNING_MSG(fmt::format("IocpPoller::processPendingEvents: GetQueuedCompletionStatus failed: {}\n",
			kbe_strerror(dequeueError)));
	}

	recordCompletionBatch(static_cast<uint32>(readyCount),
		readyCount >= static_cast<int>(COMPLETION_MAX_COMPLETIONS_PER_TICK),
		completionTimeBudgetExhausted);

	return readyCount;
}

}
}

#endif // KBE_PLATFORM == PLATFORM_WIN32
