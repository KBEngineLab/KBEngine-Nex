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
#include "bots.h"
#include "clientobject.h"
#include "network/common.h"
#include "network/message_handler.h"
#include "network/tcp_packet.h"
#include "network/bundle.h"
#include "network/fixed_messages.h"
#include "thread/threadpool.h"
#include "server/components.h"
#include "server/serverconfig.h"
#include "entitydef/scriptdef_module.h"
#include "entitydef/entitydef.h"
#include "client_lib/client_interface.h"
#include "common/kbeversion.h"

#include <cstring>

#include "baseapp/baseapp_interface.h"
#include "cellapp/cellapp_interface.h"
#include "baseappmgr/baseappmgr_interface.h"
#include "cellappmgr/cellappmgr_interface.h"
#include "loginapp/loginapp_interface.h"


namespace KBEngine{

namespace
{
const uint64 KCP_HANDSHAKE_TIMEOUT = 30 * stampsPerSecond();
const uint64 KCP_HELLO_RETRY_INTERVAL = stampsPerSecond();
const int KCP_MAX_ACKS_PER_TICK = 4;

bool isTransientSocketError(int errorCode)
{
#if KBE_PLATFORM == PLATFORM_WIN32
	return errorCode == WSAEWOULDBLOCK || errorCode == WSAEINTR;
#else
	return errorCode == EAGAIN || errorCode == EWOULDBLOCK || errorCode == EINTR;
#endif
}

int lastSocketError()
{
#if KBE_PLATFORM == PLATFORM_WIN32
	return WSAGetLastError();
#else
	return errno;
#endif
}

bool parseKcpHelloAck(const char* data, size_t length, uint32& channelID)
{
	const size_t ackLength = std::strlen(Network::UDP_HELLO_ACK);
	const std::string& version = KBEVersion::versionString();
	const size_t expectedLength = ackLength + 1 + version.length() + 1 + sizeof(uint32);
	if (length != expectedLength ||
		std::memcmp(data, Network::UDP_HELLO_ACK, ackLength) != 0 || data[ackLength] != '\0')
	{
		return false;
	}

	const size_t versionOffset = ackLength + 1;
	if (version.empty() || std::memcmp(data + versionOffset, version.data(), version.length()) != 0 ||
		data[versionOffset + version.length()] != '\0')
	{
		return false;
	}

	const unsigned char* encodedID = reinterpret_cast<const unsigned char*>(data + expectedLength - sizeof(uint32));
	channelID = static_cast<uint32>(encodedID[0]) |
		(static_cast<uint32>(encodedID[1]) << 8) |
		(static_cast<uint32>(encodedID[2]) << 16) |
		(static_cast<uint32>(encodedID[3]) << 24);
	return channelID != 0;
}
}

SCRIPT_METHOD_DECLARE_BEGIN(ClientObject)
SCRIPT_METHOD_DECLARE_END()

SCRIPT_MEMBER_DECLARE_BEGIN(ClientObject)
SCRIPT_MEMBER_DECLARE_END()

SCRIPT_GETSET_DECLARE_BEGIN(ClientObject)
SCRIPT_GETSET_DECLARE_END()
SCRIPT_INIT(ClientObject, 0, 0, 0, 0, 0)		

//-------------------------------------------------------------------------------------
ClientObject::ClientObject(std::string name, Network::NetworkInterface& ninterface):
ClientObjectBase(ninterface, getScriptType()),
error_(C_ERROR_NONE),
state_(C_STATE_INIT),
pBlowfishFilter_(0),
pTCPPacketSenderEx_(NULL),
pTCPPacketReceiverEx_(NULL),
pKCPPacketSenderEx_(NULL),
pKCPPacketReceiverEx_(NULL),
kcpHandshakeStartTime_(0),
kcpHelloSentTime_(0),
kcpHelloAttempts_(0)
{
	name_ = name;
	typeClient_ = CLIENT_TYPE_BOTS;
	clientDatas_ = "bots";
	password_ = ServerConfig::getSingleton().getBots().bots_account_passwd;
}

//-------------------------------------------------------------------------------------
ClientObject::~ClientObject()
{
	SAFE_RELEASE(pBlowfishFilter_);
}

//-------------------------------------------------------------------------------------		
void ClientObject::finalise(void)
{
	reset();
	ClientObjectBase::finalise();
}

//-------------------------------------------------------------------------------------		
void ClientObject::reset(void)
{
	clearStates();

	std::string name = name_;
	std::string passwd = password_;
	ClientObjectBase::reset();
	
	name_ = name;
	password_ = passwd;
	clientDatas_ = "bots";
	state_ = C_STATE_INIT;
	connectedBaseapp_ = false;
}

//-------------------------------------------------------------------------------------
void ClientObject::deregisterReceiverEndPoint(Network::PacketReceiver* pPacketReceiver)
{
	if (pPacketReceiver != NULL && pPacketReceiver->pEndPoint() != NULL && pPacketReceiver->pEndPoint()->good())
	{
		Bots::getSingleton().networkInterface().dispatcher().deregisterReadFileDescriptor(*pPacketReceiver->pEndPoint());
	}
}

//-------------------------------------------------------------------------------------
bool ClientObject::isKcpTransport() const
{
	return connectedBaseapp_ && pServerChannel_ != NULL && pServerChannel_->pEndPoint() != NULL &&
		pServerChannel_->protocoltype() == Network::PROTOCOL_UDP &&
		pServerChannel_->protocolSubtype() == Network::SUB_PROTOCOL_KCP;
}

//-------------------------------------------------------------------------------------
bool ClientObject::isTcpTransport() const
{
	return connectedBaseapp_ && pServerChannel_ != NULL && pServerChannel_->pEndPoint() != NULL &&
		pServerChannel_->protocoltype() == Network::PROTOCOL_TCP;
}

//-------------------------------------------------------------------------------------
void ClientObject::sendBaseappActiveTick(bool force)
{
	if (!connectedBaseapp_ || pServerChannel_ == NULL || pServerChannel_->pEndPoint() == NULL ||
		pServerChannel_->isDestroyed() || pServerChannel_->condemn() > 0)
	{
		return;
	}

	uint64 interval = stampsPerSecond() * 10;
	if (Network::g_channelExternalTimeout > 0.f)
	{
		// 心跳最多使用外部超时的四分之一，并限制在 1 至 10 秒，给拥塞和调度抖动留出恢复余量。
		// Cap heartbeats at one quarter of the external timeout and between 1-10 seconds, leaving recovery margin for congestion and scheduler jitter.
		interval = KBE_MAX<uint64>(stampsPerSecond(),
			static_cast<uint64>(Network::g_channelExternalTimeout * stampsPerSecond()) / 4);
		interval = KBE_MIN<uint64>(interval, stampsPerSecond() * 10);
	}

	const uint64 now = timestamp();
	if (!force && now - lastSentActiveTickTime_ < interval)
		return;

	lastSentActiveTickTime_ = now;
	Network::Bundle* pBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
	(*pBundle).newMessage(BaseappInterface::onClientActiveTick);
	pServerChannel_->send(pBundle);
}

//-------------------------------------------------------------------------------------
void ClientObject::clearStates(void)
{
	if (pServerChannel_ == NULL)
		return;

	deregisterReceiverEndPoint(pTCPPacketReceiverEx_);
	deregisterReceiverEndPoint(pKCPPacketReceiverEx_);

	pServerChannel_->stopSend();
	pServerChannel_->stopInactivityDetection();
	pServerChannel_->pPacketSender(NULL);
	pServerChannel_->pPacketReceiver(NULL);
	pServerChannel_->resetTransport();

	SAFE_RELEASE(pTCPPacketSenderEx_);
	SAFE_RELEASE(pTCPPacketReceiverEx_);
	SAFE_RELEASE(pKCPPacketSenderEx_);
	SAFE_RELEASE(pKCPPacketReceiverEx_);

	if (pServerChannel_->pEndPoint())
	{
		pServerChannel_->pEndPoint()->destroySSL();
		pServerChannel_->pEndPoint()->close();
		pServerChannel_->pEndPoint(NULL);
	}

	kcpHandshakeStartTime_ = 0;
	kcpHelloSentTime_ = 0;
	kcpHelloAttempts_ = 0;
}

//-------------------------------------------------------------------------------------
void ClientObject::onNetworkError(const std::string& err)
{
	if (isDestroyed())
		return;

	WARNING_MSG(fmt::format("ClientObject::onNetworkError: name={}, state={}, error={}\n",
		name_, static_cast<int>(state_), err));
	Bots::getSingleton().onClientNetworkError();
	destroy();
}

//-------------------------------------------------------------------------------------
bool ClientObject::initCreate()
{
	clearStates();

	Network::EndPoint* pEndpoint = Network::EndPoint::createPoolObject(OBJECTPOOL_POINT);
	
	pEndpoint->socket(SOCK_STREAM);
	if (!pEndpoint->good())
	{
		ERROR_MSG("ClientObject::initNetwork: couldn't create a socket\n");
		Network::EndPoint::reclaimPoolObject(pEndpoint);
		error_ = C_ERROR_INIT_NETWORK_FAILED;
		return false;
	}
	
	ENGINE_COMPONENT_INFO& infos = g_kbeSrvConfig.getBots();
	if (infos.login_port_max > infos.login_port_min)
	{
		infos.login_port = infos.login_port_min + (rand() % (infos.login_port_max - infos.login_port_min + 1));
	}

	u_int32_t address;

	Network::Address::string2ip(infos.login_ip, address);
	if(pEndpoint->connect(htons(infos.login_port), address) == -1)
	{
		ERROR_MSG(fmt::format("ClientObject::initNetwork({1}): connect server({2}:{3}) error({0})!\n",
			kbe_strerror(), name_, infos.login_ip, infos.login_port));

		Network::EndPoint::reclaimPoolObject(pEndpoint);
		// error_ = C_ERROR_INIT_NETWORK_FAILED;
		state_ = C_STATE_INIT;
		return false;
	}

	Network::Address addr(infos.login_ip, infos.login_port);
	pEndpoint->addr(addr);

	pServerChannel_->pEndPoint(pEndpoint);
	pEndpoint->setnonblocking(true);
	pEndpoint->setnodelay(true);

	pServerChannel_->pMsgHandlers(&ClientInterface::messageHandlers);

	pTCPPacketSenderEx_ = new Network::TCPPacketSenderEx(*pEndpoint, this->networkInterface_, this);
	pTCPPacketReceiverEx_ = new Network::TCPPacketReceiverEx(*pEndpoint, this->networkInterface_, this);
	Bots::getSingleton().networkInterface().dispatcher().registerReadFileDescriptor((*pEndpoint), pTCPPacketReceiverEx_);
	
	//不在这里注册
	//Bots::getSingleton().networkInterface().dispatcher().registerWriteFileDescriptor((*pEndpoint), pTCPPacketSenderEx_);
	pServerChannel_->pPacketSender(pTCPPacketSenderEx_);

	Network::Bundle* pBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
	(*pBundle).newMessage(LoginappInterface::hello);
	(*pBundle) << KBEVersion::versionString() << KBEVersion::scriptVersionString();

	if(Network::g_channelExternalEncryptType == 1)
	{
		pBlowfishFilter_ = new Network::BlowfishFilter();
		(*pBundle).appendBlob(pBlowfishFilter_->key());
	}
	else
	{
		std::string key = "";
		(*pBundle).appendBlob(key);
	}

	pEndpoint->send(pBundle);
	Network::Bundle::reclaimPoolObject(pBundle);
	return true;
}

//-------------------------------------------------------------------------------------
bool ClientObject::initLoginBaseapp()
{
	clearStates();
	connectedBaseapp_ = false;

	if (udp_port_ > 0 && startKcpHandshake())
		return true;

	clearStates();
	return connectBaseappTcp();
}

//-------------------------------------------------------------------------------------
bool ClientObject::startKcpHandshake()
{
	u_int32_t address = 0;
	if (Network::Address::string2ip(ip_.c_str(), address) != 0)
	{
		WARNING_MSG(fmt::format("ClientObject::startKcpHandshake: invalid address, name={}, address={}\n",
			name_, ip_));
		return false;
	}

	Network::EndPoint* pEndpoint = Network::EndPoint::createPoolObject(OBJECTPOOL_POINT);
	pEndpoint->socket(SOCK_DGRAM);
	if (!pEndpoint->good())
	{
		WARNING_MSG("ClientObject::startKcpHandshake: couldn't create a UDP socket, falling back to TCP.\n");
		Network::EndPoint::reclaimPoolObject(pEndpoint);
		return false;
	}

	// UDP connect 固定远端并过滤伪造 ACK；false 避免对 UDP socket 设置 TCP_NODELAY。
	// UDP connect pins the peer and filters spoofed ACKs; false avoids applying TCP_NODELAY to a UDP socket.
	if (pEndpoint->connect(htons(udp_port_), address, false) == -1)
	{
		WARNING_MSG(fmt::format("ClientObject::startKcpHandshake: UDP connect failed, name={}, error={}\n",
			name_, lastSocketError()));
		Network::EndPoint::reclaimPoolObject(pEndpoint);
		return false;
	}

	pEndpoint->addr(Network::Address(ip_.c_str(), udp_port_));
	pEndpoint->setnonblocking(true);
	pServerChannel_->pEndPoint(pEndpoint);
	state_ = C_STATE_LOGIN_BASEAPP_KCP_HANDSHAKE;
	kcpHandshakeStartTime_ = timestamp();
	kcpHelloSentTime_ = 0;
	kcpHelloAttempts_ = 0;

	if (!sendKcpHello())
	{
		WARNING_MSG(fmt::format("ClientObject::startKcpHandshake: initial hello failed, name={}\n", name_));
		return false;
	}

	return true;
}

//-------------------------------------------------------------------------------------
bool ClientObject::sendKcpHello()
{
	Network::EndPoint* pEndpoint = pServerChannel_ != NULL ? pServerChannel_->pEndPoint() : NULL;
	if (pEndpoint == NULL)
		return false;

	++kcpHelloAttempts_;
	const int helloLength = static_cast<int>(std::strlen(Network::UDP_HELLO));
	const int sent = pEndpoint->send(Network::UDP_HELLO, helloLength);
	if (sent != helloLength)
	{
		const int errorCode = sent < 0 ? lastSocketError() : 0;
		if (sent < 0 && isTransientSocketError(errorCode))
		{
			// 瞬态背压也算一次发送尝试，避免在每个 Tick 忙重试并放大网络与 CPU 压力。
			// Transient backpressure still counts as an attempt to avoid busy retries on every Tick and amplified network and CPU load.
			kcpHelloSentTime_ = timestamp();
			return true;
		}

		WARNING_MSG(fmt::format("ClientObject::sendKcpHello: name={}, sent={}, expected={}, error={}\n",
			name_, sent, helloLength, errorCode));
		return false;
	}

	kcpHelloSentTime_ = timestamp();
	return true;
}

//-------------------------------------------------------------------------------------
void ClientObject::processKcpHandshake()
{
	Network::EndPoint* pEndpoint = pServerChannel_ != NULL ? pServerChannel_->pEndPoint() : NULL;
	if (pEndpoint == NULL)
	{
		fallbackToBaseappTcp("UDP endpoint disappeared during handshake");
		return;
	}

	for (int attempt = 0; attempt < KCP_MAX_ACKS_PER_TICK; ++attempt)
	{
		char ack[PACKET_MAX_SIZE_UDP];
		const int received = pEndpoint->recv(ack, sizeof(ack));
		if (received < 0)
		{
			const int errorCode = lastSocketError();
			if (isTransientSocketError(errorCode))
				break;

			fallbackToBaseappTcp("UDP receive failed during KCP handshake");
			return;
		}

		uint32 channelID = 0;
		if (received == 0 || !parseKcpHelloAck(ack, static_cast<size_t>(received), channelID))
		{
			fallbackToBaseappTcp("invalid KCP hello ACK");
			return;
		}

		if (!completeKcpHandshake(channelID))
		{
			fallbackToBaseappTcp("failed to activate KCP transport");
		}
		return;
	}

	const uint64 now = timestamp();
	if (now - kcpHandshakeStartTime_ >= KCP_HANDSHAKE_TIMEOUT)
	{
		fallbackToBaseappTcp("KCP handshake timed out");
		return;
	}

	if (kcpHelloSentTime_ == 0 || now - kcpHelloSentTime_ >= KCP_HELLO_RETRY_INTERVAL)
	{
		if (!sendKcpHello())
			fallbackToBaseappTcp("failed to retry KCP hello");
	}
}

//-------------------------------------------------------------------------------------
bool ClientObject::completeKcpHandshake(uint32 channelID)
{
	Network::EndPoint* pEndpoint = pServerChannel_->pEndPoint();
	if (pEndpoint == NULL)
		return false;

	pKCPPacketSenderEx_ = new Network::KCPPacketSenderEx(*pEndpoint, networkInterface_, this);
	Network::KCPPacketReceiverEx* pReceiver = new Network::KCPPacketReceiverEx(*pEndpoint, networkInterface_, this);
	pServerChannel_->pPacketSender(pKCPPacketSenderEx_);
	pServerChannel_->pPacketReceiver(pReceiver);

	if (!pServerChannel_->configureTransport(Network::PROTOCOL_UDP, Network::SUB_PROTOCOL_KCP,
		static_cast<Network::ChannelID>(channelID)))
	{
		pServerChannel_->pPacketSender(NULL);
		pServerChannel_->pPacketReceiver(NULL);
		SAFE_RELEASE(pKCPPacketSenderEx_);
		SAFE_RELEASE(pReceiver);
		return false;
	}

	pServerChannel_->setFlags(true, Network::Channel::FLAG_HANDSHAKE);
	if (!Bots::getSingleton().networkInterface().dispatcher().registerReadFileDescriptor(*pEndpoint, pReceiver))
	{
		pServerChannel_->pPacketReceiver(NULL);
		SAFE_RELEASE(pReceiver);
		return false;
	}

	pKCPPacketReceiverEx_ = pReceiver;
	// Bots 自己管理客户端 Channel，传输激活后必须显式启动超时检测，不能依赖 NetworkInterface 注册流程。
	// Bots owns client Channels directly, so transport activation must explicitly start timeout detection instead of relying on NetworkInterface registration.
	pServerChannel_->startInactivityDetection(Network::g_channelExternalTimeout,
		Network::g_channelExternalTimeout / 2.f);
	connectedBaseapp_ = true;
	state_ = C_STATE_PLAY;

	Network::Bundle* pBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
	(*pBundle).newMessage(BaseappInterface::hello);
	(*pBundle) << KBEVersion::versionString() << KBEVersion::scriptVersionString();

	if(Network::g_channelExternalEncryptType == 1)
	{
		pBlowfishFilter_ = new Network::BlowfishFilter();
		(*pBundle).appendBlob(pBlowfishFilter_->key());
		pServerChannel_->pFilter(NULL);
	}
	else
	{
		std::string key = "";
		(*pBundle).appendBlob(key);
	}

	// Channel 接管 Bundle 所有权，可靠标志会把 BaseApp hello 写入 KCP 队列。
	// Channel takes Bundle ownership, and the reliable flag queues the BaseApp hello through KCP.
	pServerChannel_->sendTo(true, pBundle);
	Bots::getSingleton().onKcpHandshakeSucceeded();
	const double elapsedSeconds = static_cast<double>(timestamp() - kcpHandshakeStartTime_) /
		static_cast<double>(stampsPerSecond());
	INFO_MSG(fmt::format("ClientObject::completeKcpHandshake: name={}, address={}:{}, channelID={}, attempts={}, elapsed={:.3f}s\n",
		name_, ip_, udp_port_, channelID, kcpHelloAttempts_, elapsedSeconds));
	return true;
}

//-------------------------------------------------------------------------------------
void ClientObject::fallbackToBaseappTcp(const char* reason)
{
	const double elapsedSeconds = kcpHandshakeStartTime_ == 0 ? 0.0 :
		static_cast<double>(timestamp() - kcpHandshakeStartTime_) / static_cast<double>(stampsPerSecond());
	WARNING_MSG(fmt::format("ClientObject::fallbackToBaseappTcp: name={}, reason={}, attempts={}, elapsed={:.3f}s\n",
		name_, reason, kcpHelloAttempts_, elapsedSeconds));
	Bots::getSingleton().onTcpFallback();
	clearStates();
	connectedBaseapp_ = false;
	state_ = C_STATE_PLAY;

	if (!connectBaseappTcp())
		state_ = C_STATE_LOGIN_BASEAPP_CREATE;
}

//-------------------------------------------------------------------------------------
bool ClientObject::connectBaseappTcp()
{
	if (tcp_port_ == 0)
	{
		ERROR_MSG(fmt::format("ClientObject::connectBaseappTcp: no TCP fallback port, name={}\n", name_));
		error_ = C_ERROR_INIT_NETWORK_FAILED;
		state_ = C_STATE_LOGIN_BASEAPP_CREATE;
		return false;
	}

	Network::EndPoint* pEndpoint = Network::EndPoint::createPoolObject(OBJECTPOOL_POINT);
	pEndpoint->socket(SOCK_STREAM);
	if (!pEndpoint->good())
	{
		ERROR_MSG("ClientObject::connectBaseappTcp: couldn't create a TCP socket.\n");
		Network::EndPoint::reclaimPoolObject(pEndpoint);
		error_ = C_ERROR_INIT_NETWORK_FAILED;
		return false;
	}

	u_int32_t address = 0;
	Network::Address::string2ip(ip_.c_str(), address);
	if (pEndpoint->connect(htons(tcp_port_), address) == -1)
	{
		ERROR_MSG(fmt::format("ClientObject::connectBaseappTcp: name={}, error={}\n", name_, kbe_strerror()));
		Network::EndPoint::reclaimPoolObject(pEndpoint);
		state_ = C_STATE_LOGIN_BASEAPP_CREATE;
		return false;
	}

	pEndpoint->addr(Network::Address(ip_.c_str(), tcp_port_));
	pServerChannel_->pEndPoint(pEndpoint);
	pEndpoint->setnonblocking(true);
	pEndpoint->setnodelay(true);
	pServerChannel_->resetTransport();

	pTCPPacketSenderEx_ = new Network::TCPPacketSenderEx(*pEndpoint, networkInterface_, this);
	pTCPPacketReceiverEx_ = new Network::TCPPacketReceiverEx(*pEndpoint, networkInterface_, this);
	if (!Bots::getSingleton().networkInterface().dispatcher().registerReadFileDescriptor(*pEndpoint, pTCPPacketReceiverEx_))
	{
		ERROR_MSG(fmt::format("ClientObject::connectBaseappTcp: failed to register receiver, name={}\n", name_));
		clearStates();
		state_ = C_STATE_LOGIN_BASEAPP_CREATE;
		return false;
	}

	pServerChannel_->pPacketSender(pTCPPacketSenderEx_);
	pServerChannel_->startInactivityDetection(Network::g_channelExternalTimeout,
		Network::g_channelExternalTimeout / 2.f);
	connectedBaseapp_ = true;

	Network::Bundle* pBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
	(*pBundle).newMessage(BaseappInterface::hello);
	(*pBundle) << KBEVersion::versionString() << KBEVersion::scriptVersionString();

	if(Network::g_channelExternalEncryptType == 1)
	{
		pBlowfishFilter_ = new Network::BlowfishFilter();
		(*pBundle).appendBlob(pBlowfishFilter_->key());
		pServerChannel_->pFilter(NULL);
	}
	else
	{
		std::string key = "";
		(*pBundle).appendBlob(key);
	}

	pEndpoint->send(pBundle);
	Network::Bundle::reclaimPoolObject(pBundle);
	Bots::getSingleton().onTcpConnected();
	INFO_MSG(fmt::format("ClientObject::connectBaseappTcp: name={}, address={}:{}\n", name_, ip_, tcp_port_));
	return true;
}

//-------------------------------------------------------------------------------------
void ClientObject::gameTick()
{
	// 握手 socket 尚未交给 completion poller，必须由状态机有界推进且禁止普通 Channel Tick 触碰未初始化的 KCP。
	// The handshake socket is not owned by the completion poller yet, so advance it with bounded work and keep normal Channel ticks away from uninitialized KCP.
	if (state_ == C_STATE_LOGIN_BASEAPP_KCP_HANDSHAKE)
	{
		processKcpHandshake();
		return;
	}

	if(pServerChannel()->pEndPoint())
	{
		if(pServerChannel()->condemn() > 0)
		{
			destroy();
			return;
		}

		// NetworkInterface::processChannels() advances the shared tick epoch once
		// before Bots visits its clients. Channel counters reset lazily on actual IO,
		// so touching every idle Channel here only creates O(N) cache-line writes.
		// NetworkInterface::processChannels() 会在 Bots 遍历客户端前统一推进 Tick epoch。
		// Channel 计数器已在真实收发时懒清零，因此这里逐个写空闲 Channel 只会制造 O(N) 缓存写入。
	}
	else
	{
		if(connectedBaseapp_)
		{
			EventData_ServerCloased eventdata;
			eventHandler_.fire(&eventdata);
			connectedBaseapp_ = false;
			canReset_ = true;
			state_ = C_STATE_INIT;
			
			DEBUG_MSG(fmt::format("ClientObject({})::tickSend: serverCloased! name({})!\n", 
			this->appID(), this->name()));
		}
	}

	if(locktime() > 0 && timestamp() < locktime())
	{
		return;
	}

	switch(state_)
	{
		case C_STATE_INIT:

			state_ = C_STATE_PLAY;

			if(!initCreate())
				return;

			break;
		case C_STATE_CREATE:

			state_ = C_STATE_PLAY;

			if(!createAccount())
				return;

			break;
		case C_STATE_LOGIN:

			state_ = C_STATE_PLAY;

			if(!login())
				return;

			break;
		case C_STATE_LOGIN_BASEAPP_CREATE:

			state_ = C_STATE_PLAY;

			if(!initLoginBaseapp())
				return;

			break;
		case C_STATE_LOGIN_BASEAPP:

			state_ = C_STATE_PLAY;

			if(!loginBaseapp())
				return;

			break;
		case C_STATE_LOGIN_BASEAPP_KCP_HANDSHAKE:
			break;
		case C_STATE_PLAY:
			break;	
		case C_STATE_DESTROYED:
			return;
		default:
			KBE_ASSERT(false);
			break;
	};

	sendBaseappActiveTick(false);
	tickSend();
}

//-------------------------------------------------------------------------------------	
void ClientObject::onHelloCB_(Network::Channel* pChannel, const std::string& verInfo, 
		const std::string& scriptVerInfo, const std::string& protocolMD5, const std::string& entityDefMD5, 
		COMPONENT_TYPE componentType)
{
	if(Network::g_channelExternalEncryptType == 1)
	{
		pServerChannel_->pFilter(pBlowfishFilter_);
		pBlowfishFilter_ = NULL;
	}

	if(componentType == LOGINAPP_TYPE)
	{
		state_ = C_STATE_CREATE;
	}
	else
	{
		sendBaseappActiveTick(true);
		state_ = C_STATE_LOGIN_BASEAPP;
	}
}

//-------------------------------------------------------------------------------------
void ClientObject::onCreateAccountResult(Network::Channel * pChannel, MemoryStream& s)
{
	SERVER_ERROR_CODE retcode;

	s >> retcode;
	s.readBlob(serverDatas_);

	if(retcode != 0)
	{
		//error_ = C_ERROR_CREATE_FAILED;

		// 继续尝试登录
		state_ = C_STATE_LOGIN;
		
		INFO_MSG(fmt::format("ClientObject::onCreateAccountResult: {} create is failed! code={}.\n", 
			name_, SERVER_ERR_STR[retcode]));
		
		return;
	}

	state_ = C_STATE_LOGIN;
	INFO_MSG(fmt::format("ClientObject::onCreateAccountResult: {} create is successfully!\n", name_));
}

//-------------------------------------------------------------------------------------	
void ClientObject::onLoginSuccessfully(Network::Channel * pChannel, MemoryStream& s)
{
	std::string accountName;

	s >> accountName;
	s >> ip_;
	s >> tcp_port_;
	s >> udp_port_;
	s.readBlob(serverDatas_);

	INFO_MSG(fmt::format("ClientObject::onLoginSuccessfully: {} addr={}:{}|{}!\n",
		name_, ip_, tcp_port_, udp_port_));

	state_ = C_STATE_LOGIN_BASEAPP_CREATE;
}

//-------------------------------------------------------------------------------------	
void ClientObject::onLoginFailed(Network::Channel * pChannel, MemoryStream& s)
{
	SERVER_ERROR_CODE failedcode;

	s >> failedcode;
	s.readBlob(serverDatas_);

	INFO_MSG(fmt::format("ClientObject::onLoginFailed: {} failedcode={}!\n", 
		name_, SERVER_ERR_STR[failedcode]));

	// error_ = C_ERROR_LOGIN_FAILED;

	// 继续尝试登录
	state_ = C_STATE_LOGIN;
}

//-------------------------------------------------------------------------------------	
void ClientObject::onLoginBaseappFailed(Network::Channel * pChannel, SERVER_ERROR_CODE failedcode)
{
	ClientObjectBase::onLoginBaseappFailed(pChannel, failedcode);
	destroy();
}

//-------------------------------------------------------------------------------------
void ClientObject::onLogin(Network::Bundle* pBundle)
{
}

//-------------------------------------------------------------------------------------
}
