#ifndef KBE_NETWORK_CHANNEL_INACTIVITY_POLICY_H
#define KBE_NETWORK_CHANNEL_INACTIVITY_POLICY_H

#include <algorithm>
#include <cstdint>

namespace KBEngine
{
namespace Network
{

/**
 * 内部 TCP completion 链路是双向字节流。高业务积压可能把反向心跳压在队尾，
 * 但正字节 native send completion 仍证明连接的发送路径在前进，因此可作为存活证据。
 * 外部连接及 readiness 后端继续只使用接收时间，避免改变既有超时语义。
 *
 * An internal TCP completion link is a bidirectional byte stream. Business backlog may leave
 * reverse heartbeats behind queued data, while positive-byte native send completions still prove
 * transport progress. External and readiness links retain receive-only inactivity semantics.
 */
class ChannelInactivityPolicy
{
public:
	static std::uint64_t effectiveLastActivity(std::uint64_t lastReceivedTime,
		std::uint64_t lastTcpSendProgressTime, bool useSendProgress)
	{
		return useSendProgress ? std::max(lastReceivedTime, lastTcpSendProgressTime) : lastReceivedTime;
	}

	static bool expired(std::uint64_t now, std::uint64_t lastActivityTime,
		std::uint64_t inactivityPeriod)
	{
		return inactivityPeriod > 0 && now >= lastActivityTime &&
			now - lastActivityTime >= inactivityPeriod;
	}
};

}
}

#endif
