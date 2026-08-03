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

#ifndef KBE_NETWORKCHANCEL_H
#define KBE_NETWORKCHANCEL_H

#include "common/common.h"
#include "common/timer.h"
#include "common/smartpointer.h"
#include "common/timestamp.h"
#include "common/objectpool.h"
#include "helper/debug_helper.h"
#include "network/address.h"
#include "network/event_dispatcher.h"
#include "network/endpoint.h"
#include "network/packet.h"
#include "network/common.h"
#include "network/bundle.h"
#include "network/interfaces.h"
#include "network/packet_filter.h"
#include "network/ikcp.h"

namespace KBEngine { 
namespace Network
{

class Bundle;
class NetworkInterface;
class MessageHandlers;
class PacketReader;
class PacketSender;
class KcpUpdateScheduler;

class Channel : public TimerHandler, public PoolObject
{
	friend class KcpUpdateScheduler;
public:
	typedef KBEShared_ptr< SmartPoolObject< Channel > > SmartPoolObjectPtr;
	static SmartPoolObjectPtr createSmartPoolObj(const std::string& logPoint);
	static ObjectPool<Channel>& ObjPool();
	static Channel* createPoolObject(const std::string& logPoint);
	static void reclaimPoolObject(Channel* obj);
	static void destroyObjPool();
	virtual void onReclaimObject();
	virtual size_t getPoolObjectBytes();
	virtual void onEabledPoolObject();

	enum Traits
	{
		/// This describes the properties of channel from server to server.
		INTERNAL = 0,

		/// This describes the properties of a channel from client to server.
		EXTERNAL = 1,
	};
	
	enum ChannelTypes
	{
		/// 普通通道
		CHANNEL_NORMAL = 0,

		// 浏览器web通道
		CHANNEL_WEB = 1,
	};

	enum Flags
	{
		FLAG_SENDING					= 0x00000001,	// 发送信息中
		FLAG_DESTROYED					= 0x00000002,	// 通道已经销毁
		FLAG_HANDSHAKE					= 0x00000004,	// 已经握手过
		FLAG_CONDEMN_AND_WAIT_DESTROY	= 0x00000008,	// 该频道已经变得不合法，即将在数据发送完毕后关闭
		FLAG_CONDEMN_AND_DESTROY		= 0x00000010,	// 该频道已经变得不合法，即将关闭
		FLAG_CONDEMN					= FLAG_CONDEMN_AND_WAIT_DESTROY | FLAG_CONDEMN_AND_DESTROY,
	};

public:
	Channel();

	Channel(NetworkInterface & networkInterface, 
		const EndPoint * pEndPoint, 
		Traits traits, 
		ProtocolType pt = PROTOCOL_TCP, 
		PacketFilterPtr pFilter = NULL, 
		ChannelID id = CHANNEL_ID_NULL,
		ProtocolSubType protocolSubtype = SUB_PROTOCOL_DEFAULT);

	virtual ~Channel();
	
	static Channel * get(NetworkInterface & networkInterface,
			const Address& addr);
	
	static Channel * get(NetworkInterface & networkInterface,
			const EndPoint* pSocket);
	
	void startInactivityDetection( float inactivityPeriod,
			float checkPeriod = 1.f );
	
	void stopInactivityDetection();

	PacketFilterPtr pFilter() const { return pFilter_; }
	void pFilter(PacketFilterPtr pFilter) { pFilter_ = pFilter; }

	void destroy();
	bool isDestroyed() const { return (flags_ & FLAG_DESTROYED) > 0; }

	NetworkInterface & networkInterface()			{ return *pNetworkInterface_; }
	NetworkInterface* pNetworkInterface()			{ return pNetworkInterface_; }
	void pNetworkInterface(NetworkInterface* pNetworkInterface) { pNetworkInterface_ = pNetworkInterface; }

	INLINE const Address& addr() const;
	void pEndPoint(const EndPoint* pEndPoint);
	INLINE EndPoint * pEndPoint() const;
	// 暴露只读传输类型，使地址索引维护能够区分 TCP 与 UDP/KCP Channel，而不允许外部改写生命周期状态。
	// Expose the transport type read-only so address-index maintenance can distinguish TCP from UDP/KCP channels without allowing external lifecycle mutation.
	ProtocolType protocoltype() const { return protocoltype_; }
	ProtocolSubType protocolSubtype() const { return protocolSubtype_; }
	// 外部客户端在完成传输握手后通过该接口原子更新协议、会话号和 KCP 生命周期。
	// External clients use this API to update protocol, conversation ID, and KCP lifetime atomically after transport negotiation.
	bool configureTransport(ProtocolType protocolType, ProtocolSubType protocolSubtype, ChannelID channelID);
	// 连接回退或复用前恢复默认 TCP 状态，避免旧 KCP 定时器和握手标志污染下一条连接。
	// Restore default TCP state before fallback or reuse so stale KCP timers and handshake flags cannot contaminate the next connection.
	void resetTransport();

