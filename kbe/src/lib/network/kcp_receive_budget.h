#ifndef KBE_NETWORK_KCP_RECEIVE_BUDGET_H
#define KBE_NETWORK_KCP_RECEIVE_BUDGET_H

#include "common/common.h"

namespace KBEngine
{
namespace Network
{

// KCP input may make several already reassembled application packets readable at once. Processing
// all of them inside one socket completion makes that completion non-preemptible and can starve
// timers and other clients. One packet is guaranteed; only cheap packets are batched further.
// KCP 输入可能一次释放多个已重组的应用包。若在一个 socket completion 内全部处理，回调将
// 无法被抢占并饿死 Timer 与其他客户端。每轮保证一个包，仅在处理足够快时继续小批量合并。
static const uint32 KCP_RECEIVE_MIN_PACKETS_PER_SLICE = 1;
static const uint32 KCP_RECEIVE_MAX_PACKETS_PER_SLICE = 8;
static const uint64 KCP_RECEIVE_PROCESSING_BUDGET_MICROS = 500;

inline bool shouldDrainAnotherKcpPacket(uint32 processedPackets,
	uint64 elapsedStamps, uint64 processingBudgetStamps)
{
	if (processedPackets >= KCP_RECEIVE_MAX_PACKETS_PER_SLICE)
		return false;

	return processedPackets < KCP_RECEIVE_MIN_PACKETS_PER_SLICE ||
		processingBudgetStamps == 0 || elapsedStamps < processingBudgetStamps;
}

}
}

#endif // KBE_NETWORK_KCP_RECEIVE_BUDGET_H
