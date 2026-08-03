#ifndef KBE_NETWORK_KCP_SEND_STATE_H
#define KBE_NETWORK_KCP_SEND_STATE_H

#include <algorithm>
#include <cstdint>

namespace KBEngine
{
namespace Network
{

/**
 * 该值对象把 KCP 未发送队列与待确认队列分开，避免把应用层 admission 水位误报为协议发送窗口阻塞。
 * This value object separates KCP's unsent and unacknowledged queues so an application admission watermark is not misreported as protocol-window blocking.
 */
class KcpSendState
{
public:
	KcpSendState(std::uint32_t queuedSegments, std::uint32_t unackedSegments,
		std::uint32_t sendWindow, std::uint32_t remoteWindow,
		std::uint32_t congestionWindow, bool congestionControlEnabled,
		std::uint64_t pendingPayloadBytes = 0, std::uint64_t payloadLimitBytes = 0) :
		queuedSegments_(queuedSegments),
		unackedSegments_(unackedSegments),
		sendWindow_(sendWindow),
		remoteWindow_(remoteWindow),
		congestionWindow_(congestionWindow),
		congestionControlEnabled_(congestionControlEnabled),
		pendingPayloadBytes_(pendingPayloadBytes),
		payloadLimitBytes_(payloadLimitBytes)
	{
	}

	std::uint32_t effectiveWindow() const
	{
		std::uint32_t window = std::min(sendWindow_, remoteWindow_);
		if (congestionControlEnabled_)
			window = std::min(window, congestionWindow_);
		return window;
	}

	bool isWindowBlocked() const
	{
		// 只有存在尚未发送的数据时，满窗口才构成实际阻塞；纯待 ACK 状态不应计为生产者受阻。
		// A full window is actually blocking only when unsent data exists; an ACK-only tail must not count as producer blocking.
		return queuedSegments_ > 0 && unackedSegments_ >= effectiveWindow();
	}

	bool isAdmissionLimited() const
	{
		// 与 KCPPacketSender 的保护水位保持一致，用于区分内存 admission 限制和 KCP 协议窗口限制。
		// Match KCPPacketSender's protective watermark to distinguish memory admission limiting from KCP protocol-window limiting.
		return static_cast<std::uint64_t>(queuedSegments_) + unackedSegments_ >
			static_cast<std::uint64_t>(sendWindow_) * 2 ||
			(payloadLimitBytes_ > 0 && pendingPayloadBytes_ >= payloadLimitBytes_);
	}

private:
	std::uint32_t queuedSegments_;
	std::uint32_t unackedSegments_;
	std::uint32_t sendWindow_;
	std::uint32_t remoteWindow_;
	std::uint32_t congestionWindow_;
	bool congestionControlEnabled_;
	std::uint64_t pendingPayloadBytes_;
	std::uint64_t payloadLimitBytes_;
};

}
}

#endif
