// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

#include "udp_send_backpressure.h"

namespace KBEngine {
namespace Network
{

UdpSendBackpressure::UdpSendBackpressure() :
	blockedSince_(0),
	rejectionCount_(0)
{
}

void UdpSendBackpressure::recordProgress()
{
	blockedSince_ = 0;
}

bool UdpSendBackpressure::recordBlocked(uint64 now, uint64 timeoutStamps)
{
	++rejectionCount_;
	if (blockedSince_ == 0)
	{
		// timestamp() 理论上可以返回零；用一避免零值哨兵让首个周期被重复初始化。
		// timestamp() may theoretically return zero; use one so the zero sentinel cannot restart the first period.
		blockedSince_ = now == 0 ? 1 : now;
		return false;
	}

	return now >= blockedSince_ && now - blockedSince_ >= timeoutStamps;
}

}
}
