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
const size_t COMPLETION_CONTEXT_RETAINED_BUFFER_BYTES = 64 * 1024;
const uint64 COMPLETION_BUDGET_WARNING_INTERVAL = 10 * stampsPerSecond();
const uint32 COMPLETION_BUDGET_WARNING_MULTIPLIER = 10;

// io_uring 的 head/tail 位于内核共享映射中，普通指针访问不足以建立 SQE/CQE 的发布顺序。
// io_uring head/tail values live in a kernel-shared mapping, so plain pointer access cannot publish SQEs/CQEs with the required ordering.
inline unsigned loadAcquire(const unsigned* value)
{
	return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

inline unsigned loadRelaxed(const unsigned* value)
{
	return __atomic_load_n(value, __ATOMIC_RELAXED);
}

inline void storeRelease(unsigned* target, unsigned value)
{
	__atomic_store_n(target, value, __ATOMIC_RELEASE);
}

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
IoUringPoller::IoUringContext::IoUringContext() :
	fd(-1),
	socket(-1),
	kind(SOCKET_KIND_UNKNOWN),
	operation(OP_ACCEPT),
	generation(0),
	data(),
	tcpSendData(),
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
void IoUringPoller::IoUringContext::reset(KBESOCKET fdArg, KBESOCKET socketArg, SocketKind kindArg, Operation operationArg, uint64 generationArg)
{
	// Rebuild every pointer-bearing kernel structure after buffers may have moved or changed capacity.
	// 缓冲可能移动或改变容量，因此复用时必须重建所有包含指针的内核结构。
	fd = fdArg;
	socket = socketArg;
	kind = kindArg;
	operation = operationArg;
	generation = generationArg;
	data.clear();
	if (data.capacity() > COMPLETION_CONTEXT_RETAINED_BUFFER_BYTES)
	{
		std::vector<char>().swap(data);
	}
	tcpSendData.reset(COMPLETION_CONTEXT_RETAINED_BUFFER_BYTES);
	memset(&addr, 0, sizeof(addr));
	addrLen = sizeof(addr);
	memset(&iov, 0, sizeof(iov));
	memset(&msg, 0, sizeof(msg));
	msg.msg_name = &addr;
	msg.msg_namelen = addrLen;
}

size_t IoUringPoller::IoUringContext::retainedBytes() const
{
	return data.capacity() + tcpSendData.capacity();
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
	sqeHead(0),
	sqeTail(0),
	cqHead(NULL),
	cqTail(NULL),
	cqRingMask(NULL),
	cqRingEntries(NULL),
	cqOverflow(NULL),
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
	contextPool_(entries),
	lastCompletionBudgetWarningTime_(0),
	lastSqDropped_(0),
	lastCqOverflow_(0)
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
		contextPool_.discard(context);
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
uint64 IoUringPoller::contextAllocationCount() const { return contextPool_.allocationCount(); }
uint64 IoUringPoller::contextReuseCount() const { return contextPool_.reuseCount(); }
uint64 IoUringPoller::contextOutstandingCount() const { return contextPool_.outstandingCount(); }
uint64 IoUringPoller::contextCachedCount() const { return contextPool_.cachedCount(); }
uint64 IoUringPoller::contextPeakOutstandingCount() const { return contextPool_.peakOutstandingCount(); }
uint64 IoUringPoller::contextOutstandingBytes() const
{
	size_t bytes = 0;
	for (const IoUringContext* context : outstandingContexts_)
	{
		if (context != NULL)
		{
			bytes += context->retainedBytes();
		}
	}
	return static_cast<uint64>(bytes);
}
uint64 IoUringPoller::contextCachedBytes() const { return static_cast<uint64>(contextPool_.cachedBytes()); }

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

	// 没有 NODROP 时，CQ 满会永久丢失 CQE，使对应 context、buffer 和 fd 生命周期无法收敛。
	// Without NODROP, a full CQ can permanently lose CQEs and strand their contexts, buffers, and descriptor lifetimes.
	if ((params.features & IORING_FEAT_NODROP) == 0)
	{
		errno = ENOTSUP;
		destroyRing();
		return false;
	}

	ring_.sqRingSize = params.sq_off.array + params.sq_entries * sizeof(unsigned);
	ring_.cqRingSize = params.cq_off.cqes + params.cq_entries * sizeof(io_uring_cqe);
	ring_.sqesSize = params.sq_entries * sizeof(io_uring_sqe);

	if ((params.features & IORING_FEAT_SINGLE_MMAP) != 0)
	{
		// SINGLE_MMAP 要求 SQ/CQ 共享一次映射，大小必须覆盖两套 ring 元数据。
		// SINGLE_MMAP requires SQ and CQ to share one mapping sized for both sets of ring metadata.
		const size_t sharedRingSize = std::max(ring_.sqRingSize, ring_.cqRingSize);
		ring_.sqRingSize = sharedRingSize;
		ring_.cqRingSize = sharedRingSize;
		ring_.sqRingPtr = mmap(0, sharedRingSize, PROT_READ | PROT_WRITE,
			MAP_SHARED | MAP_POPULATE, ringFd_, IORING_OFF_SQ_RING);
		ring_.cqRingPtr = ring_.sqRingPtr;
	}
	else
	{
		ring_.sqRingPtr = mmap(0, ring_.sqRingSize, PROT_READ | PROT_WRITE,
			MAP_SHARED | MAP_POPULATE, ringFd_, IORING_OFF_SQ_RING);
		ring_.cqRingPtr = mmap(0, ring_.cqRingSize, PROT_READ | PROT_WRITE,
			MAP_SHARED | MAP_POPULATE, ringFd_, IORING_OFF_CQ_RING);
	}

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
	ring_.cqOverflow = reinterpret_cast<unsigned*>(cqPtr + params.cq_off.overflow);
	ring_.cqes = reinterpret_cast<io_uring_cqe*>(cqPtr + params.cq_off.cqes);
	lastSqDropped_ = loadRelaxed(ring_.sqDropped);
	lastCqOverflow_ = loadRelaxed(ring_.cqOverflow);
	return true;
}

//-------------------------------------------------------------------------------------
void IoUringPoller::destroyRing()
{
	// SINGLE_MMAP 下 SQ/CQ 指针相同，只能解除一次映射；独立映射则按反向顺序分别释放。
	// SQ and CQ share one pointer under SINGLE_MMAP and must be unmapped once; separate mappings are released independently in reverse order.
	void* sqRingPtr = ring_.sqRingPtr;
	void* cqRingPtr = ring_.cqRingPtr;
	if (cqRingPtr != MAP_FAILED && cqRingPtr != sqRingPtr)
	{
		munmap(cqRingPtr, ring_.cqRingSize);
	}

	if (sqRingPtr != MAP_FAILED)
	{
		munmap(sqRingPtr, ring_.sqRingSize);
	}

	ring_.sqRingPtr = MAP_FAILED;
	ring_.cqRingPtr = MAP_FAILED;

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

	// 本地 head 包含尚未发布的 SQE；与 acquire 读取的内核 head 比较可同时约束已发布和待发布条目。
	// The local head includes unpublished SQEs; comparing it with the kernel head loaded with acquire bounds both published and pending entries.
	const unsigned head = ring_.sqeHead;
	const unsigned kernelHead = loadAcquire(ring_.sqHead);
	if (head - kernelHead >= loadRelaxed(ring_.sqRingEntries))
	{
		return NULL;
	}

	const unsigned index = head & loadRelaxed(ring_.sqRingMask);
	io_uring_sqe* sqe = &ring_.sqes[index];
	memset(sqe, 0, sizeof(*sqe));
	ring_.sqeHead = head + 1;
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

	// 先把完整 SQE 的索引写入 submission array，再用 release store 一次性发布 tail。
	// Write indexes for fully initialized SQEs into the submission array, then publish the tail once with a release store.
	unsigned kernelTail = loadRelaxed(ring_.sqTail);
	const unsigned ringMask = loadRelaxed(ring_.sqRingMask);
	while (ring_.sqeTail != ring_.sqeHead)
	{
		const unsigned sqeIndex = ring_.sqeTail & ringMask;
		ring_.sqArray[kernelTail & ringMask] = sqeIndex;
		++kernelTail;
		++ring_.sqeTail;
	}
	storeRelease(ring_.sqTail, kernelTail);

	// 已发布但尚未被内核消费的 SQE 也需要在重试时再次调用 io_uring_enter。
	// Published SQEs not yet consumed by the kernel must be included when io_uring_enter is retried.
	const unsigned toSubmit = kernelTail - loadAcquire(ring_.sqHead);
	if (toSubmit == 0)
	{
		return true;
	}

	int ret = ioUringEnter(ringFd_, toSubmit, 0, 0);
	return ret >= 0 || errno == EINTR;
}

//-------------------------------------------------------------------------------------
bool IoUringPoller::doRegisterForRead(KBESOCKET fd)
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
bool IoUringPoller::queueTcpSend(KBESOCKET fd, const void* data, int len)
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
		if (!armTcpSend(fd, state))
		{
			requestRearm(fd, REARM_WRITE);
		}
		submitSqes();
	}

	return true;
}

