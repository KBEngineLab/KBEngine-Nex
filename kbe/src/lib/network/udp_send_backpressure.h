// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

#ifndef KBE_NETWORK_UDP_SEND_BACKPRESSURE_H
#define KBE_NETWORK_UDP_SEND_BACKPRESSURE_H

#include "common/common.h"

namespace KBEngine {
namespace Network
{

class UdpSendBackpressure
{
public:
	UdpSendBackpressure();

	// 成功交付一个数据报意味着发送路径仍在前进，应结束当前连续背压周期。
	// Delivering one datagram proves forward progress, so it ends the current continuous backpressure period.
	void recordProgress();

	// 只有持续无进展达到超时才报告失效，避免按主循环调用次数误断健康连接。
	// Report expiration only after continuous no-progress time reaches the timeout, avoiding disconnects based on loop frequency.
	bool recordBlocked(uint64 now, uint64 timeoutStamps);

	uint64 blockedSince() const { return blockedSince_; }
	uint64 rejectionCount() const { return rejectionCount_; }

private:
	uint64 blockedSince_;
	uint64 rejectionCount_;
};

}
}

#endif // KBE_NETWORK_UDP_SEND_BACKPRESSURE_H