	typedef std::vector<Bundle*> Bundles;
	Bundles & bundles();
	const Bundles & bundles() const;

	/**
		创建发送bundle，该bundle可能是从send放入发送队列中获取的，如果队列为空
		则创建一个新的
	*/
	Bundle* createSendBundle();
	void clearBundle();

	INLINE void pushBundle(Bundle* pBundle);

	int32 bundlesLength();

	bool sending() const { return (flags_ & FLAG_SENDING) > 0;}
	void stopSend();

	void send(Bundle * pBundle = NULL);
	// reliable=true 时数据进入 KCP 可靠队列，否则直接作为 UDP 数据报发送。
	// reliable=true queues data through KCP; false sends it directly as a UDP datagram.
	void sendTo(bool reliable, Bundle* pBundle = NULL);
	void delayedSend();

	ikcpcb* pKCP() const { return pKCP_; }
	// 资源验收读取调度器里的实际 active 项，不能只用 KCP Channel 数量推断。
	// Resource validation reads the scheduler's active entry instead of inferring it only from KCP Channel count.
	bool hasKcpUpdateTimer() const;
	void scheduleKcpUpdate(int64 microseconds = 0);


	INLINE PacketReader* pPacketReader() const;
	INLINE PacketSender* pPacketSender() const;
	INLINE void pPacketSender(PacketSender* pPacketSender);
	INLINE PacketReceiver* pPacketReceiver() const;
	INLINE void pPacketReceiver(PacketReceiver* pPacketReceiver);

	Traits traits() const { return traits_; }
	bool isExternal() const { return traits_ == EXTERNAL; }
	bool isInternal() const { return traits_ == INTERNAL; }
		
	void onPacketReceived(int bytes);
	void onPacketSent(int bytes, bool sentCompleted);
	void onSendCompleted();

	const char * c_str() const;
	ChannelID id() const	{ return id_; }
	void id(ChannelID v) { id_ = v; }

	uint32	numPacketsSent() const { return numPacketsSent_; }
	uint32	numPacketsReceived() const { return numPacketsReceived_; }
	uint32	numBytesSent() const { return numBytesSent_; }
	uint32	numBytesReceived() const { return numBytesReceived_; }

	uint64 lastReceivedTime() const { return lastReceivedTime_; }
	void updateLastReceivedTime() { lastReceivedTime_ = timestamp(); }

	void addReceiveWindow(Packet* pPacket);

	uint64 inactivityExceptionPeriod() const { return inactivityExceptionPeriod_; }

	void updateTick(KBEngine::Network::MessageHandlers* pMsgHandlers);
	void processPackets(KBEngine::Network::MessageHandlers* pMsgHandlers, Packet* pPacket);

	uint32 condemn() const 
	{
		if ((flags_ & FLAG_CONDEMN_AND_DESTROY) > 0)
			return FLAG_CONDEMN_AND_DESTROY;

		if ((flags_ & FLAG_CONDEMN_AND_WAIT_DESTROY) > 0)
			return FLAG_CONDEMN_AND_WAIT_DESTROY;

		return 0;
	}

	void condemn(const std::string& reason, bool waitSendCompletedDestroy = false);
	std::string condemnReason() const { return condemnReason_; }

	bool hasHandshake() const { return (flags_ & FLAG_HANDSHAKE) > 0; }

	void setFlags(bool add, uint32 flag)
	{
		if (add)
			flags_ |= flag;
		else
			flags_ &= ~flag;
	}

	ENTITY_ID proxyID() const { return proxyID_; }
	void proxyID(ENTITY_ID pid){ proxyID_ = pid; }
	  
	const std::string& extra() const { return strextra_; }
	void extra(const std::string& s){ strextra_ = s; }

	COMPONENT_ID componentID() const{ return componentID_; }
	void componentID(COMPONENT_ID cid){ componentID_ = cid; }
	// 每次 Channel 生命周期分配唯一会话号，供异步回调和跨组件授权拒绝旧连接重放。
	// Each Channel lifetime receives a unique epoch so async callbacks and relay authorization can fence stale connections.
	uint64 sessionEpoch() const { return sessionEpoch_; }

	bool handshake(Packet* pPacket);
	// 将 TLS 内存 BIO 的输出密文交给 completion poller，保持 socket IO 的单一所有权。
	// Hand memory-BIO ciphertext to the completion poller so socket IO keeps a single owner.
	bool flushSSLNetworkOutput();
	// 处理一个已经完整解码的客户端 WebSocket close，并回送相同 payload。
	// Handle a fully decoded client WebSocket close and echo the same payload.
	bool handleWebSocketClose(const void* payload, size_t length);
	// 以指定状态码启动服务端协议错误关闭，不回显无效的客户端 payload。
	// Start a server protocol-error close with the given status code without echoing an invalid client payload.
	bool handleWebSocketCloseError(uint16 closeCode);
	// 对端 TLS close_notify 到达后回送本端通知，并进入有界的优雅关闭阶段。
	// Reply to a peer TLS close_notify and enter the bounded graceful-close phase.
	void handleTLSCloseNotify();
	// 推进服务端主动关闭或等待对端关闭响应；返回 true 表示可以销毁 socket。
	// Advance server-initiated close or wait for peer responses; true means the socket may be destroyed.
	bool processGracefulClose();
	bool isGracefulClosing() const { return gracefulCloseStarted_; }