//-------------------------------------------------------------------------------------
bool IoUringPoller::queueUdpSend(KBESOCKET fd, const void* data, int len, const Address& dstAddr)
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
		if (!armUdpSend(fd, state))
		{
			requestRearm(fd, REARM_WRITE);
		}
		submitSqes();
	}

	return true;
}

//-------------------------------------------------------------------------------------
bool IoUringPoller::doRegisterForWrite(KBESOCKET fd)
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
bool IoUringPoller::doDeregisterForRead(KBESOCKET fd)
{
	// 不强制 cancel：通过 generation 丢弃迟到 CQE，避免 fd 复用误投递。
	auto iter = socketStates_.find(fd);
	if (iter == socketStates_.end())
	{
		return true;
	}

	SocketState& state = *iter->second;
	state.registeredRead = false;
	cancelRearm(fd, REARM_READ);
	state.readArmed = false;
	state.pPendingReadContext = NULL;
	++state.generation;
	clearReceivedData(fd);
	cleanupStateIfUnused(fd);
	return true;
}

//-------------------------------------------------------------------------------------
bool IoUringPoller::doDeregisterForWrite(KBESOCKET fd)
{
	// 写注销清空未投递队列，已经在内核里的 CQE 由 generation/状态检查处理。
	auto iter = socketStates_.find(fd);
	if (iter == socketStates_.end())
	{
		return true;
	}

	SocketState& state = *iter->second;
	clearPendingSends(state);
	cancelRearm(fd, REARM_WRITE);
	state.writeArmed = false;
	state.pPendingWriteContext = NULL;
	cleanupStateIfUnused(fd);
	return true;
}

