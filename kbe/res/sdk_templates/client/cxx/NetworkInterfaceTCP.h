// Copyright 1998-2016 Epic Games, Inc. All Rights Reserved.

#pragma once

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
	TCP 网络实现。
	后台线程只负责连接与收取原始 TCP 字节流；主线程 process() 负责交给 MessageReader。
	这样可以避免网络线程直接触碰实体系统，也能保持 TCP 流解析状态连续。
*/
class NetworkInterfaceTCP : public NetworkInterfaceBase
{
public:
	NetworkInterfaceTCP();
	~NetworkInterfaceTCP() override;

	bool connectTo(const KBString& addr, uint16 port, InterfaceConnect* callback, int userdata) override;
	void reset() override;
	void close() override;
	bool valid() override;
	bool sendTo(MemoryStream* pMemoryStream) override;
	void process() override;

private:
	void workerLoop_(KBString addr, uint16 port, InterfaceConnect* callback, int userdata, uint64 sessionId);
	void stopWorker_();
	void clearRecvQueue_();
	void closeSocket_(bool fireDisconnectedEvent);
	void fireConnectionState_(InterfaceConnect* callback, const KBString& addr, uint16 port, bool success, int userdata, uint64 sessionId);

	std::atomic<bool> stopRequested_;
	std::atomic<bool> connected_;
	std::atomic<bool> disconnectedEventPending_;
	std::atomic<uint64> sessionId_;

	std::thread workerThread_;
	std::mutex socketMutex_;
	std::mutex sendMutex_;
	std::mutex recvMutex_;

	NativeSocket::Socket socket_;
	std::queue<std::vector<uint8>> recvQueue_;
};

}
