// Copyright 1998-2016 Epic Games, Inc. All Rights Reserved.

#pragma once

#include "ikcp.h"
#include "KBECommon.h"
#include "KBENativeSocket.h"
#include "NetworkInterfaceBase.h"

#include <atomic>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace KBEngine
{

/*
	KCP 网络实现。
	UDP socket 使用原生系统 API；KCP 仍使用 ikcp 作为可靠传输协议。
	后台线程负责 UDP 收包和 KCP 输入，主线程 process() 负责 update 与消息分发。
*/
class NetworkInterfaceKCP : public NetworkInterfaceBase
{
public:
	NetworkInterfaceKCP();
	~NetworkInterfaceKCP() override;

	bool connectTo(const KBString& addr, uint16 port, InterfaceConnect* callback, int userdata) override;
	void reset() override;
	void close() override;
	bool valid() override;
	bool sendTo(MemoryStream* pMemoryStream) override;
	void process() override;

	ikcpcb* pKCP()
	{
		return kcp_;
	}

private:
	bool initKCP_();
	void finiKCP_();
	void workerLoop_(KBString addr, uint16 port, InterfaceConnect* callback, int userdata, uint64 sessionId);
	void stopWorker_();
	void clearRecvQueue_();
	void closeSocket_(bool fireDisconnectedEvent);
	void handleDatagram_(const uint8* data, int32 length, uint64 sessionId);
	void drainKCPRecvLocked_();
	void fireConnectionState_(InterfaceConnect* callback, const KBString& addr, uint16 port, bool success, int userdata, uint64 sessionId);
	bool sendDatagram_(const uint8* data, int32 length);
	static uint32 nowMs_();

	static int kcpOutput_(const char* buf, int len, ikcpcb* kcp, void* user);

	ikcpcb* kcp_;
	uint32 connID_;
	uint32 nextKcpUpdate_;

	std::atomic<bool> stopRequested_;
	std::atomic<bool> connected_;
	std::atomic<bool> disconnectedEventPending_;
	std::atomic<uint64> sessionId_;

	std::thread workerThread_;
	std::mutex socketMutex_;
	std::mutex sendMutex_;
	std::mutex recvMutex_;
	std::mutex kcpMutex_;

	NativeSocket::Socket socket_;
	std::queue<std::vector<uint8>> recvQueue_;
};

}