//-------------------------------------------------------------------------------------
bool IoUringPoller::ensureReadArmed(KBESOCKET fd, SocketState& state)
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
void IoUringPoller::processRearmRequests()
{
	const size_t requestCount = rearmBatchSize();
	for (size_t i = 0; i < requestCount; ++i)
	{
		KBESOCKET fd = -1;
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
		if ((flags & REARM_READ) != 0 && state.registeredRead && !state.readArmed)
		{
			const bool armed = ensureReadArmed(fd, state) && state.readArmed;
			recordRearmAttempt(!armed);
			if (!armed && state.registeredRead)
			{
				requestRearm(fd, REARM_READ);
			}
		}

		if ((flags & REARM_WRITE) != 0 && !state.writeArmed)
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
bool IoUringPoller::armAccept(KBESOCKET fd, SocketState& state)
{
	// IORING_OP_ACCEPT 完成后 listener 只消费 completion 队列。
	io_uring_sqe* sqe = getSqe();
	if (sqe == NULL)
	{
		return false;
	}

	IoUringContext* context = acquireContext(fd, state.socket, SOCKET_KIND_LISTENER, OP_ACCEPT, state.generation);
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
bool IoUringPoller::armTcpRead(KBESOCKET fd, SocketState& state)
{
	// TCP recv completion 直接把字节流送入 tcpReceived_。
	io_uring_sqe* sqe = getSqe();
	if (sqe == NULL)
	{
		return false;
	}

	IoUringContext* context = acquireContext(fd, state.socket, SOCKET_KIND_TCP, OP_TCP_RECV, state.generation);
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
bool IoUringPoller::armUdpRead(KBESOCKET fd, SocketState& state)
{
	// UDP 使用 recvmsg，这样 CQE 回来时能同时拿到来源地址。
	io_uring_sqe* sqe = getSqe();
	if (sqe == NULL)
	{
		return false;
	}

	IoUringContext* context = acquireContext(fd, state.socket, SOCKET_KIND_UDP, OP_UDP_RECV, state.generation);
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
bool IoUringPoller::armTcpSend(KBESOCKET fd, SocketState& state)
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

	IoUringContext* context = acquireContext(fd, state.socket, SOCKET_KIND_TCP, OP_TCP_SEND, state.generation);
	trackContext(context);
	bool copied = false;
	if (!popTcpSendBatch(state, IO_URING_TCP_SEND_BATCH_BYTES, context->tcpSendData, copied))
	{
		untrackContext(context);
		recycleContext(context);
		return true;
	}
	(void)copied;

	sqe->opcode = IORING_OP_SEND;
	sqe->fd = fd;
	sqe->addr = reinterpret_cast<uint64>(context->tcpSendData.data());
	sqe->len = static_cast<unsigned>(context->tcpSendData.size());
	sqe->user_data = reinterpret_cast<uint64>(context);
	state.pPendingWriteContext = context;
	state.writeArmed = true;
	return true;
}

//-------------------------------------------------------------------------------------
bool IoUringPoller::armUdpSend(KBESOCKET fd, SocketState& state)
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

	PendingUdpSend pending;
	dequeueUdpSend(state, pending);

	IoUringContext* context = acquireContext(fd, state.socket, SOCKET_KIND_UDP, OP_UDP_SEND, state.generation);
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
	const KBESOCKET fd = context.fd;
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
		recycleContext(&context);
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
		if (result > 0)
		{
			context.data.resize(static_cast<size_t>(result));
		}
		else
		{
			context.data.clear();
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

			if (pushTcpReceivedData(fd, context.data, result == 0, errorCode))
			{
				this->triggerRead(fd);
			}
		}
	}
	else if (context.operation == OP_UDP_RECV)
	{
		if (result > 0)
		{
			context.data.resize(static_cast<size_t>(result));
			if (pushUdpReceivedData(fd, context.data, context.addr, 0))
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
			recycleContext(&context);

			auto currentIter = socketStates_.find(fd);
			if (currentIter != socketStates_.end() && !currentIter->second->registeredRead)
			{
				cleanupStateIfUnused(fd);
			}
			return;
		}
		else if (static_cast<size_t>(result) < context.tcpSendData.size())
		{
			pushTcpSendFront(*state, context.tcpSendData, static_cast<size_t>(result));
			++tcpPartialSendCount_;
		}

		if (!state->pendingTcpSends.empty())
		{
			if (!armTcpSend(fd, *state))
			{
				requestRearm(fd, REARM_WRITE);
			}
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
			if (!armUdpSend(fd, *state))
			{
				requestRearm(fd, REARM_WRITE);
			}
		}
	}

	untrackContext(&context);
	recycleContext(&context);

	auto currentIter = socketStates_.find(fd);
	if (currentIter != socketStates_.end())
	{
		SocketState& currentState = *currentIter->second;
		if (currentState.registeredRead && !currentState.readArmed)
		{
			if (!ensureReadArmed(fd, currentState))
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
	// CQE 已经回到用户态并即将回收 context 时，从兜底集合移除。
	// Remove the context from the fallback set after its CQE reaches user space and before recycling it.
	// erase(NULL) 没有意义，这里显式判断能避免把异常路径写得晦涩。
	if (context != NULL)
	{
		outstandingContexts_.erase(context);
	}
}

//-------------------------------------------------------------------------------------
IoUringPoller::IoUringContext* IoUringPoller::acquireContext(KBESOCKET fd, KBESOCKET socket, SocketKind kind, Operation operation, uint64 generation)
{
	IoUringContext* context = contextPool_.acquire();
	context->reset(fd, socket, kind, operation, generation);
	return context;
}

//-------------------------------------------------------------------------------------
void IoUringPoller::recycleContext(IoUringContext* context)
{
	// The caller must consume the CQE and untrack user_data before making this storage reusable.
	// 调用方必须先消费 CQE 并移除 user_data 跟踪，之后这块存储才可复用。
	contextPool_.release(context);
}

//-------------------------------------------------------------------------------------
int IoUringPoller::processPendingEvents(double maxWait)
{
	// ring 未建立时不能进入 poll/CQ 处理，返回 0 表示本轮没有完成事件。
	if (ringFd_ < 0 || ring_.cqHead == NULL || ring_.cqTail == NULL)
	{
		return 0;
	}

	// 只处理上一轮明确登记的失败项；普通空闲连接不再参与主循环维护。
	// Process only failures explicitly queued by the previous round; ordinary idle sockets leave the main loop untouched.
	processRearmRequests();

	int timeoutMs = toTimeoutMilliseconds(maxWait);

#if ENABLE_WATCHERS
	g_ioUringIdleProfile.start();
#else
	uint64 startTime = timestamp();
#endif

	KBEConcurrency::onStartMainThreadIdling();
	submitSqes();
	if (loadRelaxed(ring_.cqHead) == loadAcquire(ring_.cqTail) && timeoutMs > 0)
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
		COMPLETION_MAX_PROCESSING_TIME_MS > 0 ?
		(uint64(COMPLETION_MAX_PROCESSING_TIME_MS) * stampsPerSecond() / 1000) : 0;

	while (readyCount < static_cast<int>(COMPLETION_MAX_COMPLETIONS_PER_TICK) &&
		(completionProcessingBudget == 0 || timestamp() - completionProcessingStart < completionProcessingBudget))
	{
		const unsigned head = loadRelaxed(ring_.cqHead);
		if (head == loadAcquire(ring_.cqTail))
		{
			break;
		}

		io_uring_cqe* cqe = &ring_.cqes[head & loadRelaxed(ring_.cqRingMask)];
		IoUringContext* context = reinterpret_cast<IoUringContext*>(cqe->user_data);
		int result = cqe->res;
		storeRelease(ring_.cqHead, head + 1);

		if (context != NULL)
		{
			++readyCount;
			handleCompletion(*context, result);
		}
	}

	// NODROP 会保留 CQ 满时的 completion，但 overflow 增长仍表示单 tick 消费预算不足；SQ dropped 则表示提交条目无效。
	// NODROP preserves completions while the CQ is full, but overflow growth still signals an insufficient per-tick budget; SQ drops indicate invalid submissions.
	const unsigned sqDropped = loadAcquire(ring_.sqDropped);
	if (sqDropped != lastSqDropped_)
	{
		ERROR_MSG(fmt::format("IoUringPoller::processPendingEvents: submission queue dropped {} entries, total={}.\n",
			sqDropped - lastSqDropped_, sqDropped));
		lastSqDropped_ = sqDropped;
	}

	const unsigned cqOverflow = loadAcquire(ring_.cqOverflow);
	if (cqOverflow != lastCqOverflow_)
	{
		WARNING_MSG(fmt::format("IoUringPoller::processPendingEvents: completion queue overflowed by {} entries, total={}; NODROP preserved delivery.\n",
			cqOverflow - lastCqOverflow_, cqOverflow));
		lastCqOverflow_ = cqOverflow;
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
				readyCount, COMPLETION_MAX_COMPLETIONS_PER_TICK, COMPLETION_MAX_PROCESSING_TIME_MS,
				completionProcessingElapsed * 1000 / stampsPerSecond()));
		}
	}

	recordCompletionBatch(static_cast<uint32>(readyCount),
		readyCount >= static_cast<int>(COMPLETION_MAX_COMPLETIONS_PER_TICK));

	return readyCount;
}

}
}

#endif // defined(__linux__)
