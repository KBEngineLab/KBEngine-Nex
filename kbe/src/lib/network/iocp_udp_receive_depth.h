// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

#ifndef KBE_IOCP_UDP_RECEIVE_DEPTH_H
#define KBE_IOCP_UDP_RECEIVE_DEPTH_H

#include "common.h"

namespace KBEngine {
namespace Network
{

// Connected client sockets need a small amount of overlap; an unconnected
// listener must absorb a burst from all peers while the main thread drains IOCP.
// 已 connect 的客户端 socket 只需小深度；未 connect 的共享 listener 必须能
// 在主线程排空 IOCP 时吸收所有客户端的突发包。
static const uint32 IOCP_CONNECTED_UDP_RECEIVE_DEPTH = 4;
static const uint32 IOCP_UNCONNECTED_UDP_RECEIVE_DEPTH = 64;

inline uint32 iocpUdpReceiveDepth(bool connected)
{
	return connected ? IOCP_CONNECTED_UDP_RECEIVE_DEPTH : IOCP_UNCONNECTED_UDP_RECEIVE_DEPTH;
}

}
}

#endif // KBE_IOCP_UDP_RECEIVE_DEPTH_H
