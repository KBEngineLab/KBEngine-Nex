// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

#include "poller_io_uring.h"

#if defined(__linux__)

#include "helper/profile.h"
#include <algorithm>
#include <array>
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
// 1 MiB keeps each context bounded while amortizing CQE and copy cost for dense internal traffic.
// 1 MiB 在限制单个 context 内存的同时，摊薄高密度内部流量的 CQE 与合批复制开销。
const size_t IO_URING_TCP_SEND_BATCH_BYTES = 1024 * 1024;
// Only inspect CQEs from the send hot path after one legacy-sized packet is queued.
// 仅当积压达到一个传统最大包大小后，才从发送热路径探测 CQE，避免小包增加共享队列访问。
const size_t IO_URING_TCP_SEND_PROGRESS_BYTES = 64 * 1024;
// Completion fairness is enforced between CQEs, so one receive must remain one bounded
// protocol packet. Larger buffers can synchronously execute many Entity callbacks before
// the time budget is observed and let staged KCP notifications grow without bound.
// completion 公平性只能在 CQE 之间让步，因此单次接收保持为一个有界协议包；更大的缓冲会在
// 预算检查前同步执行多个 Entity 回调，并让已搬运的 KCP 通知持续积压。
const size_t IO_URING_TCP_RECEIVE_BYTES = PACKET_MAX_SIZE_TCP;
const unsigned IO_URING_DEQUEUE_BATCH_SIZE = 128;
const uint32 IO_URING_NON_UDP_PRIORITY_BURST_SIZE = 8;
const uint32 IO_URING_TCP_SEND_PRIORITY_BURST_SIZE = 4;
const uint64 IO_URING_CONTROL_USER_DATA = uint64(1) << 63;
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

inline int ioUringRegister(int ringFd, unsigned opcode, const void* arg, unsigned nrArgs)
{
	return static_cast<int>(::syscall(__NR_io_uring_register, ringFd, opcode, arg, nrArgs));
}
}

