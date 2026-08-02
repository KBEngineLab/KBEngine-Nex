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


#include "channel.h"
#ifndef CODE_INLINE
#include "channel.inl"
#endif

#include "network/websocket_protocol.h"
#include "network/websocket_packet_filter.h"
#include "network/websocket_packet_reader.h"
#include "network/bundle.h"
#include "network/packet_reader.h"
#include "network/network_interface.h"
#include "network/event_poller.h"
#include "network/tcp_packet_receiver.h"
#include "network/tcp_packet_sender.h"
#include "network/udp_packet_receiver.h"
#include "network/kcp_packet_receiver.h"
#include "network/kcp_packet_reader.h"
#include "network/kcp_packet_sender.h"
#include "network/udp_packet_sender.h"
#include "network/tcp_packet.h"
#include "network/udp_packet.h"
#include "network/message_handler.h"
#include "network/network_stats.h"
#include "helper/profile.h"
#include "common/ssl.h"
#include "common/kbeversion.h"
#include <atomic>
#include <cstring>

namespace KBEngine { 
namespace Network
{

namespace
{
// 空闲 KCP 没有需要刷新、确认或重传的状态；它不需要继续占用全局调度堆。
// 收到数据或提交发送队列时，接收器/发送器会重新调用 scheduleKcpUpdate() 唤醒它。
// A fully idle KCP has nothing to flush, acknowledge, or retransmit, so it does not
// need to occupy the global scheduler heap. Receives and sends call scheduleKcpUpdate()
// again before touching KCP state and wake the connection on demand.
bool isKcpIdle(const ikcpcb& kcp)
{
	return kcp.nsnd_que == 0 && kcp.nsnd_buf == 0 &&
		kcp.nrcv_que == 0 && kcp.nrcv_buf == 0 &&
		kcp.ackcount == 0 && kcp.probe == 0 &&
		kcp.probe_wait == 0 && kcp.rmt_wnd != 0;
}
}

// 优雅关闭必须有硬上限，防止失联客户端永久占用 Channel、socket 和 completion 状态。
// Graceful close needs a hard bound so an absent peer cannot retain Channel, socket, and completion state forever.
static const uint64 GRACEFUL_CLOSE_TIMEOUT_STAMPS = 5 * stampsPerSecond();
static std::atomic<uint64> g_nextChannelSessionEpoch(1);

//-------------------------------------------------------------------------------------
static ObjectPool<Channel> _g_objPool("Channel");
ObjectPool<Channel>& Channel::ObjPool()
{
	return _g_objPool;
}

//-------------------------------------------------------------------------------------
Channel* Channel::createPoolObject(const std::string& logPoint)
{
	return _g_objPool.createObject(logPoint);
}

//-------------------------------------------------------------------------------------
void Channel::reclaimPoolObject(Channel* obj)
{
	_g_objPool.reclaimObject(obj);
}

//-------------------------------------------------------------------------------------
void Channel::destroyObjPool()
{
	DEBUG_MSG(fmt::format("Channel::destroyObjPool(): size {}.\n", 
		_g_objPool.size()));

	_g_objPool.destroy();
}

//-------------------------------------------------------------------------------------
size_t Channel::getPoolObjectBytes()
{
	size_t bytes = sizeof(pNetworkInterface_) + sizeof(traits_) + sizeof(protocoltype_) + sizeof(protocolSubtype_) +
		sizeof(id_) + sizeof(inactivityTimerHandle_) + sizeof(inactivityExceptionPeriod_) + 
		sizeof(lastReceivedTime_) + sizeof(lastTickBufferedReceives_) + sizeof(pPacketReader_) + (bundles_.size() * sizeof(Bundle*)) +
		+ sizeof(flags_) + sizeof(numPacketsSent_) + sizeof(numPacketsReceived_) + sizeof(numBytesSent_) + sizeof(numBytesReceived_)
		+ sizeof(lastTickBytesReceived_) + sizeof(lastTickBytesSent_) + sizeof(lastTickEpoch_) + sizeof(pFilter_) + sizeof(pEndPoint_) + sizeof(pPacketReceiver_) + sizeof(pPacketSender_)
		+ sizeof(proxyID_) + strextra_.size() + sizeof(channelType_)
		+ sizeof(componentID_) + sizeof(sessionEpoch_) + sizeof(pMsgHandlers_) + sizeof(pKCP_) + condemnReason_.size();

	return bytes;
}

//-------------------------------------------------------------------------------------
Channel::SmartPoolObjectPtr Channel::createSmartPoolObj(const std::string& logPoint)
{
	return SmartPoolObjectPtr(new SmartPoolObject<Channel>(ObjPool().createObject(logPoint), _g_objPool));
}

//-------------------------------------------------------------------------------------
void Channel::onReclaimObject()
{
	this->clearState();
}

//-------------------------------------------------------------------------------------
void Channel::onEabledPoolObject()
{
	sessionEpoch_ = g_nextChannelSessionEpoch.fetch_add(1, std::memory_order_relaxed);
	if (sessionEpoch_ == 0)
		sessionEpoch_ = g_nextChannelSessionEpoch.fetch_add(1, std::memory_order_relaxed);
}

//-------------------------------------------------------------------------------------
Channel::Channel(NetworkInterface & networkInterface,
		const EndPoint * pEndPoint, Traits traits, ProtocolType pt,
		PacketFilterPtr pFilter, ChannelID id, ProtocolSubType protocolSubtype):
	pNetworkInterface_(NULL),
	traits_(traits),
	protocoltype_(pt),
	protocolSubtype_(protocolSubtype),
	id_(id),
	inactivityTimerHandle_(),
	inactivityExceptionPeriod_(0),
	lastReceivedTime_(0),
	bundles_(),
	lastTickBufferedReceives_(0),
	pPacketReader_(0),
	numPacketsSent_(0),
	numPacketsReceived_(0),
	numBytesSent_(0),
	numBytesReceived_(0),
	lastTickBytesReceived_(0),
	lastTickBytesSent_(0),
	lastTickEpoch_(0),
	pFilter_(pFilter),
	pEndPoint_(NULL),
	pPacketReceiver_(NULL),
	pPacketSender_(NULL),
	proxyID_(0),
	strextra_(),
	channelType_(CHANNEL_NORMAL),
	componentID_(UNKNOWN_COMPONENT_TYPE),
	sessionEpoch_(0),
	pMsgHandlers_(NULL),
	flags_(0),
	pKCP_(NULL),
	condemnReason_(),
	tlsDetectionPrefix_(),
	gracefulCloseStarted_(false),
	webSocketCloseSent_(false),
	webSocketCloseReceived_(false),
	tlsCloseNotifyReceived_(false),
	gracefulCloseDeadline_(0)
{
	this->clearBundle();
	initialize(networkInterface, pEndPoint, traits, pt, pFilter, id, protocolSubtype);
}

//-------------------------------------------------------------------------------------
Channel::Channel():
	pNetworkInterface_(NULL),
	traits_(EXTERNAL),
	protocoltype_(PROTOCOL_TCP),
	protocolSubtype_(SUB_PROTOCOL_DEFAULT),
	id_(0),
	inactivityTimerHandle_(),
	inactivityExceptionPeriod_(0),
	lastReceivedTime_(0),
	bundles_(),
	lastTickBufferedReceives_(0),
	pPacketReader_(0),
	// Stats
	numPacketsSent_(0),
	numPacketsReceived_(0),
	numBytesSent_(0),
	numBytesReceived_(0),
	lastTickBytesReceived_(0),
	lastTickBytesSent_(0),
	lastTickEpoch_(0),
	pFilter_(NULL),
	pEndPoint_(NULL),
	pPacketReceiver_(NULL),
	pPacketSender_(NULL),
	proxyID_(0),
	strextra_(),
	channelType_(CHANNEL_NORMAL),
	componentID_(UNKNOWN_COMPONENT_TYPE),
	sessionEpoch_(0),
	pMsgHandlers_(NULL),
	flags_(0),
	pKCP_(NULL),
	condemnReason_(),
	tlsDetectionPrefix_(),
	gracefulCloseStarted_(false),
	webSocketCloseSent_(false),
	webSocketCloseReceived_(false),
	tlsCloseNotifyReceived_(false),
	gracefulCloseDeadline_(0)
{
	this->clearBundle();
}

//-------------------------------------------------------------------------------------
Channel::~Channel()
{
	// DEBUG_MSG(fmt::format("Channel::~Channel(): {}\n", this->c_str()));
	finalise();
}

//-------------------------------------------------------------------------------------
bool Channel::initialize(NetworkInterface & networkInterface, 
		const EndPoint * pEndPoint, 
		Traits traits, 
		ProtocolType pt, 
		PacketFilterPtr pFilter, 
		ChannelID id,
		ProtocolSubType protocolSubtype)
{
	id_ = id;
	protocoltype_ = pt;
	protocolSubtype_ = protocoltype_ == PROTOCOL_UDP && protocolSubtype == SUB_PROTOCOL_DEFAULT
		? SUB_PROTOCOL_UDP : protocolSubtype;
	traits_ = traits;
	pFilter_ = pFilter;
	pNetworkInterface_ = &networkInterface;
	this->pEndPoint(pEndPoint);

	KBE_ASSERT(pNetworkInterface_ != NULL);
	KBE_ASSERT(pEndPoint_ != NULL);

	if(protocoltype_ == PROTOCOL_TCP)
	{
		if(pPacketReceiver_)
		{
			if(pPacketReceiver_->type() == PacketReceiver::UDP_PACKET_RECEIVER)
			{
				SAFE_RELEASE(pPacketReceiver_);
				pPacketReceiver_ = new TCPPacketReceiver(*pEndPoint_, *pNetworkInterface_);
			}
		}
		else
		{
			pPacketReceiver_ = new TCPPacketReceiver(*pEndPoint_, *pNetworkInterface_);
		}

		KBE_ASSERT(pPacketReceiver_->type() == PacketReceiver::TCP_PACKET_RECEIVER);

		// UDP不需要注册描述符
		pNetworkInterface_->dispatcher().registerReadFileDescriptor(*pEndPoint_, pPacketReceiver_);

		// 需要发送数据时再注册
		// pPacketSender_ = new TCPPacketSender(*pEndPoint_, *pNetworkInterface_);
		// pNetworkInterface_->dispatcher().registerWriteFileDescriptor(*pEndPoint_, pPacketSender_);
	}
	else
	{
		const bool useKcp = protocolSubtype_ == SUB_PROTOCOL_KCP;
		if(pPacketReceiver_)
		{
			const bool receiverMismatch = pPacketReceiver_->type() == PacketReceiver::TCP_PACKET_RECEIVER ||
				(pPacketReceiver_->type() == PacketReceiver::UDP_PACKET_RECEIVER &&
				 static_cast<UDPPacketReceiver*>(pPacketReceiver_)->protocolSubType() != protocolSubtype_);
			if(receiverMismatch)
			{
				SAFE_RELEASE(pPacketReceiver_);
				pPacketReceiver_ = useKcp ? static_cast<PacketReceiver*>(new KCPPacketReceiver(*pEndPoint_, *pNetworkInterface_)) : static_cast<PacketReceiver*>(new UDPPacketReceiver(*pEndPoint_, *pNetworkInterface_));
			}
		}
		else
		{
			pPacketReceiver_ = useKcp ? static_cast<PacketReceiver*>(new KCPPacketReceiver(*pEndPoint_, *pNetworkInterface_)) : static_cast<PacketReceiver*>(new UDPPacketReceiver(*pEndPoint_, *pNetworkInterface_));
		}

		KBE_ASSERT(pPacketReceiver_->type() == PacketReceiver::UDP_PACKET_RECEIVER);

		SAFE_RELEASE(pPacketSender_);
		pPacketSender_ = useKcp ? static_cast<PacketSender*>(new KCPPacketSender(*pEndPoint_, *pNetworkInterface_)) : static_cast<PacketSender*>(new UDPPacketSender(*pEndPoint_, *pNetworkInterface_));
		if (useKcp)
		{
			if (!pPacketReader_ || pPacketReader_->type() != PacketReader::PACKET_READER_TYPE_KCP)
			{
				SAFE_RELEASE(pPacketReader_);
				pPacketReader_ = new KCPPacketReader(this);
			}

			if (!initKcp())
				return false;
		}
	}

	pPacketReceiver_->pEndPoint(pEndPoint_);
	if(pPacketSender_)
		pPacketSender_->pEndPoint(pEndPoint_);

	startInactivityDetection((traits_ == INTERNAL) ? g_channelInternalTimeout : 
													g_channelExternalTimeout,
							(traits_ == INTERNAL) ? g_channelInternalTimeout / 2.f: 
													g_channelExternalTimeout / 2.f);

	return true;
}

//-------------------------------------------------------------------------------------
bool Channel::finalise()
{
	this->clearState();
	
	SAFE_RELEASE(pPacketReceiver_);
	SAFE_RELEASE(pPacketReader_);
	SAFE_RELEASE(pPacketSender_);

	if (pEndPoint_)
	{
		// Channel与EndPoint对象池位于不同编译单元，进程退出时二者的静态析构顺序没有保证。
		// Channel and EndPoint pools live in different translation units, so their static destruction order is unspecified during process shutdown.
		// 空闲Channel早已释放端点；跳过空指针回收可避免析构阶段再次访问已销毁的EndPoint对象池。
		// An idle Channel has already released its endpoint; skipping a null reclaim avoids touching a destroyed EndPoint pool during static teardown.
		EndPoint* pEndPoint = pEndPoint_;
		pEndPoint_ = NULL;
		Network::EndPoint::reclaimPoolObject(pEndPoint);
	}

	return true;
}

//-------------------------------------------------------------------------------------
bool Channel::configureTransport(ProtocolType protocolType, ProtocolSubType protocolSubtype, ChannelID channelID)
{
	finaliseKcp();
	protocoltype_ = protocolType;
	protocolSubtype_ = protocolType == PROTOCOL_UDP && protocolSubtype == SUB_PROTOCOL_DEFAULT
		? SUB_PROTOCOL_UDP : protocolSubtype;
	id_ = channelID;
	setFlags(false, FLAG_HANDSHAKE);

	if (protocolSubtype_ == SUB_PROTOCOL_KCP)
		return initKcp();

	return true;
}

//-------------------------------------------------------------------------------------
void Channel::resetTransport()
{
	finaliseKcp();
	protocoltype_ = PROTOCOL_TCP;
	protocolSubtype_ = SUB_PROTOCOL_DEFAULT;
	id_ = CHANNEL_ID_NULL;
	setFlags(false, FLAG_HANDSHAKE);
}

//-------------------------------------------------------------------------------------
uint32 Channel::getRTT()
{
	if (protocolSubtype_ == SUB_PROTOCOL_KCP && pKCP_)
		return static_cast<uint32>(pKCP_->rx_srtt) * 1000;

	if (!pEndPoint())
		return 0;

	return pEndPoint()->getRTT();
}

//-------------------------------------------------------------------------------------
bool Channel::initKcp()
{
	if (pKCP_)
		return true;

	// 每个组件在单线程 dispatcher 中创建 Channel，因此递增会话号不需要额外加锁；零值保留为未分配状态。
	// Channels are created on the component's single dispatcher thread, so the increment needs no lock; zero remains reserved for the unassigned state.
	static IUINT32 nextConversation = 1;
	if (id_ == CHANNEL_ID_NULL)
	{
		id_ = static_cast<ChannelID>(nextConversation++);
		if (nextConversation == 0)
			nextConversation = 1;
	}

	pKCP_ = ikcp_create(static_cast<IUINT32>(id_), this);
	if (!pKCP_)
	{
		ERROR_MSG(fmt::format("Channel::initKcp: ikcp_create failed, channel={}\n", c_str()));
		return false;
	}

	pKCP_->output = &Channel::kcpOutput;
	const int sendWindow = static_cast<int>(isExternal() ? g_rudp_extWritePacketsQueueSize : g_rudp_intWritePacketsQueueSize);
	const int receiveWindow = static_cast<int>(isExternal() ? g_rudp_extReadPacketsQueueSize : g_rudp_intReadPacketsQueueSize);
	ikcp_wndsize(pKCP_, sendWindow, receiveWindow);
	ikcp_nodelay(pKCP_, g_rudp_nodelay ? 1 : 0, static_cast<int>(g_rudp_tickInterval),
		static_cast<int>(g_rudp_missAcksResend), g_rudp_congestionControl ? 0 : 1);
	pKCP_->rx_minrto = static_cast<IUINT32>(g_rudp_minRTO);

	const int mtu = isExternal() && g_rudp_mtu > 0 && g_rudp_mtu < PACKET_MAX_SIZE_UDP * 4
		? static_cast<int>(g_rudp_mtu) : PACKET_MAX_SIZE_UDP - 72;
	if (ikcp_setmtu(pKCP_, mtu) < 0)
	{
		ERROR_MSG(fmt::format("Channel::initKcp: invalid mtu={}, channel={}\n", mtu, c_str()));
		finaliseKcp();
		return false;
	}

	scheduleKcpUpdate();
	return true;
}

//-------------------------------------------------------------------------------------
void Channel::finaliseKcp()
{
	if (pNetworkInterface_)
		pNetworkInterface_->kcpUpdateScheduler_.cancel(*this);

	if (pKCP_)
	{
		if (pNetworkInterface_)
		{
			pNetworkInterface_->accumulateFinalizedKcpDiagnostics(
				static_cast<uint64>(pKCP_->ack_sent),
				static_cast<uint64>(pKCP_->ack_received),
				static_cast<uint64>(pKCP_->timeout_retransmissions),
				static_cast<uint64>(pKCP_->fast_retransmissions));
		}
		ikcp_release(pKCP_);
		pKCP_ = NULL;
	}
}

//-------------------------------------------------------------------------------------
int Channel::kcpOutput(const char* buffer, int length, ikcpcb* kcp, void* user)
{
	Channel* pChannel = static_cast<Channel*>(user);
	if (!pChannel || pChannel->condemn() == FLAG_CONDEMN_AND_DESTROY || !pChannel->pPacketSender_)
		return -1;

	return static_cast<KCPPacketSender*>(pChannel->pPacketSender_)->kcp_output(buffer, length, kcp, pChannel);
}

//-------------------------------------------------------------------------------------
void Channel::scheduleKcpUpdate(int64 microseconds)
{
	if (!pKCP_ || isDestroyed() || !pNetworkInterface_)
		return;

	pNetworkInterface_->kcpUpdateScheduler_.schedule(*this, microseconds > 0 ? microseconds : 1);
}

//-------------------------------------------------------------------------------------
bool Channel::hasKcpUpdateTimer() const
{
	return pNetworkInterface_ != NULL && pNetworkInterface_->kcpUpdateScheduler_.isScheduled(*this);
}

//-------------------------------------------------------------------------------------
void Channel::updateKcp()
{
	if (!pKCP_ || isDestroyed())
		return;

	const IUINT32 current = static_cast<IUINT32>(kbe_clock());
	ikcp_update(pKCP_, current);
	if (isKcpIdle(*pKCP_))
	{
		// 完全空闲时取消当前队列项；真实 UDP 收包和 KCP 发送会在进入 KCP 前重新入队。
		// Cancel the queue entry for a fully idle connection; real UDP receives and KCP sends
		// re-enqueue it before entering KCP state.
		if (pNetworkInterface_ != NULL)
			pNetworkInterface_->kcpUpdateScheduler_.cancel(*this);
		return;
	}

	const IUINT32 next = ikcp_check(pKCP_, current);
	// KCP uses wrapping 32-bit milliseconds; signed subtraction preserves ordering across the roughly 49-day wrap boundary.
	// KCP 使用会回绕的 32 位毫秒时钟；有符号差值可在约 49 天回绕边界保持正确顺序。
	const IINT32 checkedDelay = static_cast<IINT32>(next - current);
	const IUINT32 delay = checkedDelay > 0 ? static_cast<IUINT32>(checkedDelay) : 1;
	scheduleKcpUpdate(static_cast<int64>(delay) * 1000);
}

//-------------------------------------------------------------------------------------
Channel * Channel::get(NetworkInterface & networkInterface,
			const Address& addr)
{
	return networkInterface.findChannel(addr);
}

//-------------------------------------------------------------------------------------
Channel * get(NetworkInterface & networkInterface,
		const EndPoint* pSocket)
{
	return networkInterface.findChannel(pSocket->addr());
}

//-------------------------------------------------------------------------------------
void Channel::startInactivityDetection( float period, float checkPeriod )
{
	stopInactivityDetection();

	// 如果周期为负数则不检查
	if (period > 0.1f)
	{
		checkPeriod = std::max(1.f, checkPeriod);

		int64 icheckPeriod = int64(checkPeriod * 1000000);
		if (icheckPeriod <= 0)
		{
			ERROR_MSG(fmt::format("Channel::startInactivityDetection: checkPeriod overflowed, close checker! period={}, checkPeriod={}\n", period, checkPeriod));
			return;
		}

		inactivityExceptionPeriod_ = uint64(period * stampsPerSecond()) - uint64(0.05f * stampsPerSecond());
		lastReceivedTime_ = timestamp();

		inactivityTimerHandle_ =
			this->dispatcher().addTimer(icheckPeriod,
				this, (void *)TIMEOUT_INACTIVITY_CHECK);
	}
}

//-------------------------------------------------------------------------------------
void Channel::stopInactivityDetection()
{
	inactivityTimerHandle_.cancel();
}

//-------------------------------------------------------------------------------------
void Channel::pEndPoint(const EndPoint* pEndPoint)
{
	if (pEndPoint_ != pEndPoint)
	{
		Network::EndPoint::reclaimPoolObject(pEndPoint_);
		pEndPoint_ = const_cast<EndPoint*>(pEndPoint);
	}
	
	lastReceivedTime_ = timestamp();
}

//-------------------------------------------------------------------------------------
void Channel::destroy()
{
	if(isDestroyed())
	{
		CRITICAL_MSG("is channel has Destroyed!\n");
		return;
	}

	if (pNetworkInterface_)
		pNetworkInterface_->requestChannelMaintenance(this);

	clearState();
	flags_ |= FLAG_DESTROYED;
}

//-------------------------------------------------------------------------------------
void Channel::clearState( bool warnOnDiscard /*=false*/ )
{
	// KCP timer 必须在 Channel 进入对象池前取消，否则旧回调可能作用于已经复用的新连接。
	// The KCP timer must be cancelled before pooling the Channel, or a stale callback could target a newly reused connection.
	finaliseKcp();
	clearBundle();

	lastReceivedTime_ = timestamp();

	numPacketsSent_ = 0;
	numPacketsReceived_ = 0;
	numBytesSent_ = 0;
	numBytesReceived_ = 0;
	lastTickBytesReceived_ = 0;
	lastTickBytesSent_ = 0;
	lastTickBufferedReceives_ = 0;
	lastTickEpoch_ = pNetworkInterface_ ? pNetworkInterface_->channelTickEpoch() : 0;
	proxyID_ = 0;
	strextra_ = "";
	channelType_ = CHANNEL_NORMAL;
	condemnReason_ = "";
	tlsDetectionPrefix_.clear();
	gracefulCloseStarted_ = false;
	webSocketCloseSent_ = false;
	webSocketCloseReceived_ = false;
	tlsCloseNotifyReceived_ = false;
	gracefulCloseDeadline_ = 0;

	if(pEndPoint_ && protocoltype_ == PROTOCOL_TCP && !this->isDestroyed())
	{
		this->stopSend();

		if(pNetworkInterface_)
		{
			if(!this->isDestroyed())
				pNetworkInterface_->dispatcher().deregisterReadFileDescriptor(*pEndPoint_);
		}
	}

	// 这里只清空状态，不释放
	//SAFE_RELEASE(pPacketReader_);
	//SAFE_RELEASE(pPacketSender_);

	if (pPacketReader_)
		pPacketReader_->reset();

	flags_ = 0;
	pFilter_ = NULL;

	stopInactivityDetection();

	// 由于pEndPoint通常由外部给入，必须释放，频道重新激活时会重新赋值
	if(pEndPoint_)
	{
		pEndPoint_->destroySSL();
		pEndPoint_->close();
		this->pEndPoint(NULL);
	}
}

//-------------------------------------------------------------------------------------
Channel::Bundles & Channel::bundles()
{
	return bundles_;
}

//-------------------------------------------------------------------------------------
const Channel::Bundles & Channel::bundles() const
{
	return bundles_;
}

//-------------------------------------------------------------------------------------
int32 Channel::bundlesLength()
{
	int32 len = 0;
	Bundles::iterator iter = bundles_.begin();
	for(; iter != bundles_.end(); ++iter)
	{
		len += (*iter)->packetsLength();
	}

	return len;
}

//-------------------------------------------------------------------------------------
void Channel::delayedSend()
{
	this->networkInterface().delayedSend(*this);
}

//-------------------------------------------------------------------------------------
const char * Channel::c_str() const
{
	static char dodgyString[MAX_BUF * 2] = { "None" };
	char tdodgyString[MAX_BUF] = { 0 };

	if (pEndPoint_ && !pEndPoint_->addr().isNone())
		pEndPoint_->addr().writeToString(tdodgyString, MAX_BUF);

	kbe_snprintf(dodgyString, MAX_BUF * 2, "%s/%d/%d/%d", tdodgyString, id_,
		this->condemn(), this->isDestroyed());

	return dodgyString;
}

//-------------------------------------------------------------------------------------
void Channel::clearBundle()
{
	Bundles::iterator iter = bundles_.begin();
	for(; iter != bundles_.end(); ++iter)
	{
		Bundle::reclaimPoolObject((*iter));
	}

	bundles_.clear();
}

//-------------------------------------------------------------------------------------
void Channel::handleTimeout(TimerHandle, void * arg)
{
	switch (reinterpret_cast<uintptr>(arg))
	{
		case TIMEOUT_INACTIVITY_CHECK:
		{
			if (timestamp() - lastReceivedTime_ >= inactivityExceptionPeriod_)
			{
				this->networkInterface().onChannelTimeOut(this);
			}

			break;
		}
		default:
			break;
	}
}

//-------------------------------------------------------------------------------------
void Channel::send(Bundle * pBundle)
{
	if (protocoltype_ == PROTOCOL_UDP)
	{
		sendTo(true, pBundle);
		return;
	}

	if (isDestroyed())
	{
		ERROR_MSG(fmt::format("Channel::send({}): channel has destroyed.\n", 
			this->c_str()));
		
		this->clearBundle();

		if(pBundle)
			Network::Bundle::reclaimPoolObject(pBundle);

		return;
	}

	if(condemn() > 0)
	{
		//WARNING_MSG(fmt::format("Channel::send: error, reason={}, from {}.\n", reasonToString(REASON_CHANNEL_CONDEMN), 
		//	c_str()));

		// this->clearBundle();

		if(pBundle)
			Network::Bundle::reclaimPoolObject(pBundle);

		return;
	}

	if(pBundle)
	{
		pBundle->pChannel(this);
		pBundle->finiMessage(true);
		bundles_.push_back(pBundle);
	}
	
	uint32 bundleSize = (uint32)bundles_.size();
	if(bundleSize == 0)
		return;

	if(!sending())
	{
		if(pPacketSender_ == NULL)
			pPacketSender_ = new TCPPacketSender(*pEndPoint_, *pNetworkInterface_);

		pPacketSender_->processSend(this);

		// 如果不能立即发送到系统缓冲区，那么交给poller处理
		if(bundles_.size() > 0 && condemn() == 0 && !isDestroyed())
		{
			flags_ |= FLAG_SENDING;
			pNetworkInterface_->dispatcher().registerWriteFileDescriptor(*pEndPoint_, pPacketSender_);
		}
	}

	if(this->isExternal())
	{
		if (Network::g_sendWindowMessagesOverflowCritical > 0 && bundleSize > Network::g_sendWindowMessagesOverflowCritical)
		{
			WARNING_MSG(fmt::format("Channel::send[{:p}]: external channel({}), send-window bufferedMessages has overflowed({} > {}).\n",
				(void*)this, this->c_str(), bundleSize, Network::g_sendWindowMessagesOverflowCritical));

			if (Network::g_extSendWindowMessagesOverflow > 0 &&
				bundleSize >  Network::g_extSendWindowMessagesOverflow)
			{
				ERROR_MSG(fmt::format("Channel::send[{:p}]: external channel({}), send-window bufferedMessages has overflowed({} > {}), Try adjusting the kbengine[_defs].xml->windowOverflow->send->messages.\n",
					(void*)this, this->c_str(), bundleSize, Network::g_extSendWindowMessagesOverflow));

				this->condemn("Channel::send: send-window bufferedMessages has overflowed!");
			}
		}

		if (g_extSendWindowBytesOverflow > 0)
		{
			uint32 bundleBytes = bundlesLength();
			if(bundleBytes >= g_extSendWindowBytesOverflow)
			{
				ERROR_MSG(fmt::format("Channel::send[{:p}]: external channel({}), bufferedBytes has overflowed({} > {}), Try adjusting the kbengine[_defs].xml->windowOverflow->send->bytes.\n",
					(void*)this, this->c_str(), bundleBytes, g_extSendWindowBytesOverflow));

				this->condemn("Channel::send: send-window bufferedBytes has overflowed!");
			}
		}
	}
	else
	{
		if (Network::g_sendWindowMessagesOverflowCritical > 0 && bundleSize > Network::g_sendWindowMessagesOverflowCritical)
		{
			if (Network::g_intSendWindowMessagesOverflow > 0 &&
				bundleSize > Network::g_intSendWindowMessagesOverflow)
			{
				ERROR_MSG(fmt::format("Channel::send[{:p}]: internal channel({}), send-window bufferedMessages has overflowed({} > {}).\n",
					(void*)this, this->c_str(), bundleSize, Network::g_intSendWindowMessagesOverflow));

				this->condemn("Channel::send: send-window bufferedMessages has overflowed!");
			}
			else
			{
				WARNING_MSG(fmt::format("Channel::send[{:p}]: internal channel({}), send-window bufferedMessages has overflowed({} > {}).\n",
					(void*)this, this->c_str(), bundleSize, Network::g_sendWindowMessagesOverflowCritical));
			}
		}

		if (g_intSendWindowBytesOverflow > 0)
		{
			uint32 bundleBytes = bundlesLength();
			if (bundleBytes >= g_intSendWindowBytesOverflow)
			{
				WARNING_MSG(fmt::format("Channel::send[{:p}]: internal channel({}), bufferedBytes has overflowed({} > {}).\n",
					(void*)this, this->c_str(), bundleBytes, g_intSendWindowBytesOverflow));
			}
		}
	}
}

//-------------------------------------------------------------------------------------
void Channel::sendTo(bool reliable, Bundle* pBundle)
{
	if (protocoltype_ != PROTOCOL_UDP || isDestroyed() || condemn() > 0)
	{
		if (pBundle)
			Bundle::reclaimPoolObject(pBundle);
		return;
	}

	if (pBundle)
	{
		pBundle->pChannel(this);
		pBundle->finiMessage(true);
		bundles_.push_back(pBundle);
	}

	if (bundles_.empty())
		return;

	if (!pPacketSender_)
	{
		pPacketSender_ = protocolSubtype_ == SUB_PROTOCOL_KCP
			? static_cast<PacketSender*>(new KCPPacketSender(*pEndPoint_, *pNetworkInterface_))
			: static_cast<PacketSender*>(new UDPPacketSender(*pEndPoint_, *pNetworkInterface_));
	}

	// KCP sender 的 userarg=1 表示写入可靠队列；普通 UDP 以及 KCP 控制报文直接走 socket。
	// KCP sender userarg=1 means enqueue reliably; plain UDP and KCP control datagrams go directly to the socket.
	pPacketSender_->processSend(this, reliable && protocolSubtype_ == SUB_PROTOCOL_KCP ? 1 : 0);
}

//-------------------------------------------------------------------------------------
void Channel::stopSend()
{
	if(!sending())
		return;

	flags_ &= ~FLAG_SENDING;

	pNetworkInterface_->dispatcher().deregisterWriteFileDescriptor(*pEndPoint_);
}

//-------------------------------------------------------------------------------------
void Channel::onSendCompleted()
{
	KBE_ASSERT(bundles_.size() == 0 && sending());

	EventPoller* pPoller = pNetworkInterface_->dispatcher().pPoller();
	if (pPoller != NULL && pPoller->supportsCompletion() && pPoller->hasPendingSend(*pEndPoint_))
	{
		// Bundle 已经交给 completion poller 并不代表 TCP 字节已经完成发送。
		// 保持写 handler 和 FLAG_SENDING，等 outstanding/queued 字节真正排空后，poller 会再次触发完成通知。
		// Handing Bundles to a completion poller does not mean their TCP bytes have completed.
		// Keep the write handler and FLAG_SENDING until the poller drains outstanding and queued bytes and notifies us again.
		return;
	}

	stopSend();
}

//-------------------------------------------------------------------------------------
void Channel::onPacketSent(int bytes, bool sentCompleted)
{
	prepareTickCounters();

	if(sentCompleted)
	{
		++numPacketsSent_;
		++g_numPacketsSent;
	}

	if (bytes > 0)
	{
		numBytesSent_ += bytes;
		g_numBytesSent += bytes;
		lastTickBytesSent_ += bytes;
	}

	if(this->isExternal())
	{
		if(g_extSentWindowBytesOverflow > 0 && 
			lastTickBytesSent_ >= g_extSentWindowBytesOverflow)
		{
			ERROR_MSG(fmt::format("Channel::onPacketSent[{:p}]: external channel({}), sentBytes has overflowed({} > {}), Try adjusting the kbengine[_defs].xml->windowOverflow->send->tickSentBytes.\n", 
				(void*)this, this->c_str(), lastTickBytesSent_, g_extSentWindowBytesOverflow));

			this->condemn("Channel::onPacketSent: sentBytes has overflowed!");
		}
	}
	else
	{
		if(g_intSentWindowBytesOverflow > 0 && 
			lastTickBytesSent_ >= g_intSentWindowBytesOverflow)
		{
			WARNING_MSG(fmt::format("Channel::onPacketSent[{:p}]: internal channel({}), sentBytes has overflowed({} > {}).\n", 
				(void*)this, this->c_str(), lastTickBytesSent_, g_intSentWindowBytesOverflow));
		}
	}
}

//-------------------------------------------------------------------------------------
void Channel::onPacketReceived(int bytes)
{
	prepareTickCounters();

	lastReceivedTime_ = timestamp();
	++numPacketsReceived_;
	++g_numPacketsReceived;

	if (bytes > 0)
	{
		numBytesReceived_ += bytes;
		lastTickBytesReceived_ += bytes;
		g_numBytesReceived += bytes;
	}

	if(this->isExternal())
	{
		if(g_extReceiveWindowBytesOverflow > 0 && 
			lastTickBytesReceived_ >= g_extReceiveWindowBytesOverflow)
		{
			ERROR_MSG(fmt::format("Channel::onPacketReceived[{:p}]: external channel({}), bufferedBytes has overflowed({} > {}), Try adjusting the kbengine[_defs].xml->windowOverflow->receive.\n", 
				(void*)this, this->c_str(), lastTickBytesReceived_, g_extReceiveWindowBytesOverflow));

			this->condemn("Channel::onPacketReceived: bufferedBytes has overflowed!");
		}
	}
	else
	{
		if(g_intReceiveWindowBytesOverflow > 0 && 
			lastTickBytesReceived_ >= g_intReceiveWindowBytesOverflow)
		{
			WARNING_MSG(fmt::format("Channel::onPacketReceived[{:p}]: internal channel({}), bufferedBytes has overflowed({} > {}).\n", 
				(void*)this, this->c_str(), lastTickBytesReceived_, g_intReceiveWindowBytesOverflow));
		}
	}
}

//-------------------------------------------------------------------------------------
void Channel::addReceiveWindow(Packet* pPacket)
{
	prepareTickCounters();

	++lastTickBufferedReceives_;

	if(Network::g_receiveWindowMessagesOverflowCritical > 0 && lastTickBufferedReceives_ > Network::g_receiveWindowMessagesOverflowCritical)
	{
		if(this->isExternal())
		{
			if(Network::g_extReceiveWindowMessagesOverflow > 0 && 
				lastTickBufferedReceives_ > Network::g_extReceiveWindowMessagesOverflow)
			{
				ERROR_MSG(fmt::format("Channel::addReceiveWindow[{:p}]: external channel({}), receive window has overflowed({} > {}), Try adjusting the kbengine[_defs].xml->windowOverflow->receive->messages->external.\n", 
					(void*)this, this->c_str(), lastTickBufferedReceives_, Network::g_extReceiveWindowMessagesOverflow));

				this->condemn("Channel::addReceiveWindow: receive window has overflowed!");
			}
			else
			{
				WARNING_MSG(fmt::format("Channel::addReceiveWindow[{:p}]: external channel({}), receive window has overflowed({} > {}).\n", 
					(void*)this, this->c_str(), lastTickBufferedReceives_, Network::g_receiveWindowMessagesOverflowCritical));
			}
		}
		else
		{
			if(Network::g_intReceiveWindowMessagesOverflow > 0 && 
				lastTickBufferedReceives_ > Network::g_intReceiveWindowMessagesOverflow)
			{
				WARNING_MSG(fmt::format("Channel::addReceiveWindow[{:p}]: internal channel({}), receive window has overflowed({} > {}).\n", 
					(void*)this, this->c_str(), lastTickBufferedReceives_, Network::g_intReceiveWindowMessagesOverflow));
			}
		}
	}

	KBE_ASSERT(KBEngine::Network::MessageHandlers::pMainMessageHandlers);

	{
		AUTO_SCOPED_PROFILE("processRecvMessages");
		processPackets(KBEngine::Network::MessageHandlers::pMainMessageHandlers, pPacket);
	}
}

//-------------------------------------------------------------------------------------
void Channel::condemn(const std::string& reason, bool waitSendCompletedDestroy)
{ 
	if(condemnReason_.size() == 0)
		condemnReason_ = reason;

	flags_ |= (waitSendCompletedDestroy ? FLAG_CONDEMN_AND_WAIT_DESTROY : FLAG_CONDEMN);
	if (pNetworkInterface_)
		pNetworkInterface_->requestChannelMaintenance(this);

	if (waitSendCompletedDestroy && gracefulCloseDeadline_ == 0)
		gracefulCloseDeadline_ = timestamp() + GRACEFUL_CLOSE_TIMEOUT_STAMPS;
}

//-------------------------------------------------------------------------------------
bool Channel::handshake(Packet* pPacket)
{
	if(hasHandshake())
		return false;

	if (protocolSubtype_ == SUB_PROTOCOL_KCP)
	{
		const size_t helloLength = std::strlen(UDP_HELLO);
		const bool validHello = pPacket->length() == helloLength &&
			std::memcmp(pPacket->data() + pPacket->rpos(), UDP_HELLO, helloLength) == 0;
		pPacket->clear(false);

		if (!validHello)
		{
			// 断线探测期间可能有迟到的 KCP 数据报，丢弃它但保留该远端重新握手的机会。
			// Late KCP datagrams may arrive during disconnect detection; discard them while allowing the peer to retry the handshake.
			return true;
		}

		UDPPacket* pAckPacket = UDPPacket::createPoolObject(OBJECTPOOL_POINT);
		(*pAckPacket) << UDP_HELLO_ACK << KBEVersion::versionString() << static_cast<uint32>(id());

		bool sent = false;
		EventPoller* pPoller = this->networkInterface().dispatcher().pPoller();
		if (pPoller != NULL && pPoller->supportsCompletion())
		{
			// ACK 必须进入 completion UDP 队列；KBESOCKET 在 Win64 上不能缩窄为 int。
			// The ACK must use the completion UDP queue, and KBESOCKET must not be narrowed to int on Win64.
			sent = pPoller->queueUdpSend(static_cast<KBESOCKET>(*pEndPoint_), pAckPacket->data(),
				static_cast<int>(pAckPacket->length()), pEndPoint_->addr());
		}
		else
		{
			sent = pEndPoint_->sendto(pAckPacket->data(), static_cast<int>(pAckPacket->length())) >= 0;
		}

		UDPPacket::reclaimPoolObject(pAckPacket);
		if (!sent)
			return true;

		if (!pPacketReader_ || pPacketReader_->type() != PacketReader::PACKET_READER_TYPE_KCP)
		{
			SAFE_RELEASE(pPacketReader_);
			pPacketReader_ = new KCPPacketReader(this);
		}

		flags_ |= FLAG_HANDSHAKE;
		DEBUG_MSG(fmt::format("Channel::handshake: kcp({}) successfully!\n", this->c_str()));
		return true;
	}

	// https/wss
	if (!pEndPoint_->isSSL())
	{
		const size_t packetLength = pPacket->length();
		const bool mayBeTLS = !tlsDetectionPrefix_.empty() ||
			(packetLength > 0 && static_cast<uint8>(pPacket->data()[pPacket->rpos()]) == 0x16 && packetLength < 3);
		if (mayBeTLS)
		{
			// 判定前最多只保留两个字节；第三字节到达后立即重建当前 packet 并进入正常协议探测。
			// Keep at most two bytes before classification; rebuild the current packet as soon as byte three arrives and resume normal probing.
			tlsDetectionPrefix_.insert(tlsDetectionPrefix_.end(),
				pPacket->data() + pPacket->rpos(), pPacket->data() + pPacket->wpos());
			pPacket->read_skip(packetLength);
			if (tlsDetectionPrefix_.size() < 3)
				return true;

			pPacket->clear(false);
			pPacket->append(tlsDetectionPrefix_.data(), tlsDetectionPrefix_.size());
			tlsDetectionPrefix_.clear();
		}

		int sslVersion = KB_SSL::isSSLProtocal(pPacket);
		if (sslVersion != -1)
		{
			EventPoller* pPoller = this->dispatcher().pPoller();
			const bool useMemoryBIO = pPoller != NULL && pPoller->supportsCompletion();
			if (!pEndPoint_->setupSSL(sslVersion, pPacket, useMemoryBIO) || !flushSSLNetworkOutput())
			{
				// TLS 状态一旦推进就不能把原 ClientHello 当明文重试；失败连接必须确定性关闭。
				// Once TLS state advances, the original ClientHello cannot be retried as plaintext; fail the connection deterministically.
				this->condemn("TLS handshake failed");
				return true;
			}

			if (pPacket->length() == 0)
				return true;
		}
	}
	else
	{
		// 如果开启了ssl通讯，因目前只支持wss，所以必须等待websocket握手成功才算通过
		if (!websocket::WebSocketProtocol::isWebSocketProtocol(pPacket))
			return true;
	}

	flags_ |= FLAG_HANDSHAKE;
	// 此处判定是否为websocket或者其他协议的握手
	if(websocket::WebSocketProtocol::isWebSocketProtocol(pPacket))
	{
		channelType_ = CHANNEL_WEB;
		if(websocket::WebSocketProtocol::handshake(this, pPacket))
		{
			if(!pPacketReader_ || pPacketReader_->type() != PacketReader::PACKET_READER_TYPE_WEBSOCKET)
			{
				SAFE_RELEASE(pPacketReader_);
				pPacketReader_ = new WebSocketPacketReader(this);
			}

			pFilter_ = new WebSocketPacketFilter(this);
			DEBUG_MSG(fmt::format("Channel::handshake: websocket({}) successfully!\n", this->c_str()));

			// 无论如何都返回true，直到握手成功
			return true;
		}
		else
		{
			DEBUG_MSG(fmt::format("Channel::handshake: websocket({}) error!\n", this->c_str()));
		}
	}

	if(!pPacketReader_ || pPacketReader_->type() != PacketReader::PACKET_READER_TYPE_SOCKET)
	{
		SAFE_RELEASE(pPacketReader_);
		pPacketReader_ = new PacketReader(this);
	}

	return false;
}

//-------------------------------------------------------------------------------------
bool Channel::flushSSLNetworkOutput()
{
	if (!pEndPoint_ || !pEndPoint_->usesSSLMemoryBIO())
		return true;

	std::vector<char> output;
	if (!pEndPoint_->takeSSLNetworkOutput(output))
		return true;

	EventPoller* pPoller = this->dispatcher().pPoller();
	if (!pPoller || !pPoller->supportsCompletion())
	{
		ERROR_MSG("Channel::flushSSLNetworkOutput: memory BIO requires a completion poller.\n");
		return false;
	}

	// OpenSSL 已推进 record 序列号，密文入队失败后不能重新 SSL_write 同一明文，否则双方状态会分叉。
	// OpenSSL has advanced its record sequence; after enqueue failure the same plaintext cannot be SSL_write again without desynchronizing peers.
	if (!pPoller->queueTcpSend(*pEndPoint_, output.data(), static_cast<int>(output.size())))
	{
		ERROR_MSG(fmt::format("Channel::flushSSLNetworkOutput: failed to queue {} TLS bytes for {}.\n",
			output.size(), this->c_str()));
		return false;
	}

	return true;
}

//-------------------------------------------------------------------------------------
bool Channel::sendRawNetworkData(const void* data, int length)
{
	if (!pEndPoint_ || length < 0)
		return false;

	if (pEndPoint_->usesSSLMemoryBIO())
		return pEndPoint_->encryptSSLNetworkData(data, length) && flushSSLNetworkOutput();

	EventPoller* pPoller = this->dispatcher().pPoller();
	if (pPoller && pPoller->supportsCompletion())
		return pPoller->queueTcpSend(*pEndPoint_, data, length);

	const char* bytes = static_cast<const char*>(data);
	int sent = 0;
	while (sent < length)
	{
		const int result = pEndPoint_->send(bytes + sent, length - sent);
		if (result <= 0)
			return false;

		sent += result;
	}

	return true;
}

//-------------------------------------------------------------------------------------
bool Channel::sendWebSocketClose(const void* payload, size_t length)
{
	if (webSocketCloseSent_)
		return true;

	TCPPacket* pPayload = TCPPacket::createPoolObject(OBJECTPOOL_POINT);
	TCPPacket* pFrame = TCPPacket::createPoolObject(OBJECTPOOL_POINT);
	if (length > 0)
		pPayload->append(static_cast<const uint8*>(payload), length);

	websocket::WebSocketProtocol::makeFrame(websocket::WebSocketProtocol::CLOSE_FRAME, pPayload, pFrame);
	if (length > 0)
		pFrame->append(static_cast<const uint8*>(payload), length);

	const bool result = sendRawNetworkData(pFrame->data() + pFrame->rpos(), static_cast<int>(pFrame->length()));
	TCPPacket::reclaimPoolObject(pFrame);
	TCPPacket::reclaimPoolObject(pPayload);
	webSocketCloseSent_ = result;
	return result;
}

//-------------------------------------------------------------------------------------
bool Channel::startGracefulClose(const void* closePayload, size_t closePayloadLength, bool peerWebSocketClose)
{
	if (gracefulCloseDeadline_ == 0)
		gracefulCloseDeadline_ = timestamp() + GRACEFUL_CLOSE_TIMEOUT_STAMPS;

	if (peerWebSocketClose)
	{
		webSocketCloseReceived_ = true;
		// 收到 close 后不能再发送排队的应用数据；已经进入 poller 的字节仍保持在 close 之前完成。
		// No queued application data may follow a received close; bytes already owned by the poller still finish before the close frame.
		clearBundle();
	}

	if (channelType_ == CHANNEL_WEB && !sendWebSocketClose(closePayload, closePayloadLength))
		return false;

	gracefulCloseStarted_ = true;
	condemn("graceful protocol close", true);
	return true;
}

//-------------------------------------------------------------------------------------
bool Channel::handleWebSocketClose(const void* payload, size_t length)
{
	if (gracefulCloseStarted_ && webSocketCloseSent_)
	{
		webSocketCloseReceived_ = true;
		return true;
	}

	return startGracefulClose(payload, length, true);
}

//-------------------------------------------------------------------------------------
bool Channel::handleWebSocketCloseError(uint16 closeCode)
{
	const uint8 payload[2] = {
		static_cast<uint8>((closeCode >> 8) & 0xFF),
		static_cast<uint8>(closeCode & 0xFF)
	};
	return startGracefulClose(payload, sizeof(payload), false);
}

//-------------------------------------------------------------------------------------
void Channel::handleTLSCloseNotify()
{
	tlsCloseNotifyReceived_ = true;
	if (gracefulCloseStarted_)
		return;

	// TLS 已经禁止继续发送应用数据，此时不能再补 WebSocket frame，只回送 close_notify。
	// TLS no longer permits application data here, so skip any WebSocket frame and only reply with close_notify.
	webSocketCloseSent_ = true;
	webSocketCloseReceived_ = true;
	clearBundle();
	gracefulCloseStarted_ = true;
	condemn("TLS close_notify", true);
	if (!pEndPoint_->shutdownSSL() || !flushSSLNetworkOutput())
		condemn("TLS close_notify response failed");
}

//-------------------------------------------------------------------------------------
bool Channel::hasPendingNetworkSend() const
{
	if (!pEndPoint_)
		return false;

	EventPoller* pPoller = pNetworkInterface_->dispatcher().pPoller();
	return sending() || !bundles_.empty() ||
		(pPoller && pPoller->supportsCompletion() && pPoller->hasPendingSend(*pEndPoint_));
}

//-------------------------------------------------------------------------------------
bool Channel::processGracefulClose()
{
	if (condemn() != FLAG_CONDEMN_AND_WAIT_DESTROY)
		return true;

	if (gracefulCloseDeadline_ > 0 && timestamp() >= gracefulCloseDeadline_)
	{
		WARNING_MSG(fmt::format("Channel::processGracefulClose: timeout waiting for peer or pending send, channel={}.\n", c_str()));
		return true;
	}

	if (!gracefulCloseStarted_)
	{
		if (hasPendingNetworkSend())
			return false;

		const uint8 normalClose[2] = { 0x03, 0xE8 };
		if (!startGracefulClose(channelType_ == CHANNEL_WEB ? normalClose : NULL,
			channelType_ == CHANNEL_WEB ? sizeof(normalClose) : 0, false))
		{
			return true;
		}
	}

	if (channelType_ == CHANNEL_WEB && !webSocketCloseReceived_)
		return false;

	// WebSocket 关闭帧属于 TLS 应用数据，必须先完成双向 WebSocket 握手才能发送 close_notify。
	// A WebSocket close frame is TLS application data, so finish the bidirectional WebSocket handshake before sending close_notify.
	// Readiness TLS 可能在前一 tick 返回 WANT_WRITE，重复推进不会重放 close_notify。
	// Readiness TLS may have returned WANT_WRITE on the prior tick; advancing again does not replay close_notify.
	if (pEndPoint_ && pEndPoint_->isSSL() && (!pEndPoint_->shutdownSSL() || !flushSSLNetworkOutput()))
		return true;

	if (hasPendingNetworkSend())
		return false;

	if (pEndPoint_ && pEndPoint_->isSSL() && !tlsCloseNotifyReceived_)
		return false;

	return true;
}

//-------------------------------------------------------------------------------------
void Channel::updateTick(KBEngine::Network::MessageHandlers* pMsgHandlers)
{
	(void)pMsgHandlers;
	lastTickBytesReceived_ = 0;
	lastTickBytesSent_ = 0;
	lastTickBufferedReceives_ = 0;
	lastTickEpoch_ = pNetworkInterface_ ? pNetworkInterface_->channelTickEpoch() : 0;
}

//-------------------------------------------------------------------------------------
void Channel::prepareTickCounters()
{
	if (pNetworkInterface_ == NULL)
		return;

	const uint64 currentEpoch = pNetworkInterface_->channelTickEpoch();
	if (lastTickEpoch_ == currentEpoch)
		return;

	// 只在本 Tick 首次活动时写 Channel cache line，空闲连接不再被主循环触碰。
	// Touch the Channel cache line only on its first activity in this tick; idle connections remain untouched by the main loop.
	lastTickBytesReceived_ = 0;
	lastTickBytesSent_ = 0;
	lastTickBufferedReceives_ = 0;
	lastTickEpoch_ = currentEpoch;
}

//-------------------------------------------------------------------------------------
void Channel::processPackets(KBEngine::Network::MessageHandlers* pMsgHandlers, Packet* pPacket)
{
	if(pMsgHandlers_ != NULL)
	{
		pMsgHandlers = pMsgHandlers_;
	}

	if (this->isDestroyed())
	{
		ERROR_MSG(fmt::format("Channel::processPackets({}): channel[{:p}] is destroyed.\n", 
			this->c_str(), (void*)this));

		return;
	}

	if(this->condemn() > 0 && !isGracefulClosing())
	{
		ERROR_MSG(fmt::format("Channel::processPackets({}): channel[{:p}] is condemn.\n", 
			this->c_str(), (void*)this));

		//this->destroy();
		return;
	}
	
	if (!hasHandshake())
	{
		if (handshake(pPacket))
		{
			RECLAIM_PACKET(pPacket->isTCPPacket(), pPacket);
			return;
		}
	}

	try
	{
		pPacketReader_->processMessages(pMsgHandlers, pPacket);
	}
	catch(MemoryStreamException &)
	{
		Network::MessageHandler* pMsgHandler = pMsgHandlers->find(pPacketReader_->currMsgID());
		WARNING_MSG(fmt::format("Channel::processPackets({}): packet invalid. currMsg=({}, id={}, len={}), currMsgLen={}\n",
			this->c_str()
			, (pMsgHandler == NULL ? "unknown" : pMsgHandler->name) 
			, pPacketReader_->currMsgID() 
			, (pMsgHandler == NULL ? -1 : pMsgHandler->msgLen) 
			, pPacketReader_->currMsgLen()));

		pPacketReader_->currMsgID(0);
		pPacketReader_->currMsgLen(0);
		condemn("Channel::processPackets: packet invalid!");
	}

	RECLAIM_PACKET(pPacket->isTCPPacket(), pPacket);
}

//-------------------------------------------------------------------------------------
bool Channel::waitSend()
{
	return pEndPoint()->waitSend();
}

//-------------------------------------------------------------------------------------
EventDispatcher & Channel::dispatcher()
{
	return pNetworkInterface_->dispatcher();
}

//-------------------------------------------------------------------------------------
Bundle* Channel::createSendBundle()
{
	if(bundles_.size() > 0)
	{
		Bundle* pBundle = bundles_.back();
		Bundle::Packets& packets = pBundle->packets();

		// pBundle和packets[0]都必须是没有被对象池回收的对象
		// 必须是未经过加密的包，如果已经加密了就不要再重复拿出来用了，否则外部容易向其中添加未加密数据 
		if (pBundle->packetHaveSpace() &&
			!packets[0]->encrypted())
		{
			// 先从队列删除
			bundles_.pop_back();
			pBundle->pChannel(this);
			pBundle->pCurrMsgHandler(NULL);
			pBundle->currMsgPacketCount(0);
			pBundle->currMsgLength(0);
			pBundle->currMsgLengthPos(0);
			if (!pBundle->pCurrPacket())
			{
				Packet* pPacket = pBundle->packets().back();
				pBundle->packets().pop_back();
				pBundle->pCurrPacket(pPacket);
			}

			return pBundle;
		}
	}
	
	Bundle* pBundle = Bundle::createPoolObject(OBJECTPOOL_POINT);
	pBundle->pChannel(this);
	return pBundle;
}

//-------------------------------------------------------------------------------------

}
}
