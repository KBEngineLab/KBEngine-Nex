/*
This source file is part of KBEngine
For the latest info, see http://www.kbengine.org/

Copyright (c) 2008-2018 KBEngine.

KBEngine is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

KBEngine is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.
 
You should have received a copy of the GNU Lesser General Public License
along with KBEngine.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef KBE_NETWORK_INTERFACE_H
#define KBE_NETWORK_INTERFACE_H

#include "common/memorystream.h"
#include "network/common.h"
#include "common/common.h"
#include "common/timer.h"
#include "helper/debug_helper.h"
#include "network/endpoint.h"

namespace KBEngine { 
namespace Network
{
class Address;
class Bundle;
class Channel;
class ChannelTimeOutHandler;
class ChannelDeregisterHandler;
class DelayedChannels;
class ListenerReceiver;
class Packet;
class EventDispatcher;
class MessageHandlers;

class NetworkInterface : public TimerHandler
{
public:
	typedef std::map<Address, Channel *>	ChannelMap;
	typedef std::set<Address>			ChannelMaintenanceSet;
	
	NetworkInterface(EventDispatcher * pDispatcher,
		int32 extlisteningPort_min = -1, int32 extlisteningPort_max = -1, const char * extlisteningInterface = "",
		uint32 extrbuffer = 0, uint32 extwbuffer = 0,
		int32 intlisteningPort_min = 0, int32 intlisteningPort_max = 0, const char * intlisteningInterface = "",
		uint32 intrbuffer = 0, uint32 intwbuffer = 0,
		int32 extlisteningUdpPort_min = -1, int32 extlisteningUdpPort_max = -1);

	~NetworkInterface();

	INLINE const Address & extaddr() const;
	INLINE const Address & extUdpAddr() const;
	INLINE const Address & intaddr() const;

	bool initialize(const char* pEndPointName, uint16 listeningPort_min, uint16 listeningPort_max,
		const char * listeningInterface, EndPoint* pEP, ListenerReceiver* pLR, uint32 rbuffer = 0,
		uint32 wbuffer = 0, ProtocolType protocolType = PROTOCOL_TCP);

	bool registerChannel(Channel* pChannel);
	// listener 接受的新 TCP 连接可以替换同一对端地址的旧 Channel，用于处理内核先复用四元组、主线稍后消费断开 completion 的时序。
	// A newly accepted TCP connection may replace an old Channel for the same peer when the kernel reuses the tuple before the main thread consumes the terminal completion.
	bool registerAcceptedChannel(Channel* pChannel);
	bool deregisterChannel(Channel* pChannel);
	bool deregisterAllChannels();
	Channel * findChannel(const Address & addr);
	Channel * findChannel(KBESOCKET fd);

	ChannelTimeOutHandler * pChannelTimeOutHandler() const
		{ return pChannelTimeOutHandler_; }
	void pChannelTimeOutHandler(ChannelTimeOutHandler * pHandler)
		{ pChannelTimeOutHandler_ = pHandler; }
		
	ChannelDeregisterHandler * pChannelDeregisterHandler() const
		{ return pChannelDeregisterHandler_; }
	void pChannelDeregisterHandler(ChannelDeregisterHandler * pHandler)
		{ pChannelDeregisterHandler_ = pHandler; }

	EventDispatcher & dispatcher()		{ return *pDispatcher_; }

	/* 外部网点和内部网点 */
	EndPoint & extEndpoint()				{ return extEndpoint_; }
	EndPoint & extUdpEndpoint()			{ return extUdpEndpoint_; }
	EndPoint & intEndpoint()				{ return intEndpoint_; }

	const char * c_str() const { return extEndpoint_.c_str(); }
	
	const ChannelMap& channels(void) const { return channelMap_; }
		
	/** 发送相关 */
	void sendIfDelayed(Channel & channel);
	void delayedSend(Channel & channel);
	
	bool good() const
	{
		return (!pExtListenerReceiver_ || extEndpoint_.good()) &&
			(!pExtUdpListenerReceiver_ || extUdpEndpoint_.good()) && intEndpoint_.good();
	}

	void onChannelTimeOut(Channel * pChannel);
	
	/* 
		处理所有channels  
	*/
	void processChannels(KBEngine::Network::MessageHandlers* pMsgHandlers);

	INLINE int32 numExtChannels() const;
	// 这些只读统计仅在 watcher 查询时遍历 ChannelMap，不给网络 Tick 增加持续计数开销，也不会复制 Channel 生命周期状态。
	// These read-only statistics scan ChannelMap only for watcher queries, adding no continuous accounting cost to the network tick and duplicating no Channel lifecycle state.
	uint32 numExternalTcpChannels() const;
	uint32 numExternalWebSocketChannels() const;
	uint32 numExternalKcpChannels() const;
	uint32 numExternalUdpChannels() const;
	uint32 numExternalKcpControlBlocks() const;
	uint32 numExternalKcpUpdateTimers() const;
	uint32 pendingChannelMaintenanceCount() const;

private:
	friend class Channel;

	virtual void handleTimeout(TimerHandle handle, void * arg);

	void closeSocket();
	void cleanupChannel(ChannelMap::iterator iter);
	bool registerChannel(Channel* pChannel, bool replaceExistingAcceptedChannel);
	void requestChannelMaintenance(Channel* pChannel);
	void cancelChannelMaintenance(const Address& address);
	uint64 channelTickEpoch() const { return channelTickEpoch_; }

private:
	EndPoint								extEndpoint_, extUdpEndpoint_, intEndpoint_;

	ChannelMap								channelMap_;
	// 只有进入关闭生命周期的 Channel 才需要每 Tick 推进，正常空闲连接不参与该集合。
	// Only channels in their closing lifecycle require per-tick progress; ordinary idle connections never enter this set.
	ChannelMaintenanceSet					channelMaintenance_;
	// Tick epoch 让 Channel 在首次收发时懒清零窗口计数，避免为全部连接执行无效写入。
	// The tick epoch lets a Channel lazily reset window counters on first activity, avoiding writes to every connection.
	uint64									channelTickEpoch_;

	EventDispatcher *						pDispatcher_;
	
	ListenerReceiver *						pExtListenerReceiver_;
	ListenerReceiver *						pExtUdpListenerReceiver_;
	ListenerReceiver *						pIntListenerReceiver_;
	
	DelayedChannels * 						pDelayedChannels_;
	
	ChannelTimeOutHandler *					pChannelTimeOutHandler_;	// 超时的通道可被这个句柄捕捉， 例如告知上层client断开
	ChannelDeregisterHandler *				pChannelDeregisterHandler_;

	int32									numExtChannels_;
};

}
}

#ifdef CODE_INLINE
#include "network_interface.inl"
#endif
#endif // KBE_NETWORK_INTERFACE_H
