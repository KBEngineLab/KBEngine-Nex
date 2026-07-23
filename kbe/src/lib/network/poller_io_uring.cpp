// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

#include "poller_io_uring.h"

#if defined(__linux__)

#include "helper/profile.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace KBEngine {
namespace
{
ProfileVal g_ioUringIdleProfile("Idle");
}

namespace Network
{
namespace
{
const size_t IO_URING_TCP_SEND_BATCH_BYTES = 64 * 1024;
const uint64 COMPLETION_BUDGET_WARNING_INTERVAL = 10 * stampsPerSecond();
const uint32 COMPLETION_BUDGET_WARNING_MULTIPLIER = 10;

inline int ioUringSetup(uint32 entries, io_uring_params* params)
{
	// 使用 syscall 避免引入 liburing 链接依赖。
	return static_cast<int>(::syscall(__NR_io_uring_setup, entries, params));
}

inline int ioUringEnter(int ringFd, unsigned toSubmit, unsigned minComplete, unsigned flags)
{
	// 只用 io_uring_enter 提交 SQE；等待由 poll(ringFd_) 处理。
	return static_cast<int>(::syscall(__NR_io_uring_enter, ringFd, toSubmit, minComplete, flags, NULL, 0));
}
}

//-------------------------------------------------------------------------------------
IoUringPoller::IoUringContext::IoUringContext(int fdArg, KBESOCKET socketArg, SocketKind kindArg, Operation operationArg, uint64 generationArg) :
	fd(fdArg),
	socket(socketArg),
	kind(kindArg),
	operation(operationArg),
	generation(generationArg),
	data(),
	addr(),
	addrLen(sizeof(addr)),
	iov(),
	msg()
{
	memset(&addr, 0, sizeof(addr));
	memset(&iov, 0, sizeof(iov));
	memset(&msg, 0, sizeof(msg));
	msg.msg_name = &addr;
	msg.msg_namelen = addrLen;
}

//-------------------------------------------------------------------------------------
IoUringPoller::Ring::Ring() :
	sqHead(NULL),
	sqTail(NULL),
	sqRingMask(NULL),
	sqRingEntries(NULL),
	sqFlags(NULL),
	sqDropped(NULL),
	sqArray(NULL),
	sqes(NULL),
	cqHead(NULL),
	cqTail(NULL),
	cqRingMask(NULL),
	cqRingEntries(NULL),
	cqes(NULL),
	sqRingPtr(MAP_FAILED),
	sqRingSize(0),
	cqRingPtr(MAP_FAILED),
	cqRingSize(0),
	sqesPtr(MAP_FAILED),
	sqesSize(0)
{
}

//-------------------------------------------------------------------------------------
IoUringPoller::IoUringPoller(uint32 entries) :
	CompletionPoller(),
	ringFd_(-1),
	ring_(),
	outstandingContexts_(),
	lastCompletionBudgetWarningTime_(0)
{
	if (!setupRing(entries))
	{
		ERROR_MSG(fmt::format("IoUringPoller::IoUringPoller: io_uring setup failed: {}\n",
			kbe_strerror()));
	}
}

//-------------------------------------------------------------------------------------
IoUringPoller::~IoUringPoller()
{
	destroyRing();

	for (IoUringContext* context : outstandingContexts_)
	{
		// ring 销毁后，仍未返回 CQE 的 user_data 不会再交给 handleCompletion。
		// outstandingContexts_ 覆盖了当前请求和注销后等待迟到 CQE 的旧请求，
		// 因此析构时可以一次性回收所有 context/buffer。
		delete context;
	}
	outstandingContexts_.clear();

	for (auto& item : socketStates_)
	{
		item.second->pPendingReadContext = NULL;
		item.second->pPendingWriteContext = NULL;
		item.second->readArmed = false;
		item.second->writeArmed = false;
	}
}

//-------------------------------------------------------------------------------------
int IoUringPoller::toTimeoutMilliseconds(double maxWait)
{
	// poll 使用毫秒超时，负数和零都表示立即返回。
	if (maxWait <= 0.0)
	{
		return 0;
	}

	double milliseconds = std::ceil(maxWait * 1000.0);
	if (milliseconds > static_cast<double>(std::numeric_limits<int>::max()))
	{
		return std::numeric_limits<int>::max();
	}

	return static_cast<int>(milliseconds);
}

//-------------------------------------------------------------------------------------
bool IoUringPoller::setupRing(uint32 entries)
{
	// 初始化 SQ/CQ mmap 指针，后续 SQE/CQE 都直接操作共享 ring。
	io_uring_params params;
	memset(&params, 0, sizeof(params));

	ringFd_ = ioUringSetup(entries, &params);
	if (ringFd_ < 0)
	{
		return false;
	}

	ring_.sqRingSize = params.sq_off.array + params.sq_entries * sizeof(unsigned);
	ring_.cqRingSize = params.cq_off.cqes + params.cq_entries * sizeof(io_uring_cqe);
	ring_.sqesSize = params.sq_entries * sizeof(io_uring_sqe);

	ring_.sqRingPtr = mmap(0, ring_.sqRingSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, ringFd_, IORING_OFF_SQ_RING);
	ring_.cqRingPtr = mmap(0, ring_.cqRingSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, ringFd_, IORING_OFF_CQ_RING);
	ring_.sqesPtr = mmap(0, ring_.sqesSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, ringFd_, IORING_OFF_SQES);

	if (ring_.sqRingPtr == MAP_FAILED || ring_.cqRingPtr == MAP_FAILED || ring_.sqesPtr == MAP_FAILED)
	{
		destroyRing();
		return false;
	}

	char* sqPtr = static_cast<char*>(ring_.sqRingPtr);
	char* cqPtr = static_cast<char*>(ring_.cqRingPtr);
	ring_.sqHead = reinterpret_cast<unsigned*>(sqPtr + params.sq_off.head);
	ring_.sqTail = reinterpret_cast<unsigned*>(sqPtr + params.sq_off.tail);
	ring_.sqRingMask = reinterpret_cast<unsigned*>(sqPtr + params.sq_off.ring_mask);
	ring_.sqRingEntries = reinterpret_cast<unsigned*>(sqPtr + params.sq_off.ring_entries);
	ring_.sqFlags = reinterpret_cast<unsigned*>(sqPtr + params.sq_off.flags);
	ring_.sqDropped = reinterpret_cast<unsigned*>(sqPtr + params.sq_off.dropped);
	ring_.sqArray = reinterpret_cast<unsigned*>(sqPtr + params.sq_off.array);
	ring_.sqes = static_cast<io_uring_sqe*>(ring_.sqesPtr);
	ring_.cqHead = reinterpret_cast<unsigned*>(cqPtr + params.cq_off.head);
	ring_.cqTail = reinterpret_cast<unsigned*>(cqPtr + params.cq_off.tail);
	ring_.cqRingMask = reinterpret_cast<unsigned*>(cqPtr + params.cq_off.ring_mask);
	ring_.cqRingEntries = reinterpret_cast<unsigned*>(cqPtr + params.cq_off.ring_entries);
	ring_.cqes = reinterpret_cast<io_uring_cqe*>(cqPtr + params.cq_off.cqes);
	return true;
}

//-------------------------------------------------------------------------------------
void IoUringPoller::destroyRing()
{
	// 按 mmap 的反向顺序释放 ring 资源。
	if (ring_.sqRingPtr != MAP_FAILED)
	{
		munmap(ring_.sqRingPtr, ring_.sqRingSize);
		ring_.sqRingPtr = MAP_FAILED;
	}

	if (ring_.cqRingPtr != MAP_FAILED)
	{
		munmap(ring_.cqRingPtr, ring_.cqRingSize);
		ring_.cqRingPtr = MAP_FAILED;
	}

	if (ring_.sqesPtr != MAP_FAILED)
	{
		munmap(ring_.sqesPtr, ring_.sqesSize);
		ring_.sqesPtr = MAP_FAILED;
	}

	if (ringFd_ >= 0)
	{
		::close(ringFd_);
		ringFd_ = -1;
	}
}

//-------------------------------------------------------------------------------------
io_uring_sqe* IoUringPoller::getSqe()
{
	// ring 初始化失败时直接拒绝投递，避免访问未映射的共享内存。
	if (ringFd_ < 0 || ring_.sqHead == NULL || ring_.sqTail == NULL || ring_.sqRingEntries == NULL)
	{
		return NULL;
	}

	// SQ ring 满时返回 NULL，让调用方下个 tick 再投递。
	unsigned tail = *ring_.sqTail;
	unsigned head = *ring_.sqHead;
	if (tail - head >= *ring_.sqRingEntries)
	{
		return NULL;
	}

	unsigned index = tail & *ring_.sqRingMask;
	io_uring_sqe* sqe = &ring_.sqes[index];
	memset(sqe, 0, sizeof(*sqe));
	ring_.sqArray[index] = index;
	*ring_.sqTail = tail + 1;
	return sqe;
}

//-------------------------------------------------------------------------------------
bool IoUringPoller::submitSqes()
{
	// 没有可用 ring 时提交必然失败，调用方会保留队列等待后续处理。
	if (ringFd_ < 0 || ring_.sqHead == NULL || ring_.sqTail == NULL)
	{
		return false;
	}

	// 这里只负责提交 SQE；等待 CQE 交给 poll(ringFd_)，避免额外 timeout SQE。
	unsigned toSubmit = *ring_.sqTail - *ring_.sqHead;
	if (toSubmit == 0)
	{
		return true;
	}

	int ret = ioUringEnter(ringFd_, toSubmit, 0, 0);
	return ret >= 0 || errno == EINTR;
}

//-------------------------------------------------------------------------------------
bool IoUringPoller::doRegisterForRead(int fd)
{
	// io_uring 不可用时注册失败，让上层能在启动阶段暴露平台能力问题。
	if (ringFd_ < 0)
	{
		return false;
	}

	// 注册读侧会刷新 generation，迟到的旧 CQE 会被 handleCompletion 丢弃。
	SocketState& state = socketStateForFd(fd);
	state.registeredRead = true;
	state.readArmed = false;
	state.pPendingReadContext = NULL;
	++state.generation;
	clearReceivedData(fd);

	if (!tryDetermineSocketKind(state.socket, state.kind))
	{
		return false;
	}

	return ensureReadArmed(fd, state);
}

//-------------------------------------------------------------------------------------
bool IoUringPoller::queueTcpSend(int fd, const void* data, int len)
{
	// 基类负责有界排队；io_uring 这里额外做一次“就近投递”。
	// 如果 SQ ring 暂时满了，数据仍留在 pendingTcpSends 中，后续 processPendingEvents
	// 会继续补投递，所以不能把“暂时没有 SQE”误报成发送失败。
	if (!CompletionPoller::queueTcpSend(fd, data, len))
	{
		return false;
	}

	if (ringFd_ < 0)
	{
		return false;
	}

	SocketState& state = socketStateForFd(fd);
	if (!state.writeArmed)
	{
		armTcpSend(fd, state);
		submitSqes();
	}

	return true;
}

//-------------------------------------------------------------------------------------
bool IoUringPoller::queueUdpSend(int fd, const void* data, int len, const Address& dstAddr)
{
	// UDP/KCP 的发送路径同样尽量在入队时投递，减少高频小包多等一轮 tick 的尾延迟。
	// SQ 满时保留 pending 队列，由主循环后续重试，保持和原有异步语义一致。
	if (!CompletionPoller::queueUdpSend(fd, data, len, dstAddr))
	{
		return false;
	}

	if (ringFd_ < 0)
	{
		return false;
	}

	SocketState& state = socketStateForFd(fd);
	if (!state.writeArmed)
	{
		armUdpSend(fd, state);
		submitSqes();
	}

	return true;
}

//-------------------------------------------------------------------------------------
bool IoUringPoller::doRegisterForWrite(int fd)
{
	// io_uring 不可用时写注册失败，保持与读侧一致的错误反馈。
	if (ringFd_ < 0)
	{
		return false;
	}

	// 写 handler 保存在 EventPoller，io_uring 不需要 readiness 注册。
	(void)fd;
	return true;
}

//-------------------------------------------------------------------------------------
bool IoUringPoller::doDeregisterForRead(int fd)
{
	// 不强制 cancel：通过 generation 丢弃迟到 CQE，避免 fd 复用误投递。
	auto iter = socketStates_.find(fd);
	if (iter == socketStates_.end())
	{
		return true;
	}

	SocketState& state = *iter->second;
	state.registeredRead = false;
	state.readArmed = false;
	state.pPendingReadContext = NULL;
	++state.generation;
	clearReceivedData(fd);
	cleanupStateIfUnused(fd);
	return true;
}

//-------------------------------------------------------------------------------------
bool IoUringPoller::doDeregisterForWrite(int fd)
{
	// 写注销清空未投递队列，已经在内核里的 CQE 由 generation/状态检查处理。
	auto iter = socketStates_.find(fd);
	if (iter == socketStates_.end())
	{
		return true;
	}

	SocketState& state = *iter->second;
	clearPendingSends(state);
	state.writeArmed = false;
	state.pPendingWriteContext = NULL;
	cleanupStateIfUnused(fd);
	return true;
}

//-------------------------------------------------------------------------------------
bool IoUringPoller::ensureReadArmed(int fd, SocketState& state)
{
	// 每个 fd 同时只挂一个读类请求，保持上层 PacketReceiver 的串行消费语义。
	// io_uring 本身就是 completion 模型，不需要像 kqueue adapter 那样用用户态
	// 队列水位反向暂停 read；完成事件到达后会立即 triggerRead，让上层消费 handoff 队列。
	if (!state.registeredRead || state.readArmed)
	{
		return true;
	}

	if (state.kind == SOCKET_KIND_UNKNOWN && !tryDetermineSocketKind(state.socket, state.kind))
	{
		return false;
	}

	switch (state.kind)
	{
	case SOCKET_KIND_LISTENER:
		return armAccept(fd, state);
	case SOCKET_KIND_UDP:
		return armUdpRead(fd, state);
	case SOCKET_KIND_TCP:
		return armTcpRead(fd, state);
	default:
		return false;
	}
}

//-------------------------------------------------------------------------------------
bool IoUringPoller::armAccept(int fd, SocketState& state)
{
	// IORING_OP_ACCEPT 完成后 listener 只消费 completion 队列。
	io_uring_sqe* sqe = getSqe();
	if (sqe == NULL)
	{
		return false;
	}

	IoUringContext* context = new IoUringContext(fd, state.socket, SOCKET_KIND_LISTENER, OP_ACCEPT, state.generation);
	trackContext(context);
	sqe->opcode = IORING_OP_ACCEPT;
	sqe->fd = fd;
	sqe->addr = 0;
	sqe->off = 0;
	sqe->accept_flags = SOCK_NONBLOCK;
	sqe->user_data = reinterpret_cast<uint64>(context);
	state.pPendingReadContext = context;
	state.readArmed = true;
	return true;
}

//-------------------------------------------------------------------------------------
bool IoUringPoller::armTcpRead(int fd, SocketState& state)
{
	// TCP recv completion 直接把字节流送入 tcpReceived_。
	io_uring_sqe* sqe = getSqe();
	if (sqe == NULL)
	{
		return false;
	}

	IoUringContext* context = new IoUringContext(fd, state.socket, SOCKET_KIND_TCP, OP_TCP_RECV, state.generation);
	trackContext(context);
	context->data.resize(PACKET_MAX_SIZE_TCP);
	sqe->opcode = IORING_OP_RECV;
	sqe->fd = fd;
	sqe->addr = reinterpret_cast<uint64>(context->data.data());
	sqe->len = static_cast<unsigned>(context->data.size());
	sqe->user_data = reinterpret_cast<uint64>(context);
	state.pPendingReadContext = context;
	state.readArmed = true;
	return true;
}

//-------------------------------------------------------------------------------------
bool IoUringPoller::armUdpRead(int fd, SocketState& state)
{
	// UDP 使用 recvmsg，这样 CQE 回来时能同时拿到来源地址。
	io_uring_sqe* sqe = getSqe();
	if (sqe == NULL)
	{
		return false;
	}

	IoUringContext* context = new IoUringContext(fd, state.socket, SOCKET_KIND_UDP, OP_UDP_RECV, state.generation);
	trackContext(context);
	context->data.resize(PACKET_MAX_SIZE_UDP);
	context->iov.iov_base = context->data.data();
	context->iov.iov_len = context->data.size();
	context->msg.msg_iov = &context->iov;
	context->msg.msg_iovlen = 1;
	sqe->opcode = IORING_OP_RECVMSG;
	sqe->fd = fd;
	sqe->addr = reinterpret_cast<uint64>(&context->msg);
	sqe->user_data = reinterpret_cast<uint64>(context);
	state.pPendingReadContext = context;
	state.readArmed = true;
	return true;
}

//-------------------------------------------------------------------------------------
bool IoUringPoller::armTcpSend(int fd, SocketState& state)
{
	// 合并小包后投递一次 send，降低 CQE 数量。
	if (state.writeArmed || state.pendingTcpSends.empty())
	{
		return true;
	}

	io_uring_sqe* sqe = getSqe();
	if (sqe == NULL)
	{
		return false;
	}

	IoUringContext* context = new IoUringContext(fd, state.socket, SOCKET_KIND_TCP, OP_TCP_SEND, state.generation);
	trackContext(context);
	if (!popTcpSendBatch(state, IO_URING_TCP_SEND_BATCH_BYTES, context->data))
	{
		untrackContext(context);
		delete context;
		return true;
	}

	sqe->opcode = IORING_OP_SEND;
	sqe->fd = fd;
	sqe->addr = reinterpret_cast<uint64>(context->data.data());
	sqe->len = static_cast<unsigned>(context->data.size());
	sqe->user_data = reinterpret_cast<uint64>(context);
	state.pPendingWriteContext = context;
	state.writeArmed = true;
	return true;
}

//-------------------------------------------------------------------------------------
bool IoUringPoller::armUdpSend(int fd, SocketState& state)
{
	// UDP 使用 sendmsg，避免上层直接 sendto。
	if (state.writeArmed || state.pendingUdpSends.empty())
	{
		return true;
	}

	io_uring_sqe* sqe = getSqe();
	if (sqe == NULL)
	{
		return false;
	}

	PendingUdpSend pending = std::move(state.pendingUdpSends.front());
	state.pendingUdpSends.pop_front();
	state.pendingUdpSendBytes -= pending.data.size();

	IoUringContext* context = new IoUringContext(fd, state.socket, SOCKET_KIND_UDP, OP_UDP_SEND, state.generation);
	trackContext(context);
	context->data.swap(pending.data);
	context->addr = pending.dstAddr;
	context->addrLen = sizeof(context->addr);
	context->iov.iov_base = context->data.data();
	context->iov.iov_len = context->data.size();
	context->msg.msg_name = &context->addr;
	context->msg.msg_namelen = context->addrLen;
	context->msg.msg_iov = &context->iov;
	context->msg.msg_iovlen = 1;
	sqe->opcode = IORING_OP_SENDMSG;
	sqe->fd = fd;
	sqe->addr = reinterpret_cast<uint64>(&context->msg);
	sqe->user_data = reinterpret_cast<uint64>(context);
	state.pPendingWriteContext = context;
	state.writeArmed = true;
	return true;
}

//-------------------------------------------------------------------------------------
void IoUringPoller::handleCompletion(IoUringContext& context, int result)
{
	// 先校验 fd/generation，防止旧 CQE 打到复用后的新连接。
	const int fd = context.fd;
	auto iter = socketStates_.find(fd);
	SocketState* state = iter != socketStates_.end() ? iter->second.get() : NULL;
	void** ppCurrentContext = NULL;
	if (state != NULL)
	{
		ppCurrentContext = (context.operation == OP_TCP_SEND || context.operation == OP_UDP_SEND) ?
			&state->pPendingWriteContext : &state->pPendingReadContext;
	}

	const bool isCurrent = state != NULL &&
		state->socket == context.socket &&
		state->generation == context.generation &&
		ppCurrentContext != NULL &&
		*ppCurrentContext == &context;

	if (!isCurrent)
	{
		// 注销或 fd 生命周期重置后，generation 会先递增；旧 CQE 回来时虽然不能再投递给上层，
		// 但如果 SocketState 仍保存着这个旧 context 指针，必须在这里解除引用。
		// 这样 cleanupStateIfUnused 才能在最后一个迟到 CQE 被丢弃后释放 fd 状态。
		if (state != NULL && ppCurrentContext != NULL && *ppCurrentContext == &context)
		{
			*ppCurrentContext = NULL;
			if (context.operation == OP_ACCEPT || context.operation == OP_TCP_RECV || context.operation == OP_UDP_RECV)
			{
				state->readArmed = false;
			}
			else
			{
				state->writeArmed = false;
			}
			cleanupStateIfUnused(fd);
		}

		untrackContext(&context);
		delete &context;
		return;
	}

	const int errorCode = result < 0 ? -result : 0;
	if (context.operation == OP_ACCEPT || context.operation == OP_TCP_RECV || context.operation == OP_UDP_RECV)
	{
		*ppCurrentContext = NULL;
		state->readArmed = false;
	}
	else
	{
		*ppCurrentContext = NULL;
		state->writeArmed = false;
	}

	if (context.operation == OP_ACCEPT)
	{
		if (result >= 0)
		{
			// pushAcceptedSocket 可能因为用户态 accept 队列满而关闭 accepted fd。
			// 只有真正入队后才 triggerRead，否则 listener handler 会被空唤醒。
			if (pushAcceptedSocket(fd, static_cast<KBESOCKET>(result)))
			{
				this->triggerRead(fd);
			}
		}
		else if (errorCode != EAGAIN && errorCode != ECONNABORTED)
		{
			WARNING_MSG(fmt::format("IoUringPoller::handleCompletion: accept failed on fd {}: {}\n",
				fd, kbe_strerror(errorCode)));
		}
	}
	else if (context.operation == OP_TCP_RECV)
	{
		// result==0 表示对端有序关闭，也要作为一个零字节 completion 入队。
		// 零字节 completion 不增加 pending bytes，因此公共层还会用 item 数限制它。
		std::vector<char> data;
		if (result > 0)
		{
			data.assign(context.data.begin(), context.data.begin() + result);
		}

		if (result < 0 && errorCode == EAGAIN)
		{
			// io_uring 对非阻塞 socket 的 recv 可能以 -EAGAIN 完成。
			// 这不是连接错误，也不应该进入 TCPPacketReceiver 的错误路径；
			// 让函数尾部重新挂一个 recv 即可，保持 completion 模型的串行读语义。
		}
		else
		{
			const bool terminal = result <= 0;
			if (terminal)
			{
				// EOF/真实错误表示 TCP 读生命周期结束。
				// 停掉内部 registeredRead，避免尾部自动 ensureReadArmed 又提交 IORING_OP_RECV，
				// 从而在断开的 socket 上持续收到 0 字节/错误 CQE。
				state->registeredRead = false;
			}

			if (pushTcpReceivedData(fd, data, result == 0, errorCode))
			{
				this->triggerRead(fd);
			}
		}
	}
	else if (context.operation == OP_UDP_RECV)
	{
		if (result > 0)
		{
			std::vector<char> data(context.data.begin(), context.data.begin() + result);
			if (pushUdpReceivedData(fd, data, context.addr, 0))
			{
				this->triggerRead(fd);
			}
		}
		else if (errorCode != 0 && errorCode != ECONNREFUSED && errorCode != EHOSTUNREACH)
		{
			WARNING_MSG(fmt::format("IoUringPoller::handleCompletion: udp recv failed on fd {}: {}\n",
				fd, kbe_strerror(errorCode)));
		}
	}
	else if (context.operation == OP_TCP_SEND)
	{
		if (result < 0)
		{
			// 发送失败沿用读侧错误通知，统一走 TCPPacketReceiver 的关闭/错误处理。
			// triggerRead 可能同步销毁 Channel 并注销 fd；错误投递完成后不能再继续访问
			// 当前 state 的发送队列，因此这里和 IOCP 一样直接结束本次 completion。
			std::vector<char> data;
			if (pushTcpReceivedData(fd, data, false, errorCode))
			{
				this->triggerRead(fd);
			}

			untrackContext(&context);
			delete &context;

			auto currentIter = socketStates_.find(fd);
			if (currentIter != socketStates_.end() && !currentIter->second->registeredRead)
			{
				cleanupStateIfUnused(fd);
			}
			return;
		}
		else if (static_cast<size_t>(result) < context.data.size())
		{
			std::vector<char> remaining(context.data.begin() + result, context.data.end());
			pushTcpSendFront(*state, remaining);
		}

		if (!state->pendingTcpSends.empty())
		{
			armTcpSend(fd, *state);
		}
		else if (this->findForWrite(fd) != NULL)
		{
			this->triggerWrite(fd);
		}
	}
	else if (context.operation == OP_UDP_SEND)
	{
		if (result < 0 && errorCode != ECONNREFUSED && errorCode != EHOSTUNREACH)
		{
			WARNING_MSG(fmt::format("IoUringPoller::handleCompletion: udp send failed on fd {}: {}\n",
				fd, kbe_strerror(errorCode)));
		}

		if (!state->pendingUdpSends.empty())
		{
			armUdpSend(fd, *state);
		}
	}

	untrackContext(&context);
	delete &context;

	auto currentIter = socketStates_.find(fd);
	if (currentIter != socketStates_.end())
	{
		SocketState& currentState = *currentIter->second;
		if (currentState.registeredRead && !currentState.readArmed)
		{
			ensureReadArmed(fd, currentState);
		}
		else if (!currentState.registeredRead)
		{
			cleanupStateIfUnused(fd);
		}
	}
}

//-------------------------------------------------------------------------------------
void IoUringPoller::trackContext(IoUringContext* context)
{
	// 每个 SQE 的 user_data 都拥有一个 heap context。
	// 正常路径由 CQE 到达后删除；如果 poller 析构时 CQE 还没有回来，
	// outstandingContexts_ 就是最后的兜底所有权列表。
	if (context != NULL)
	{
		outstandingContexts_.insert(context);
	}
}

//-------------------------------------------------------------------------------------
void IoUringPoller::untrackContext(IoUringContext* context)
{
	// CQE 已经回到用户态并即将 delete context 时，从兜底集合移除。
	// erase(NULL) 没有意义，这里显式判断能避免把异常路径写得晦涩。
	if (context != NULL)
	{
		outstandingContexts_.erase(context);
	}
}

//-------------------------------------------------------------------------------------
int IoUringPoller::processPendingEvents(double maxWait)
{
	// ring 未建立时不能进入 poll/CQ 处理，返回 0 表示本轮没有完成事件。
	if (ringFd_ < 0 || ring_.cqHead == NULL || ring_.cqTail == NULL)
	{
		return 0;
	}

	// 先把未挂起的读写请求补投递，再进入内核等待 CQE。
	for (auto& item : socketStates_)
	{
		SocketState& state = *item.second;
		if (state.registeredRead && !state.readArmed)
		{
			ensureReadArmed(item.first, state);
		}

		if (!state.pendingTcpSends.empty() && !state.writeArmed)
		{
			armTcpSend(item.first, state);
		}

		if (!state.pendingUdpSends.empty() && !state.writeArmed)
		{
			armUdpSend(item.first, state);
		}
	}

	int timeoutMs = toTimeoutMilliseconds(maxWait);

#if ENABLE_WATCHERS
	g_ioUringIdleProfile.start();
#else
	uint64 startTime = timestamp();
#endif

	KBEConcurrency::onStartMainThreadIdling();
	submitSqes();
	if (*ring_.cqHead == *ring_.cqTail && timeoutMs > 0)
	{
		pollfd pfd;
		memset(&pfd, 0, sizeof(pfd));
		pfd.fd = ringFd_;
		pfd.events = POLLIN;
		::poll(&pfd, 1, timeoutMs);
	}
	KBEConcurrency::onEndMainThreadIdling();

#if ENABLE_WATCHERS
	g_ioUringIdleProfile.stop();
	spareTime_ += g_ioUringIdleProfile.lastTime_;
#else
	spareTime_ += timestamp() - startTime;
#endif

	int readyCount = 0;
	const uint64 completionProcessingStart = timestamp();
	const uint64 completionProcessingBudget =
		g_maxCompletionProcessingTimeMS > 0 ?
		(uint64(g_maxCompletionProcessingTimeMS) * stampsPerSecond() / 1000) : 0;

	while (readyCount < static_cast<int>(g_maxCompletionsPerTick) &&
		(completionProcessingBudget == 0 || timestamp() - completionProcessingStart < completionProcessingBudget))
	{
		unsigned head = *ring_.cqHead;
		if (head == *ring_.cqTail)
		{
			break;
		}

		io_uring_cqe* cqe = &ring_.cqes[head & *ring_.cqRingMask];
		IoUringContext* context = reinterpret_cast<IoUringContext*>(cqe->user_data);
		int result = cqe->res;
		*ring_.cqHead = head + 1;

		if (context != NULL)
		{
			++readyCount;
			handleCompletion(*context, result);
		}
	}

	const uint64 completionProcessingElapsed = timestamp() - completionProcessingStart;
	const bool timeBudgetWarningExceeded = completionProcessingBudget > 0 &&
		completionProcessingElapsed >= completionProcessingBudget * COMPLETION_BUDGET_WARNING_MULTIPLIER;
	if (timeBudgetWarningExceeded)
	{
		uint64 now = timestamp();
		if (lastCompletionBudgetWarningTime_ == 0 ||
			now - lastCompletionBudgetWarningTime_ >= COMPLETION_BUDGET_WARNING_INTERVAL)
		{
			lastCompletionBudgetWarningTime_ = now;
			WARNING_MSG(fmt::format("IoUringPoller::processPendingEvents: completion processing took too long, count={}, maxCount={}, maxTimeMS={}, elapsedMS={}\n",
				readyCount, g_maxCompletionsPerTick, g_maxCompletionProcessingTimeMS,
				completionProcessingElapsed * 1000 / stampsPerSecond()));
		}
	}

	return readyCount;
}

}
}

#endif // defined(__linux__)