//-------------------------------------------------------------------------------------
IoUringPoller::IoUringContext::IoUringContext() :
	fd(-1),
	socket(-1),
	kind(SOCKET_KIND_UNKNOWN),
	operation(OP_ACCEPT),
	generation(0),
	requestId(0),
	expectedLateCompletion(false),
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
void IoUringPoller::IoUringContext::reset(KBESOCKET fdArg, KBESOCKET socketArg, SocketKind kindArg,
	Operation operationArg, uint64 generationArg, uint64 requestIdArg)
{
	// Rebuild every pointer-bearing kernel structure after buffers may have moved or changed capacity.
	// 缓冲可能移动或改变容量，因此复用时必须重建所有包含指针的内核结构。
	fd = fdArg;
	socket = socketArg;
	kind = kindArg;
	operation = operationArg;
	generation = generationArg;
	requestId = requestIdArg;
	expectedLateCompletion = false;
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
IoUringPoller::PendingCompletion::PendingCompletion() :
	requestId(0),
	result(0),
	preparedUdpFd(-1),
	preparedUdpSocket(-1),
	preparedUdpGeneration(0),
	preparedUdp(false),
	notifyRead(false)
{
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
	pendingCancelRequestIds_(),
	supportedOperations_(),
	contextPool_(entries),
	lastCompletionBudgetWarningTime_(0),
	lastSqDropped_(0),
	lastCqOverflow_(0),
	supportsAsyncCancel_(false),
	pendingCompletions_(),
	pendingTcpSendCompletions_(),
	pendingUdpCompletions_(),
	consecutiveTcpSendCompletionCount_(0),
	consecutiveNonUdpCompletionCount_(0),
	nextRequestId_(0),
	nextSocketGeneration_(0),
	completionDequeueCallCount_(0),
	completionDequeuedCount_(0),
	completionMaxDequeuedBatchCount_(0),
	tcpSendSubmissionCount_(0),
	tcpSendSubmittedBytes_(0),
	tcpSendMaxSubmissionBytes_(0),
	submitCallCount_(0),
	submitFailureCount_(0),
	submitPartialCount_(0),
	sqCapacityExhaustionCount_(0),
	cancelRequestCount_(0),
	cancelCompletionCount_(0),
	staleCompletionCount_(0),
	udpReceiveDepthDeficitCount_(0),
	udpReceiveWouldBlockCount_(0)
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
	pendingCompletions_.clear();
	pendingTcpSendCompletions_.clear();
	pendingUdpCompletions_.clear();
	pendingCancelRequestIds_.clear();

	for (const auto& item : outstandingContexts_)
	{
		IoUringContext* context = item.second;
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
	for (const auto& item : outstandingContexts_)
	{
		const IoUringContext* context = item.second;
		if (context != NULL)
		{
			bytes += context->retainedBytes();
		}
	}
	return static_cast<uint64>(bytes);
}
uint64 IoUringPoller::contextCachedBytes() const { return static_cast<uint64>(contextPool_.cachedBytes()); }
uint64 IoUringPoller::completionDequeueCallCount() const { return completionDequeueCallCount_; }
uint64 IoUringPoller::completionDequeuedCount() const { return completionDequeuedCount_; }
uint64 IoUringPoller::completionMaxDequeuedBatchCount() const { return completionMaxDequeuedBatchCount_; }
uint64 IoUringPoller::completionPendingLocalCount() const
{
	return static_cast<uint64>(pendingCompletions_.size() +
		pendingTcpSendCompletions_.size() + pendingUdpCompletions_.size());
}
uint64 IoUringPoller::tcpSendSubmissionCount() const { return tcpSendSubmissionCount_; }
uint64 IoUringPoller::tcpSendSubmittedBytes() const { return tcpSendSubmittedBytes_; }
uint64 IoUringPoller::tcpSendMaxSubmissionBytes() const { return tcpSendMaxSubmissionBytes_; }
uint64 IoUringPoller::ioUringSubmitCallCount() const { return submitCallCount_; }
uint64 IoUringPoller::ioUringSubmitFailureCount() const { return submitFailureCount_; }
uint64 IoUringPoller::ioUringSubmitPartialCount() const { return submitPartialCount_; }
uint64 IoUringPoller::ioUringSqCapacityExhaustionCount() const { return sqCapacityExhaustionCount_; }
uint64 IoUringPoller::ioUringSqDroppedCount() const { return lastSqDropped_; }
uint64 IoUringPoller::ioUringCqOverflowCount() const { return lastCqOverflow_; }
uint64 IoUringPoller::ioUringCancelRequestCount() const { return cancelRequestCount_; }
uint64 IoUringPoller::ioUringCancelCompletionCount() const { return cancelCompletionCount_; }
uint64 IoUringPoller::ioUringStaleCompletionCount() const { return staleCompletionCount_; }
uint64 IoUringPoller::ioUringUdpReceiveDepthDeficitCount() const { return udpReceiveDepthDeficitCount_; }
uint64 IoUringPoller::ioUringUdpReceiveWouldBlockCount() const { return udpReceiveWouldBlockCount_; }
uint64 IoUringPoller::ioUringSqEntryCount() const
{
	return ring_.sqRingEntries != NULL ? loadRelaxed(ring_.sqRingEntries) : 0;
}
uint64 IoUringPoller::ioUringCqEntryCount() const
{
	return ring_.cqRingEntries != NULL ? loadRelaxed(ring_.cqRingEntries) : 0;
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

	// 没有 NODROP 时，CQ 满会永久丢失 CQE，使对应 context、buffer 和 fd 生命周期无法收敛。
	// Without NODROP, a full CQ can permanently lose CQEs and strand their contexts, buffers, and descriptor lifetimes.
	if ((params.features & IORING_FEAT_NODROP) == 0)
	{
		errno = ENOTSUP;
		destroyRing();
		return false;
	}

	if (!probeOperations())
	{
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
bool IoUringPoller::probeOperations()
{
	const unsigned operationCount = static_cast<unsigned>(IORING_OP_LAST);
	std::vector<char> storage(sizeof(io_uring_probe) + operationCount * sizeof(io_uring_probe_op), 0);
	io_uring_probe* probe = reinterpret_cast<io_uring_probe*>(storage.data());
	if (ioUringRegister(ringFd_, IORING_REGISTER_PROBE, probe, operationCount) < 0)
	{
		ERROR_MSG(fmt::format("IoUringPoller::probeOperations: IORING_REGISTER_PROBE failed: {}\n",
			kbe_strerror()));
		return false;
	}

	supportedOperations_.clear();
	for (unsigned index = 0; index < probe->ops_len; ++index)
	{
		const io_uring_probe_op& operation = probe->ops[index];
		if ((operation.flags & IO_URING_OP_SUPPORTED) != 0)
		{
			supportedOperations_.insert(operation.op);
		}
	}

	const uint8 requiredOperations[] = {
		IORING_OP_ACCEPT,
		IORING_OP_RECV,
		IORING_OP_RECVMSG,
		IORING_OP_SEND,
		IORING_OP_SENDMSG
	};
	for (uint8 operation : requiredOperations)
	{
		if (!operationSupported(operation))
		{
			errno = ENOTSUP;
			ERROR_MSG(fmt::format("IoUringPoller::probeOperations: required opcode {} is unavailable.\n",
				static_cast<unsigned>(operation)));
			return false;
		}
	}

	supportsAsyncCancel_ = operationSupported(IORING_OP_ASYNC_CANCEL);
	if (!supportsAsyncCancel_)
	{
		WARNING_MSG("IoUringPoller::probeOperations: ASYNC_CANCEL unavailable; using generation-based stale completion cleanup.\n");
	}
	return true;
}

//-------------------------------------------------------------------------------------
bool IoUringPoller::operationSupported(uint8 operation) const
{
	return supportedOperations_.find(operation) != supportedOperations_.end();
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
		++sqCapacityExhaustionCount_;
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

	++submitCallCount_;
	int ret = ioUringEnter(ringFd_, toSubmit, 0, 0);
	if (ret < 0)
	{
		++submitFailureCount_;
		return false;
	}

	if (static_cast<unsigned>(ret) < toSubmit)
	{
		++submitPartialCount_;
	}
	return true;
}

//-------------------------------------------------------------------------------------
bool IoUringPoller::doRegisterForRead(KBESOCKET fd)
{
	// io_uring 不可用时注册失败，让上层能在启动阶段暴露平台能力问题。
	if (ringFd_ < 0)
	{
		return false;
	}

	auto iter = socketStates_.find(fd);
	const bool isNewState = iter == socketStates_.end();
	SocketState& state = socketStateForFd(fd);
	state.socket = fd;
	state.registeredRead = true;
	if (isNewState)
	{
		// 发送路径可能已经为同一 socket 创建状态；只有真正的新状态才开始新 generation。
		// The send path may have created this state already; only a new state starts a socket generation.
		state.generation = nextSocketGeneration();
		clearReceivedData(fd);
	}

	if (!tryDetermineSocketKind(state.socket, state.kind))
	{
		state.registeredRead = false;
		cleanupStateIfUnused(fd);
		return false;
	}

	const bool armed = ensureReadArmed(fd, state);
	if (!armed || !isReadArmComplete(state))
	{
		// SQ 容量是瞬时资源；保留注册并由 FIFO rearm 补投，不能让大量 socket 注册随机失败。
		// SQ capacity is transient; retain registration and refill through the FIFO rearm queue.
		requestRearm(fd, REARM_READ);
	}
	return true;
}

//-------------------------------------------------------------------------------------
bool IoUringPoller::queueTcpSend(KBESOCKET fd, const void* data, int len, size_t maxPendingBytes)
{
	if (ringFd_ < 0)
	{
		return false;
	}

	const bool isNewState = socketStates_.find(fd) == socketStates_.end();
	if (!CompletionPoller::queueTcpSend(fd, data, len, maxPendingBytes))
	{
		return false;
	}
	if (isNewState)
	{
		SocketState& state = socketStateForFd(fd);
		state.socket = fd;
		state.generation = nextSocketGeneration();
	}

	SocketState& state = socketStateForFd(fd);
	if (state.pPendingWriteContext == NULL)
	{
		// Submit the first send immediately. The outstanding request keeps later
		// writes from the same callback in the 64 KiB batch without allowing a long
		// script/migration callback to fill the entire user-space backlog first.
		// 空闲 socket 的首个发送立即提交；outstanding 请求会让同一回调的后续小包
		// 自然进入 64 KiB batch，同时避免长脚本/迁移回调先填满整个用户态积压。
		if (!armTcpSend(fd, state))
		{
			requestRearm(fd, REARM_WRITE);
		}
		submitSqes();
	}
	else
	{
		const size_t pendingBytes = state.pendingTcpSends.pendingBytes();
		const size_t queuedBytes = static_cast<size_t>(len);
		const size_t previousBytes = pendingBytes >= queuedBytes ? pendingBytes - queuedBytes : 0;
		if (pendingBytes / IO_URING_TCP_SEND_PROGRESS_BYTES > previousBytes / IO_URING_TCP_SEND_PROGRESS_BYTES)
		{
			progressTcpSend(fd, state);
		}
	}
	return true;
}

//-------------------------------------------------------------------------------------
bool IoUringPoller::queueUdpSend(KBESOCKET fd, const void* data, int len, const Address& dstAddr)
{
	// UDP/KCP 的发送路径同样尽量在入队时投递，减少高频小包多等一轮 tick 的尾延迟。
	// SQ 满时保留 pending 队列，由主循环后续重试，保持和原有异步语义一致。
	if (ringFd_ < 0)
	{
		return false;
	}

	const bool isNewState = socketStates_.find(fd) == socketStates_.end();
	if (!CompletionPoller::queueUdpSend(fd, data, len, dstAddr))
	{
		return false;
	}

	SocketState& state = socketStateForFd(fd);
	if (isNewState)
	{
		state.socket = fd;
		state.generation = nextSocketGeneration();
	}
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
	auto iter = socketStates_.find(fd);
	if (iter == socketStates_.end())
	{
		return true;
	}

	SocketState& state = *iter->second;
	state.registeredRead = false;
	cancelRearm(fd, REARM_READ);
	discardPreparedUdpCompletions(fd, state.socket, state.generation);
	// 读注销代表 channel/socket 生命周期结束；和 IOCP 一样同时取消读写 outstanding IO。
	// Read deregistration ends the channel/socket lifecycle, so cancel both read and write operations like IOCP.
	cancelStateContexts(state, true, true);
	clearPendingSends(state);
	state.generation = nextSocketGeneration();
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
	cancelStateContexts(state, false, true);
	cleanupStateIfUnused(fd);
	return true;
}

//-------------------------------------------------------------------------------------
bool IoUringPoller::ensureReadArmed(KBESOCKET fd, SocketState& state)
{
	if (!state.registeredRead)
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
		return state.pPendingReadContext != NULL || armAccept(fd, state);
	case SOCKET_KIND_UDP:
		return ensureUdpReadsArmed(fd, state);
	case SOCKET_KIND_TCP:
		return state.pPendingReadContext != NULL || armTcpRead(fd, state);
	default:
		return false;
	}
}

//-------------------------------------------------------------------------------------
uint32 IoUringPoller::udpReceiveDepth(const SocketState& state) const
{
	return ioUringUdpReceiveDepth(isUdpConnected(state));
}

//-------------------------------------------------------------------------------------
uint32 IoUringPoller::udpReceiveBurstSize(const SocketState& state) const
{
	return ioUringUdpReceiveBurstSize(isUdpConnected(state));
}

//-------------------------------------------------------------------------------------
bool IoUringPoller::isUdpConnected(const SocketState& state) const
{
	sockaddr_storage peerAddress;
	socklen_t peerAddressLength = sizeof(peerAddress);
	memset(&peerAddress, 0, sizeof(peerAddress));
	return getpeername(state.socket,
		reinterpret_cast<sockaddr*>(&peerAddress), &peerAddressLength) == 0;
}

//-------------------------------------------------------------------------------------
bool IoUringPoller::isReadArmComplete(const SocketState& state) const
{
	if (state.kind == SOCKET_KIND_UDP)
	{
		return state.pendingReadContexts.size() >= udpReceiveDepth(state);
	}
	return state.pPendingReadContext != NULL;
}

//-------------------------------------------------------------------------------------
bool IoUringPoller::ensureUdpReadsArmed(KBESOCKET fd, SocketState& state)
{
	const uint32 targetDepth = udpReceiveDepth(state);
	while (state.registeredRead && state.pendingReadContexts.size() < targetDepth)
	{
		if (!armUdpRead(fd, state))
		{
			break;
		}
	}

	state.readArmed = !state.pendingReadContexts.empty();
	if (state.pendingReadContexts.size() < targetDepth)
	{
		++udpReceiveDepthDeficitCount_;
	}
	return !state.pendingReadContexts.empty();
}

//-------------------------------------------------------------------------------------
void IoUringPoller::requestCancel(IoUringContext* context)
{
	if (!supportsAsyncCancel_ || context == NULL)
	{
		return;
	}

	auto iter = outstandingContexts_.find(context->requestId);
	if (iter == outstandingContexts_.end() || iter->second != context)
	{
		return;
	}

	if (pendingCancelRequestIds_.insert(context->requestId).second)
	{
		++cancelRequestCount_;
	}
}

//-------------------------------------------------------------------------------------
bool IoUringPoller::armCancel(uint64 requestId)
{
	io_uring_sqe* sqe = getSqe();
	if (sqe == NULL)
	{
		return false;
	}

	sqe->opcode = IORING_OP_ASYNC_CANCEL;
	sqe->fd = -1;
	sqe->addr = requestId;
	sqe->cancel_flags = IORING_ASYNC_CANCEL_USERDATA;
	// The high bit distinguishes cancel completions while the low bits preserve
	// the immutable request ID for diagnostics. Request IDs never use this bit.
	// 最高位区分取消 completion，低位保留不可复用的请求 ID；普通请求永不使用该位。
	sqe->user_data = IO_URING_CONTROL_USER_DATA | requestId;
	return true;
}

//-------------------------------------------------------------------------------------
void IoUringPoller::processCancelRequests()
{
	for (auto iter = pendingCancelRequestIds_.begin(); iter != pendingCancelRequestIds_.end();)
	{
		const uint64 requestId = *iter;
		if (outstandingContexts_.find(requestId) == outstandingContexts_.end())
		{
			iter = pendingCancelRequestIds_.erase(iter);
			continue;
		}

		if (!armCancel(requestId))
		{
			break;
		}
		iter = pendingCancelRequestIds_.erase(iter);
	}
}

//-------------------------------------------------------------------------------------
void IoUringPoller::cancelStateContexts(SocketState& state, bool includeReads, bool includeWrite)
{
	if (includeReads)
	{
		IoUringContext* pendingRead = reinterpret_cast<IoUringContext*>(state.pPendingReadContext);
		if (pendingRead != NULL)
		{
			pendingRead->expectedLateCompletion = true;
			requestCancel(pendingRead);
		}
		for (void* pendingContext : state.pendingReadContexts)
		{
			IoUringContext* context = reinterpret_cast<IoUringContext*>(pendingContext);
			context->expectedLateCompletion = true;
			requestCancel(context);
		}
		state.pPendingReadContext = NULL;
		state.pendingReadContexts.clear();
		state.readArmed = false;
	}

	if (includeWrite)
	{
		IoUringContext* pendingWrite = reinterpret_cast<IoUringContext*>(state.pPendingWriteContext);
		if (pendingWrite != NULL)
		{
			pendingWrite->expectedLateCompletion = true;
			requestCancel(pendingWrite);
		}
		state.pPendingWriteContext = NULL;
		state.writeArmed = false;
	}

	processCancelRequests();
	submitSqes();
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
	sqe->user_data = context->requestId;
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
	context->data.resize(IO_URING_TCP_RECEIVE_BYTES);
	sqe->opcode = IORING_OP_RECV;
	sqe->fd = fd;
	sqe->addr = reinterpret_cast<uint64>(context->data.data());
	sqe->len = static_cast<unsigned>(context->data.size());
	sqe->user_data = context->requestId;
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
	sqe->user_data = context->requestId;
	state.pendingReadContexts.insert(context);
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
	sqe->user_data = context->requestId;
	state.pPendingWriteContext = context;
	state.writeArmed = true;
	state.pendingTcpWriteBytes = context->tcpSendData.size();
	++tcpSendSubmissionCount_;
	tcpSendSubmittedBytes_ += context->tcpSendData.size();
	tcpSendMaxSubmissionBytes_ = std::max<uint64>(tcpSendMaxSubmissionBytes_, context->tcpSendData.size());
	return true;
}

//-------------------------------------------------------------------------------------
bool IoUringPoller::progressTcpSend(KBESOCKET fd, SocketState& state)
{
	IoUringContext* current = reinterpret_cast<IoUringContext*>(state.pPendingWriteContext);
	if (current == NULL || current->operation != OP_TCP_SEND)
	{
		return false;
	}

	auto findCurrent = [this, current]()
	{
		return std::find_if(pendingTcpSendCompletions_.begin(), pendingTcpSendCompletions_.end(),
			[current](const PendingCompletion& pending)
			{
				return !pending.preparedUdp && pending.requestId == current->requestId;
			});
	};

	// 首包可能刚写入本地 SQE 游标；先发布，再从 CQ 复制稳定 completion。
	// The first send may still be in the local SQE cursor; publish it before copying stable completions from the CQ.
	submitSqes();
	auto pendingIter = findCurrent();
	if (pendingIter == pendingTcpSendCompletions_.end())
	{
		// 共享 ring 中还包含其他 socket 的 UDP/TCP completion。业务回调内为了推进
		// 一个 send 而持续 dequeue，会把这些事件从内核有界 CQ 搬到无界用户态 deque，
		// 绕过主循环的 completionBudget。达到单 Tick 上限后停止内联收割，保留 CQE
		// 给 processPendingEvents()；Channel 与 completion 发送队列继续负责有界背压。
		// The shared ring also contains completions for unrelated sockets. Stop inline
		// harvesting once the local fairness window is full so the kernel CQ, rather
		// than an unbounded user-space deque, retains the remaining work.
		if (completionPendingLocalCount() >= static_cast<uint64>(g_maxCompletionsPerTick))
		{
			return false;
		}

		// 与 IOCP 一样，内联发送推进只把 CQE 搬到稳定队列并查找当前 TCP send。
		// UDP completion 必须留给 processPendingEvents() 在公平性预算内展开；如果在
		// 业务回调中调用 prepareUdpCompletions()，一次 TCP 发送探测就可能同步搬运
		// 整个 KCP burst，持续制造不受预算约束的用户态通知积压。
		// Match IOCP by limiting inline send progress to dequeuing stable CQEs and
		// locating the current TCP send. UDP completions stay deferred until
		// processPendingEvents() can expand them inside the fairness lifecycle.
		dequeueCompletions();
		pendingIter = findCurrent();
	}

	// 错误 completion 仍由正常 poll 路径处理，避免在 Entity/脚本回调中重入 Channel 销毁。
	// Leave failed completions to the normal poll path so Channel destruction cannot re-enter an Entity or script callback.
	if (pendingIter == pendingTcpSendCompletions_.end() || pendingIter->result < 0)
	{
		return false;
	}

	const PendingCompletion pending = *pendingIter;
	pendingTcpSendCompletions_.erase(pendingIter);
	auto contextIter = outstandingContexts_.find(pending.requestId);
	if (contextIter == outstandingContexts_.end() || contextIter->second != current)
	{
		return false;
	}

	handleCompletion(*current, pending.result);
	++tcpSendInlineCompletionCount_;
	submitSqes();

	// queueTcpSend 已经放入新数据；成功 completion 会立即投递下一批，因此不能在这里触发上层写回调。
	// queueTcpSend already added new data; the successful completion immediately submits the next batch without an upper-layer write callback.
	(void)fd;
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
	sqe->user_data = context->requestId;
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
	const bool isUdpReadContext = context.operation == OP_UDP_RECV;
	if (state != NULL && !isUdpReadContext)
	{
		ppCurrentContext = (context.operation == OP_TCP_SEND || context.operation == OP_UDP_SEND) ?
			&state->pPendingWriteContext : &state->pPendingReadContext;
	}

	const bool isCurrent = state != NULL &&
		state->socket == context.socket &&
		state->generation == context.generation &&
		(isUdpReadContext ?
			state->pendingReadContexts.find(&context) != state->pendingReadContexts.end() :
			(ppCurrentContext != NULL && *ppCurrentContext == &context));

	if (!isCurrent)
	{
		// 注销路径已经显式退休的请求会像 IOCP 的 ERROR_OPERATION_ABORTED 一样正常回收；
		// stale 只保留给未预期的 generation/context 错配，才能作为真实异常告警。
		// Explicitly retired requests are reclaimed like IOCP ERROR_OPERATION_ABORTED;
		// reserve stale for unexpected generation/context mismatches so the metric remains actionable.
		if (!context.expectedLateCompletion)
		{
			++staleCompletionCount_;
		}
		// 注销或 fd 生命周期重置后，generation 会先递增；旧 CQE 回来时虽然不能再投递给上层，
		// 但如果 SocketState 仍保存着这个旧 context 指针，必须在这里解除引用。
		// 这样 cleanupStateIfUnused 才能在最后一个迟到 CQE 被丢弃后释放 fd 状态。
		if (state != NULL && isUdpReadContext)
		{
			state->pendingReadContexts.erase(&context);
			state->readArmed = !state->pendingReadContexts.empty();
			cleanupStateIfUnused(fd);
		}
		else if (state != NULL && ppCurrentContext != NULL && *ppCurrentContext == &context)
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
		if (isUdpReadContext)
		{
			state->pendingReadContexts.erase(&context);
			state->readArmed = !state->pendingReadContexts.empty();
		}
		else
		{
			*ppCurrentContext = NULL;
			state->readArmed = false;
		}
	}
	else
	{
		*ppCurrentContext = NULL;
		state->writeArmed = false;
		if (context.operation == OP_TCP_SEND)
			state->pendingTcpWriteBytes = 0;
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
				// 先提交下一次 recv，再同步进入 PacketReader/Entity。业务回调可能持续数十毫秒
				// 甚至更久；预挂让内核接收窗口保持打开，同时仍保证每个 TCP socket 只有一个读请求。
				// Submit the next recv before synchronously entering PacketReader/Entity. Business
				// callbacks can run for tens of milliseconds or longer; pre-arming keeps the receive
				// window open while preserving exactly one read request per TCP socket.
				if (!terminal && state->registeredRead &&
					(!ensureReadArmed(fd, *state) || !isReadArmComplete(*state)))
				{
					requestRearm(fd, REARM_READ);
				}
				submitSqes();
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
				if (state->registeredRead &&
					(!ensureUdpReadsArmed(fd, *state) || !isReadArmComplete(*state)))
				{
					requestRearm(fd, REARM_READ);
				}
				submitSqes();
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
		if (result > 0)
		{
			// CQE 的正 result 才证明内核发送路径仍在前进；入队或零字节 completion 都不能刷新组件存活时间。
			// Only a positive CQE result proves kernel send progress; enqueueing or a zero-byte completion must not refresh component liveness.
			state->lastTcpSendProgressTime = timestamp();
		}
		if (static_cast<size_t>(result) < context.tcpSendData.size())
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
		outstandingContexts_[context->requestId] = context;
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
		pendingCancelRequestIds_.erase(context->requestId);
		auto iter = outstandingContexts_.find(context->requestId);
		if (iter != outstandingContexts_.end() && iter->second == context)
		{
			outstandingContexts_.erase(iter);
		}
	}
}

//-------------------------------------------------------------------------------------
IoUringPoller::IoUringContext* IoUringPoller::acquireContext(KBESOCKET fd, KBESOCKET socket, SocketKind kind, Operation operation, uint64 generation)
{
	IoUringContext* context = contextPool_.acquire();
	context->reset(fd, socket, kind, operation, generation, nextRequestId());
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
uint64 IoUringPoller::nextRequestId()
{
	// Zero and the high-bit namespace are reserved for invalid/control completions.
	// 0 和最高位命名空间保留给无效/控制 completion，普通请求 ID 只使用低 63 位。
	do
	{
		nextRequestId_ = (nextRequestId_ + 1) & ~IO_URING_CONTROL_USER_DATA;
	}
	while (nextRequestId_ == 0 || outstandingContexts_.find(nextRequestId_) != outstandingContexts_.end());
	return nextRequestId_;
}

//-------------------------------------------------------------------------------------
uint64 IoUringPoller::nextSocketGeneration()
{
	++nextSocketGeneration_;
	if (nextSocketGeneration_ == 0)
	{
		++nextSocketGeneration_;
	}
	return nextSocketGeneration_;
}

//-------------------------------------------------------------------------------------
size_t IoUringPoller::dequeueCompletions()
{
	++completionDequeueCallCount_;
	const unsigned head = loadRelaxed(ring_.cqHead);
	const unsigned tail = loadAcquire(ring_.cqTail);
	const unsigned available = tail - head;
	const unsigned removed = std::min(available, IO_URING_DEQUEUE_BATCH_SIZE);
	if (removed == 0)
	{
		return 0;
	}

	const unsigned ringMask = loadRelaxed(ring_.cqRingMask);
	for (unsigned index = 0; index < removed; ++index)
	{
		const io_uring_cqe& cqe = ring_.cqes[(head + index) & ringMask];
		if ((cqe.user_data & IO_URING_CONTROL_USER_DATA) != 0)
		{
			++cancelCompletionCount_;
			continue;
		}

		PendingCompletion pending;
		pending.requestId = cqe.user_data;
		pending.result = cqe.res;

		// Classify while the stable context is still tracked. Send completion has no
		// receive-side ordering dependency and can safely bypass queued read callbacks;
		// byte order remains protected by one outstanding TCP send per socket.
		// context 仍稳定登记时完成分类。发送完成不依赖接收侧顺序，可以安全越过已排队的
		// 读回调；每个 socket 单 outstanding TCP send 继续保证字节流顺序。
		auto contextIter = outstandingContexts_.find(pending.requestId);
		if (contextIter != outstandingContexts_.end() &&
			contextIter->second->operation == OP_TCP_SEND)
		{
			pendingTcpSendCompletions_.push_back(pending);
		}
		else
		{
			pendingCompletions_.push_back(pending);
		}
	}

	// CQEs are copied to stable user-space storage before one release store advances
	// the shared head. This avoids a cache-line handoff for every completion.
	// CQE 先复制到稳定的用户态队列，再用一次 release store 推进共享 head，
	// 避免每个 completion 都产生一次共享 cache line 交接。
	storeRelease(ring_.cqHead, head + removed);
	completionDequeuedCount_ += removed;
	completionMaxDequeuedBatchCount_ = std::max<uint64>(completionMaxDequeuedBatchCount_, removed);
	return removed;
}

//-------------------------------------------------------------------------------------
uint32 IoUringPoller::drainUdpReceiveBurst(KBESOCKET fd, uint32 maxDatagrams)
{
	std::array<char, PACKET_MAX_SIZE_UDP> buffer;
	uint32 drained = 0;
	while (drained < maxDatagrams &&
		canQueueUdpReceivedData(fd, PACKET_MAX_SIZE_UDP))
	{
		sockaddr_in sourceAddress;
		std::memset(&sourceAddress, 0, sizeof(sourceAddress));
		iovec iov;
		iov.iov_base = buffer.data();
		iov.iov_len = buffer.size();
		msghdr message;
		std::memset(&message, 0, sizeof(message));
		message.msg_name = &sourceAddress;
		message.msg_namelen = sizeof(sourceAddress);
		message.msg_iov = &iov;
		message.msg_iovlen = 1;

		const ssize_t received = ::recvmsg(fd, &message, MSG_DONTWAIT);
		if (received > 0)
		{
			std::vector<char> data(buffer.data(), buffer.data() + received);
			if (!pushUdpReceivedData(fd, data, sourceAddress, 0))
				break;
			++drained;
			continue;
		}

		if (received < 0 && errno == EINTR)
			continue;

		if (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
			errno != ECONNREFUSED && errno != EHOSTUNREACH)
		{
			WARNING_MSG(fmt::format(
				"IoUringPoller::drainUdpReceiveBurst: recvmsg failed on fd {}: {}\n",
				fd, kbe_strerror(errno)));
		}
		break;
	}

	return drained;
}

//-------------------------------------------------------------------------------------
void IoUringPoller::discardPreparedUdpCompletions(KBESOCKET fd, KBESOCKET socket, uint64 generation)
{
	// prepared UDP 通知已经拥有 handoff queue 中对应 datagram，但还没有触发上层 reader。
	// fd 注销会同时清空 handoff 数据，因此这些通知必须在 generation 递增前一起删除，
	// 否则正常关闭会在下一轮被误报为 stale completion，并产生一次空 read notification。
	// Prepared UDP notifications already own matching datagrams in the handoff queue but
	// have not triggered the upper-layer reader yet. Deregistration clears that handoff data,
	// so remove the notifications before advancing the generation; otherwise a normal close
	// would be misreported as stale and would also generate an empty read notification.
	for (auto iter = pendingUdpCompletions_.begin(); iter != pendingUdpCompletions_.end();)
	{
		const PendingCompletion& pending = *iter;
		if (pending.preparedUdp &&
			pending.preparedUdpFd == fd &&
			pending.preparedUdpSocket == socket &&
			pending.preparedUdpGeneration == generation)
		{
			iter = pendingUdpCompletions_.erase(iter);
			continue;
		}

		++iter;
	}
}

//-------------------------------------------------------------------------------------
void IoUringPoller::prepareUdpCompletions(uint64 maxPendingLocal)
{
	std::set<KBESOCKET> refillFds;
	bool preparedAny = false;
	// Track entries temporarily moved to nonUdpCompletions as well. The public queue-size
	// metric cannot see that local deque during classification, so using it directly would
	// grant the same capacity twice and let UDP burst expansion cross the fairness ceiling.
	// 分类期间还要统计暂存到 nonUdpCompletions 的条目。公开队列指标看不到这个局部 deque，
	// 若直接重复读取指标会把同一容量计算两次，导致 UDP burst 展开越过公平性水位。
	uint64 stagedCompletionCount = completionPendingLocalCount();
	std::deque<PendingCompletion> nonUdpCompletions;
	while (!pendingCompletions_.empty())
	{
		PendingCompletion pending = pendingCompletions_.front();
		pendingCompletions_.pop_front();

		if (pending.preparedUdp || pending.requestId == 0)
		{
			nonUdpCompletions.push_back(pending);
			continue;
		}

		auto contextIter = outstandingContexts_.find(pending.requestId);
		if (contextIter == outstandingContexts_.end())
		{
			// Never dereference an unknown user_data value. A duplicate or corrupted CQE
			// is observable, but cannot be allowed to access recycled context storage.
			// 未登记的 user_data 绝不能解引用；重复或损坏 CQE 只记指标，不能访问已复用内存。
			++staleCompletionCount_;
			if (stagedCompletionCount > 0)
				--stagedCompletionCount;
			continue;
		}
		IoUringContext* context = contextIter->second;

		if (context->operation != OP_UDP_RECV)
		{
			nonUdpCompletions.push_back(pending);
			continue;
		}

		preparedAny = true;
		pending.preparedUdp = true;
		pending.preparedUdpFd = context->fd;
		pending.preparedUdpSocket = context->socket;
		pending.preparedUdpGeneration = context->generation;
		const KBESOCKET fd = context->fd;
		auto stateIter = socketStates_.find(fd);
		SocketState* state = stateIter != socketStates_.end() ? stateIter->second.get() : NULL;
		const bool isCurrent = state != NULL &&
			state->socket == context->socket &&
			state->generation == context->generation &&
			state->pendingReadContexts.find(context) != state->pendingReadContexts.end();

		if (state != NULL)
		{
			state->pendingReadContexts.erase(context);
			state->readArmed = !state->pendingReadContexts.empty();
		}

		if (!isCurrent)
		{
			if (!context->expectedLateCompletion)
			{
				++staleCompletionCount_;
			}
			untrackContext(context);
			recycleContext(context);
			pending.requestId = 0;
			if (stagedCompletionCount > 0)
				--stagedCompletionCount;
			if (state != NULL)
			{
				cleanupStateIfUnused(fd);
			}
			continue;
		}

		const int errorCode = pending.result < 0 ? -pending.result : 0;
		uint32 drainedDatagrams = 0;
		if (pending.result > 0)
		{
			context->data.resize(static_cast<size_t>(pending.result));
			pending.notifyRead = pushUdpReceivedData(fd, context->data, context->addr, 0);
			if (pending.notifyRead)
			{
				// One CQE already owns one local slot. Synchronous recvmsg draining may only
				// consume capacity still available below the dispatcher watermark; excess
				// datagrams remain in the socket buffer and are surfaced by later io_uring reads.
				// 一个 CQE 已占用一个本地槽位。同步 recvmsg drain 只能使用 dispatcher 水位
				// 以下的剩余容量；多余数据报留在 socket 缓冲，由后续 io_uring read 继续交付。
				const uint64 remainingSlots = stagedCompletionCount < maxPendingLocal ?
					maxPendingLocal - stagedCompletionCount : 0;
				// The successful CQE already consumes one logical receive slot. Drain only
				// the remainder of the IOCP-equivalent receive window so one readiness
				// transition cannot bypass the dispatcher completion watermark.
				// 成功 CQE 已占用一个逻辑接收槽位，只同步搬运 IOCP 等价窗口的剩余部分，
				// 避免单次就绪变化绕过 dispatcher completion 水位。
				const uint32 logicalBurstSize = udpReceiveBurstSize(*state);
				const uint32 remainingBurstSize = logicalBurstSize > 0 ? logicalBurstSize - 1 : 0;
				const uint32 drainLimit = static_cast<uint32>(std::min<uint64>(
					remainingSlots, remainingBurstSize));
				drainedDatagrams = drainUdpReceiveBurst(fd, drainLimit);
				stagedCompletionCount += drainedDatagrams;
			}
		}
		else if (errorCode == EAGAIN)
		{
			// Several single-shot receives may observe the same readiness transition on kernels
			// that do not wake socket waiters exclusively. Count the harmless retry signal so
			// production can distinguish useful receive depth from an EAGAIN wakeup storm.
			// 某些内核不会独占唤醒 socket waiter，多个单次 receive 可能观察到同一就绪事件。
			// 该可重试结果只计数不刷日志，用于区分有效接收深度与 EAGAIN 唤醒风暴。
			++udpReceiveWouldBlockCount_;
		}
		else if (errorCode != 0 && errorCode != ECONNREFUSED && errorCode != EHOSTUNREACH &&
			errorCode != ECANCELED)
		{
			WARNING_MSG(fmt::format("IoUringPoller::prepareUdpCompletions: udp recv failed on fd {}: {}\n",
				fd, kbe_strerror(errorCode)));
		}

		untrackContext(context);
		recycleContext(context);
		pending.requestId = 0;
		if (state->registeredRead)
		{
			refillFds.insert(fd);
		}

		// prepareUdpCompletions() 已经完成 context 生命周期、错误分类和重新投递。
		// 只有真正入队了 datagram 的 CQE 才需要后续 triggerRead；队列满、EAGAIN、
		// 取消和迟到 CQE 不应再生成一个什么也不做的逻辑 completion。否则高负载下
		// 这些空通知会绕过接收队列的 bytes/items 上限并无限占用 pending 队列。
		// Context cleanup, error classification, and rearming are already complete here.
		// Queue a logical notification only when a datagram was actually handed off;
		// no-op notifications would bypass receive queue limits and grow without bound.
		if (pending.notifyRead)
		{
			pendingUdpCompletions_.push_back(pending);
		}
		else if (stagedCompletionCount > 0)
		{
			// The raw CQE produced no upper-layer work, so its local slot is released.
			// 原始 CQE 没有生成上层工作，因此释放它占用的本地槽位。
			--stagedCompletionCount;
		}
		for (uint32 index = 0; index < drainedDatagrams; ++index)
		{
			// 同步搬运的每个 datagram 都生成一个逻辑 completion 通知。PacketReceiver
			// 每次通知只取一个报文，因此 Linux 与 IOCP 使用相同的公平性预算语义。
			// Every synchronously drained datagram gets one logical completion notification.
			// PacketReceiver consumes one item per notification, matching IOCP fairness accounting.
			PendingCompletion drainedCompletion;
			drainedCompletion.preparedUdp = true;
			drainedCompletion.preparedUdpFd = pending.preparedUdpFd;
			drainedCompletion.preparedUdpSocket = pending.preparedUdpSocket;
			drainedCompletion.preparedUdpGeneration = pending.preparedUdpGeneration;
			drainedCompletion.notifyRead = true;
			pendingUdpCompletions_.push_back(drainedCompletion);
		}
	}
	pendingCompletions_.swap(nonUdpCompletions);

	if (!preparedAny)
	{
		return;
	}

	// Cancellations release obsolete kernel work first; remaining SQ capacity is then
	// used to restore UDP receive depth for all descriptors in this CQ batch.
	// 先提交取消以释放过期内核请求，再用剩余 SQ 容量为本批次所有 UDP fd 恢复接收深度。
	processCancelRequests();
	for (KBESOCKET fd : refillFds)
	{
		auto stateIter = socketStates_.find(fd);
		if (stateIter == socketStates_.end())
		{
			continue;
		}

		SocketState& state = *stateIter->second;
		if (state.registeredRead &&
			(!ensureUdpReadsArmed(fd, state) || !isReadArmComplete(state)))
		{
			requestRearm(fd, REARM_READ);
		}
	}
	submitSqes();
}

//-------------------------------------------------------------------------------------
int IoUringPoller::processPendingEvents(double maxWait)
{
	// ring 未建立时不能进入 poll/CQ 处理，返回 0 表示本轮没有完成事件。
	if (ringFd_ < 0 || ring_.cqHead == NULL || ring_.cqTail == NULL)
	{
		return 0;
	}

	processCancelRequests();
	processRearmRequests();

	int timeoutMs = toTimeoutMilliseconds(maxWait);

#if ENABLE_WATCHERS
	g_ioUringIdleProfile.start();
#else
	uint64 startTime = timestamp();
#endif

	KBEConcurrency::onStartMainThreadIdling();
	submitSqes();
	if (pendingCompletions_.empty() && pendingTcpSendCompletions_.empty() &&
		pendingUdpCompletions_.empty() &&
		loadRelaxed(ring_.cqHead) == loadAcquire(ring_.cqTail) && timeoutMs > 0)
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
	const uint32 completionProcessingBudgetMs =
		completionProcessingTimeBudgetMs(completionConsecutiveBudgetExhaustions_);
	const uint64 completionProcessingBudget = completionProcessingBudgetMs > 0 ?
		(uint64(completionProcessingBudgetMs) * stampsPerSecond() / 1000) : 0;

	// Keep at most one dequeue batch beyond the per-tick fairness watermark. The kernel CQ
	// remains the bounded backpressure layer while the dispatcher catches up; TCP/control
	// CQEs are still refreshed whenever local work falls below the watermark.
	// 本地队列最多允许比单 Tick 公平性水位多一个 dequeue batch。dispatcher 追赶期间由
	// 内核 CQ 承担有界背压；本地工作降到水位以下后仍会刷新 TCP/控制 CQE。
	const uint64 completionDequeueWatermark = std::max<uint64>(
		1, static_cast<uint64>(g_maxCompletionsPerTick));
	const uint64 completionPendingLimit = completionDequeueWatermark +
		static_cast<uint64>(IO_URING_DEQUEUE_BATCH_SIZE);
	if (completionPendingLocalCount() < completionDequeueWatermark)
	{
		dequeueCompletions();
		prepareUdpCompletions(completionPendingLimit);
	}

	while ((!pendingCompletions_.empty() || !pendingTcpSendCompletions_.empty() ||
		!pendingUdpCompletions_.empty()) &&
		(readyCount == 0 || shouldProcessAnotherCompletion(static_cast<uint32>(readyCount),
			timestamp() - completionProcessingStart, completionProcessingBudget)))
	{
		// Real TCP/control completions get bounded priority over staged UDP
		// notifications. The consecutive counter persists across dispatcher rounds:
		// even when the time budget permits only one completion, a busy TCP login
		// stream cannot starve KCP handshakes indefinitely. UDP remains FIFO.
		// 真实 TCP/控制 completion 对已搬入用户态的 UDP 通知使用有界优先级。
		// 连续计数跨 dispatcher 轮次保留：即使时间预算每轮只允许一个
		// completion，高并发 TCP 登录也不能无限饿死 KCP 握手；UDP 队列内部仍保持 FIFO。
		const bool hasNonUdp = !pendingCompletions_.empty() || !pendingTcpSendCompletions_.empty();
		const bool takeNonUdp = hasNonUdp &&
			(pendingUdpCompletions_.empty() ||
				consecutiveNonUdpCompletionCount_ < IO_URING_NON_UDP_PRIORITY_BURST_SIZE);
		const bool takeTcpSend = takeNonUdp && !pendingTcpSendCompletions_.empty() &&
			(pendingCompletions_.empty() ||
				consecutiveTcpSendCompletionCount_ < IO_URING_TCP_SEND_PRIORITY_BURST_SIZE);
		PendingCompletion pending = takeTcpSend ? pendingTcpSendCompletions_.front() :
			(takeNonUdp ? pendingCompletions_.front() : pendingUdpCompletions_.front());
		if (takeNonUdp)
		{
			if (takeTcpSend)
			{
				pendingTcpSendCompletions_.pop_front();
				++consecutiveTcpSendCompletionCount_;
			}
			else
			{
				pendingCompletions_.pop_front();
				consecutiveTcpSendCompletionCount_ = 0;
			}
			++consecutiveNonUdpCompletionCount_;
		}
		else
		{
			pendingUdpCompletions_.pop_front();
			consecutiveTcpSendCompletionCount_ = 0;
			consecutiveNonUdpCompletionCount_ = 0;
		}
		++readyCount;

		if (pending.preparedUdp)
		{
			if (pending.notifyRead)
			{
				auto stateIter = socketStates_.find(pending.preparedUdpFd);
				if (stateIter != socketStates_.end() && stateIter->second->registeredRead &&
					stateIter->second->socket == pending.preparedUdpSocket &&
					stateIter->second->generation == pending.preparedUdpGeneration)
				{
					this->triggerRead(pending.preparedUdpFd);
				}
				else
				{
					// 该通知来自此前已验证的有效 CQE。triggerRead 可能同步关闭 Channel，
					// 因此同一批次后续通知失效是正常生命周期竞态，不是内核 stale CQE。
					// This notification came from a previously validated CQE. triggerRead may
					// synchronously close the Channel, so later notifications from the same batch
					// becoming invalid is a normal lifetime race, not a stale kernel CQE.
				}
			}
		}
		else if (pending.requestId != 0)
		{
			auto contextIter = outstandingContexts_.find(pending.requestId);
			if (contextIter == outstandingContexts_.end())
			{
				++staleCompletionCount_;
			}
			else
			{
				handleCompletion(*contextIter->second, pending.result);
			}
		}

		if (pendingCompletions_.empty() && pendingTcpSendCompletions_.empty() &&
			pendingUdpCompletions_.empty() &&
			shouldProcessAnotherCompletion(static_cast<uint32>(readyCount),
				timestamp() - completionProcessingStart, completionProcessingBudget))
		{
			if (dequeueCompletions() == 0)
			{
				break;
			}
			prepareUdpCompletions(completionPendingLimit);
		}
	}

	processCancelRequests();
	submitSqes();

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
				readyCount, g_maxCompletionsPerTick, completionProcessingBudgetMs,
				completionProcessingElapsed * 1000 / stampsPerSecond()));
		}
	}

	recordCompletionBatch(static_cast<uint32>(readyCount),
		readyCount >= static_cast<int>(g_maxCompletionsPerTick),
		completionTimeBudgetExhausted(static_cast<uint32>(readyCount),
			completionProcessingElapsed, completionProcessingBudget));

	return readyCount;
}

}
}

#endif // defined(__linux__)
