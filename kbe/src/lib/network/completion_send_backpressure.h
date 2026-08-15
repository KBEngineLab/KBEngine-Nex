#ifndef KBE_NETWORK_COMPLETION_SEND_BACKPRESSURE_H
#define KBE_NETWORK_COMPLETION_SEND_BACKPRESSURE_H

#include "threshold_hysteresis.h"

#include <cstdint>

namespace KBEngine
{
namespace Network
{

/**
 * 内部 completion TCP 链路的生产端背压判定。Channel 中已经滞留 Bundle 时必须保持背压，
 * 否则按待发送字节高低水位迟滞，避免队列在边界附近反复启停。
 * Producer-side backpressure for internal completion TCP links. Buffered Channel bundles keep
 * pressure active; otherwise queued bytes use hysteresis to avoid oscillation near a watermark.
 */
class CompletionSendBackpressure
{
public:
	static bool next(bool active, std::uint64_t pendingBytes, bool hasBufferedBundles,
		std::uint64_t highBytes, std::uint64_t lowBytes)
	{
		if (highBytes == 0)
			return false;

		return hasBufferedBundles || ThresholdHysteresis::next(active, pendingBytes, highBytes, lowBytes);
	}

	static bool shouldCoalesceStandaloneBundle(bool channelOwnedBundle, bool internalChannel,
		bool completionBackend, bool sending, bool hasBufferedBundles)
	{
		// 只有未参与 Channel 尾包复用的独立 Bundle 才需要压缩节点数；其他条件确保
		// 同步/kqueue/外部连接以及尚未积压的正常发送路径完全不变。
		// Only standalone Bundles need node compaction. The remaining conditions keep
		// synchronous, kqueue, external, and non-backlogged send paths unchanged.
		return !channelOwnedBundle && internalChannel && completionBackend && sending && hasBufferedBundles;
	}
};

}
}

#endif
