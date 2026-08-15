// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

#ifndef KBE_COMPLETION_UDP_RECEIVE_DEPTH_H
#define KBE_COMPLETION_UDP_RECEIVE_DEPTH_H

#include "common.h"

namespace KBEngine {
namespace Network
{

// IOCP needs several overlapped receive buffers to absorb a completion burst. Raw
// io_uring uses one kernel waiter per UDP socket because Linux may wake all one-shot
// recvmsg requests for the same readiness transition and complete the surplus with
// EAGAIN. A successful io_uring CQE performs a bounded non-blocking drain so both
// backends still expose the same logical receive window to the dispatcher.
// IOCP 需要多个 overlapped receive buffer 吸收 completion 突发。raw io_uring 每个 UDP
// socket 只保留一个内核 waiter，因为 Linux 可能为同一次就绪变化唤醒全部单次 recvmsg，
// 并让多余请求以 EAGAIN 完成。io_uring 成功 CQE 后执行有界非阻塞 drain，因此两个后端
// 对 dispatcher 暴露的逻辑接收窗口仍保持一致。
static const uint32 IOCP_CONNECTED_UDP_RECEIVE_DEPTH = 4;
static const uint32 IOCP_UNCONNECTED_UDP_RECEIVE_DEPTH = 64;
static const uint32 IO_URING_UDP_KERNEL_RECEIVE_DEPTH = 1;

inline uint32 iocpUdpReceiveDepth(bool connected)
{
	return connected ? IOCP_CONNECTED_UDP_RECEIVE_DEPTH :
		IOCP_UNCONNECTED_UDP_RECEIVE_DEPTH;
}

inline uint32 ioUringUdpReceiveDepth(bool connected)
{
	(void)connected;
	return IO_URING_UDP_KERNEL_RECEIVE_DEPTH;
}

inline uint32 ioUringUdpReceiveBurstSize(bool connected)
{
	return iocpUdpReceiveDepth(connected);
}

}
}

#endif // KBE_COMPLETION_UDP_RECEIVE_DEPTH_H