	KBEngine::Network::MessageHandlers* pMsgHandlers() const { return pMsgHandlers_; }
	void pMsgHandlers(KBEngine::Network::MessageHandlers* pMsgHandlers) { pMsgHandlers_ = pMsgHandlers; }

	bool waitSend();

	bool initialize(NetworkInterface & networkInterface, 
		const EndPoint * pEndPoint, 
		Traits traits, 
		ProtocolType pt = PROTOCOL_TCP, 
		PacketFilterPtr pFilter = NULL, 
		ChannelID id = CHANNEL_ID_NULL,
		ProtocolSubType protocolSubtype = SUB_PROTOCOL_DEFAULT);

	bool finalise();

	ChannelTypes type() const {
		return channelType_;;
	}

	uint32 getRTT();

private:

	enum TimeOutType
	{
		TIMEOUT_INACTIVITY_CHECK
	};

	virtual void handleTimeout(TimerHandle, void * pUser);
	void clearState( bool warnOnDiscard = false );
	EventDispatcher & dispatcher();
	bool initKcp();
	void finaliseKcp();
	void updateKcp();
	void prepareTickCounters();
	static int kcpOutput(const char* buffer, int length, ikcpcb* kcp, void* user);

private:
	NetworkInterface * 			pNetworkInterface_;
	Traits						traits_;
	ProtocolType				protocoltype_;
	ProtocolSubType			protocolSubtype_;
		
	ChannelID					id_;
	
	TimerHandle					inactivityTimerHandle_;
	
	uint64						inactivityExceptionPeriod_;
	
	uint64						lastReceivedTime_;
	
	Bundles						bundles_;
	
	uint32						lastTickBufferedReceives_;

	PacketReader*				pPacketReader_;

	// Statistics
	uint32						numPacketsSent_;
	uint32						numPacketsReceived_;
	uint32						numBytesSent_;
	uint32						numBytesReceived_;
	uint32						lastTickBytesReceived_;
	uint32						lastTickBytesSent_;
	uint64						lastTickEpoch_;

	PacketFilterPtr				pFilter_;
	
	EndPoint *					pEndPoint_;
	PacketReceiver*				pPacketReceiver_;
	PacketSender*				pPacketSender_;

	// 如果是外部通道且代理了一个前端则会绑定前端代理ID
	ENTITY_ID					proxyID_;

	// 扩展用
	std::string					strextra_;

	// 通道类别
	ChannelTypes				channelType_;

	COMPONENT_ID				componentID_;
	uint64					sessionEpoch_;

	// 支持指定某个通道使用某个消息handlers
	KBEngine::Network::MessageHandlers* pMsgHandlers_;

	uint32						flags_;
	ikcpcb*						pKCP_;

	std::string					condemnReason_;
	// 窗口告警采用状态边沿触发，避免积压期间每个 Bundle 都产生日志并进一步放大拥塞。
	// Window warnings are edge-triggered so every Bundle cannot amplify an existing backlog through logging.
	bool						sendWindowMessagesOverflowWarningActive_;
	bool						sendWindowBytesOverflowWarningActive_;
	bool						receiveWindowMessagesOverflowWarningActive_;
	// 只缓存不足以判定 TLS record header 的前 1-2 字节，避免首个 completion 过短时误判原生协议。
	// Retain only the first 1-2 bytes needed to classify a TLS record header when the initial completion is too short.
	std::vector<char>			tlsDetectionPrefix_;
	bool						gracefulCloseStarted_;
	bool						webSocketCloseSent_;
	bool						webSocketCloseReceived_;
	bool						tlsCloseNotifyReceived_;
	uint64						gracefulCloseDeadline_;

	// 将已经完成协议封装的数据按当前 TLS/IO 后端发送，不再经过 PacketFilter 二次封装。
	// Send protocol-framed bytes through the active TLS/IO backend without a second PacketFilter pass.
	bool sendRawNetworkData(const void* data, int length);
	bool startGracefulClose(const void* closePayload, size_t closePayloadLength, bool peerWebSocketClose);
	bool sendWebSocketClose(const void* payload, size_t length);
	bool hasPendingNetworkSend() const;
};

}
}

#ifdef CODE_INLINE
#include "channel.inl"
#endif
#endif // KBE_NETWORKCHANCEL_H
