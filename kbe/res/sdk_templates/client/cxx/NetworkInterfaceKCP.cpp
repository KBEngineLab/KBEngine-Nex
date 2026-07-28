// Copyright 1998-2016 Epic Games, Inc. All Rights Reserved.

#include "NetworkInterfaceKCP.h"

#include "GameThreadDispatcher.h"
#include "MemoryStream.h"
#include "KBEvent.h"
#include "KBDebug.h"
#include "Interfaces.h"
#include "KBEngine.h"
#include "KBEngineArgs.h"
#include "MessageReader.h"

#include <chrono>
#include <cstring>
#include <limits>
#include <thread>

namespace KBEngine
{

NetworkInterfaceKCP::NetworkInterfaceKCP():
	NetworkInterfaceBase(),
	kcp_(nullptr),
	connID_(0),
	nextKcpUpdate_(0),
	stopRequested_(false),
	connected_(false),
	disconnectedEventPending_(false),
	sessionId_(0),
	socket_(NativeSocket::InvalidSocket)
{
}

NetworkInterfaceKCP::~NetworkInterfaceKCP()
{
	NetworkInterfaceKCP::reset();
}

bool NetworkInterfaceKCP::connectTo(const KBString& addr, uint16 port, InterfaceConnect* callback, int userdata)
{
	INFO_MSG("NetworkInterfaceKCP::connectTo(): will connect to %s:%d ...", *addr, port);
	reset();

	connectCB_ = callback;
	connectIP_ = addr;
	connectPort_ = port;
	connectUserdata_ = userdata;
	startTime_ = getTimeSeconds();

	stopRequested_ = false;
	connected_ = false;
	disconnectedEventPending_ = false;
	connID_ = 0;
	nextKcpUpdate_ = 0;

	const uint64 sessionId = ++sessionId_;
	workerThread_ = std::thread(&NetworkInterfaceKCP::workerLoop_, this, addr, port, callback, userdata, sessionId);
	return true;
}

void NetworkInterfaceKCP::reset()
{
	stopWorker_();
	finiKCP_();
	clearRecvQueue_();

	disconnectedEventPending_ = false;
	connected_ = false;
	connectCB_ = nullptr;
	connectIP_ = KBTEXT("");
	connectPort_ = 0;
	connectUserdata_ = 0;
	startTime_ = 0.0;
	connID_ = 0;
	nextKcpUpdate_ = 0;
}

void NetworkInterfaceKCP::close()
{
	const bool wasActive = connected_.load() || connectCB_ != nullptr;
	stopWorker_();
	finiKCP_();
	clearRecvQueue_();
	const bool shouldNotifyDisconnected = wasActive || disconnectedEventPending_.exchange(false);

	KBE_SAFE_RELEASE(pMessageReader_);
	KBE_SAFE_RELEASE(pBuffer_);
	KBE_SAFE_RELEASE(pFilter_);

	connected_ = false;
	connectCB_ = nullptr;
	connectIP_ = KBTEXT("");
	connectPort_ = 0;
	connectUserdata_ = 0;
	startTime_ = 0.0;
	connID_ = 0;
	nextKcpUpdate_ = 0;

	if (shouldNotifyDisconnected)
	{
		// 主动关闭和工作线程检测到的断线共享一次通知；重复 close() 不得触发重复重登录。
		// Active close and worker-detected loss share one notification; repeated close() calls must not trigger duplicate relogins.
		INFO_MSG("NetworkInterfaceKCP::close(): network closed!");
		KBENGINE_EVENT_FIRE_ALL(KBEventTypes::onDisconnected, std::make_shared<UKBEventData_onDisconnected>());
	}
}

bool NetworkInterfaceKCP::valid()
{
	return connected_.load() || connectCB_ != nullptr;
}

bool NetworkInterfaceKCP::sendTo(MemoryStream* pMemoryStream)
{
	if (!pMemoryStream || pMemoryStream->length() == 0)
	{
		return false;
	}

	std::lock_guard<std::mutex> lock(kcpMutex_);
	if (!kcp_)
	{
		ERROR_MSG("NetworkInterfaceKCP::sendTo(): kcp is null!");
		return false;
	}

	if (pMemoryStream->length() > static_cast<uint32>(std::numeric_limits<int>::max()))
	{
		ERROR_MSG("NetworkInterfaceKCP::sendTo(): payload is too large, length=%u", pMemoryStream->length());
		return false;
	}

	const int result = ikcp_send(
		kcp_,
		reinterpret_cast<const char*>(pMemoryStream->data()),
		static_cast<int>(pMemoryStream->length()));
	if (result < 0)
	{
		ERROR_MSG("NetworkInterfaceKCP::sendTo(): ikcp_send failed ret=%d", result);
		return false;
	}

	return true;
}

void NetworkInterfaceKCP::process()
{
	{
		std::lock_guard<std::mutex> lock(kcpMutex_);
		if (kcp_)
		{
			const uint32 now = nowMs_();
			if (now >= nextKcpUpdate_)
			{
				ikcp_update(kcp_, now);
				nextKcpUpdate_ = ikcp_check(kcp_, now);
				drainKCPRecvLocked_();
			}
		}
	}

	std::queue<std::vector<uint8>> pending;
	{
		std::lock_guard<std::mutex> lock(recvMutex_);
		std::swap(pending, recvQueue_);
	}

	while (!pending.empty())
	{
		const std::vector<uint8>& data = pending.front();
		if (!data.empty() && pMessageReader_)
		{
			pBuffer_->clear(true);
			pBuffer_->append(data.data(), data.size());

			if (pFilter_)
			{
				pFilter_->recv(pMessageReader_, pBuffer_);
			}
			else
			{
				pMessageReader_->process(pBuffer_->data(), 0, pBuffer_->length());
			}
		}

		pending.pop();
	}

	if (disconnectedEventPending_.exchange(false))
	{
		KBENGINE_EVENT_FIRE_ALL(KBEventTypes::onDisconnected, std::make_shared<UKBEventData_onDisconnected>());
	}
}

bool NetworkInterfaceKCP::initKCP_()
{
	std::lock_guard<std::mutex> lock(kcpMutex_);

	if (kcp_)
	{
		ikcp_release(kcp_);
		kcp_ = nullptr;
	}

	kcp_ = ikcp_create(static_cast<IUINT32>(connID_), this);
	if (!kcp_)
	{
		ERROR_MSG("NetworkInterfaceKCP::initKCP_(): ikcp_create failed, conv=%u", connID_);
		return false;
	}

	kcp_->output = &NetworkInterfaceKCP::kcpOutput_;

	// KBE 服务端 UDP 默认 MTU 低于以太网 MTU，保守设置避免 IP 分片。
	// The KBE server UDP MTU is below the Ethernet MTU, so this conservative value avoids IP fragmentation.
	ikcp_setmtu(kcp_, 1400);

	KBEngineArgs* args = KBEngineApp::getSingleton().getInitArgs();
	if (args)
	{
		ikcp_wndsize(kcp_, args->getUDPSendBufferSize(), args->getUDPRecvBufferSize());
	}

	// 使用 fast mode，保持原有同步参数，降低移动与战斗同步延迟。
	// Preserve the existing fast-mode parameters to reduce movement and combat synchronization latency.
	ikcp_nodelay(kcp_, 1, 10, 2, 1);
	kcp_->rx_minrto = 10;
	nextKcpUpdate_ = nowMs_();
	return true;
}

void NetworkInterfaceKCP::finiKCP_()
{
	std::lock_guard<std::mutex> lock(kcpMutex_);
	if (kcp_)
	{
		ikcp_release(kcp_);
		kcp_ = nullptr;
	}
}

void NetworkInterfaceKCP::workerLoop_(KBString addr, uint16 port, InterfaceConnect* callback, int userdata, uint64 sessionId)
{
	NativeSocket::Socket socket = NativeSocket::InvalidSocket;
	std::string error;
	if (!NativeSocket::connectUdp(addr, port, socket, error))
	{
		ERROR_MSG("NetworkInterfaceKCP::connectTo(): create/connect udp socket failed, err=%s", error.c_str());
		fireConnectionState_(callback, addr, port, false, userdata, sessionId);
		return;
	}

	{
		std::lock_guard<std::mutex> lock(socketMutex_);
		if (sessionId != sessionId_.load() || stopRequested_.load())
		{
			NativeSocket::closeSocket(socket);
			return;
		}

		socket_ = socket;
	}

	uint8 buffer[65536];
	const uint32 connectStart = nowMs_();
	uint32 lastHelloTime = 0;
	bool handshakeDone = false;

	while (!stopRequested_.load() && sessionId == sessionId_.load())
	{
		const uint32 now = nowMs_();
		if (!handshakeDone && (lastHelloTime == 0 || now - lastHelloTime >= 1000))
		{
			sendDatagram_(reinterpret_cast<const uint8*>(UDP_HELLO.c_str()), static_cast<int32>(UDP_HELLO.length()));
			lastHelloTime = now;
		}

		if (!handshakeDone && now - connectStart > 30000)
		{
			ERROR_MSG("NetworkInterfaceKCP::connectTo(): connect to %s:%d timeout!", *addr, port);
			fireConnectionState_(callback, addr, port, false, userdata, sessionId);
			break;
		}

		const int received = NativeSocket::recvSome(socket, buffer, sizeof(buffer), error);
		if (received > 0)
		{
			if (!handshakeDone)
			{
				KBString versionString;
				uint32 connID = 0;

				bool success = parseHelloAck_(buffer, received, versionString, connID);
				if (!success)
				{
					ERROR_MSG("NetworkInterfaceKCP::connectTo(): malformed hello acknowledgement, length=%d", received);
				}
				else if (KBEngineApp::getSingleton().serverVersion() != versionString)
				{
					ERROR_MSG("NetworkInterfaceKCP::connectTo(): version(%s!=%s) mismatch!",
						*versionString, *KBEngineApp::getSingleton().serverVersion());
					success = false;
				}
				else if (connID == 0)
				{
					ERROR_MSG("NetworkInterfaceKCP::connectTo(): conv is 0!");
					success = false;
				}
				else
				{
					connID_ = connID;
					handshakeDone = initKCP_();
					connected_ = handshakeDone;
					INFO_MSG("NetworkInterfaceKCP::connectTo(): connect to %s:%d success!", *addr, port);
				}

				fireConnectionState_(callback, addr, port, success && handshakeDone, userdata, sessionId);
				if (!success || !handshakeDone)
				{
					break;
				}

				continue;
			}

			handleDatagram_(buffer, received, sessionId);
			continue;
		}

		if (received == 0)
		{
			break;
		}

		if (received == NativeSocket::WouldBlock)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			continue;
		}

		if (!stopRequested_.load())
		{
			ERROR_MSG("NetworkInterfaceKCP::workerLoop_(): recv failed, err=%s", error.c_str());
		}
		break;
	}

	closeSocket_(!stopRequested_.load() && sessionId == sessionId_.load() && handshakeDone);
}

void NetworkInterfaceKCP::stopWorker_()
{
	++sessionId_;
	stopRequested_ = true;
	closeSocket_(false);

	if (workerThread_.joinable())
	{
		workerThread_.join();
	}
}

void NetworkInterfaceKCP::clearRecvQueue_()
{
	std::lock_guard<std::mutex> lock(recvMutex_);
	std::queue<std::vector<uint8>> empty;
	std::swap(recvQueue_, empty);
}

void NetworkInterfaceKCP::closeSocket_(bool fireDisconnectedEvent)
{
	NativeSocket::Socket socket = NativeSocket::InvalidSocket;
	{
		std::lock_guard<std::mutex> lock(socketMutex_);
		socket = socket_;
		socket_ = NativeSocket::InvalidSocket;
		connected_ = false;
	}

	NativeSocket::closeSocket(socket);

	if (fireDisconnectedEvent)
	{
		disconnectedEventPending_ = true;
	}
}

void NetworkInterfaceKCP::handleDatagram_(const uint8* data, int32 length, uint64 sessionId)
{
	std::lock_guard<std::mutex> lock(kcpMutex_);
	if (!kcp_ || sessionId != sessionId_.load())
	{
		return;
	}

	if (ikcp_input(kcp_, reinterpret_cast<const char*>(data), length) < 0)
	{
		WARNING_MSG("NetworkInterfaceKCP::handleDatagram_(): ignored invalid KCP datagram, length=%d", length);
		return;
	}

	drainKCPRecvLocked_();
}

void NetworkInterfaceKCP::drainKCPRecvLocked_()
{
	if (!kcp_)
	{
		return;
	}

	while (true)
	{
		const int messageSize = ikcp_peeksize(kcp_);
		if (messageSize < 0)
		{
			break;
		}

		// KCP 会重组跨数据报消息，固定 64 KiB 缓冲区会让更大的合法协议消息永久留在接收队列中。
		// KCP reassembles messages across datagrams; a fixed 64 KiB buffer leaves larger valid protocol messages stuck in its receive queue.
		std::vector<uint8> payload(static_cast<size_t>(messageSize));
		const int recvLen = ikcp_recv(
			kcp_,
			reinterpret_cast<char*>(payload.data()),
			messageSize);
		if (recvLen < 0)
		{
			ERROR_MSG("NetworkInterfaceKCP::drainKCPRecvLocked_(): ikcp_recv failed, ret=%d", recvLen);
			break;
		}

		payload.resize(static_cast<size_t>(recvLen));
		{
			std::lock_guard<std::mutex> lock(recvMutex_);
			recvQueue_.push(std::move(payload));
		}
	}
}

bool NetworkInterfaceKCP::parseHelloAck_(
	const uint8* data,
	int32 length,
	KBString& versionString,
	uint32& connID) const
{
	versionString = KBTEXT("");
	connID = 0;

	const size_t ackLength = UDP_HELLO_ACK.length();
	const size_t packetLength = length > 0 ? static_cast<size_t>(length) : 0;
	const size_t minimumLength = ackLength + 1 + 1 + sizeof(uint32);
	if (!data || packetLength < minimumLength ||
		std::memcmp(data, UDP_HELLO_ACK.data(), ackLength) != 0 ||
		data[ackLength] != 0)
	{
		return false;
	}

	const uint8* versionBegin = data + ackLength + 1;
	const uint8* packetEnd = data + packetLength;
	const uint8* versionEnd = static_cast<const uint8*>(
		std::memchr(versionBegin, 0, static_cast<size_t>(packetEnd - versionBegin)));
	if (!versionEnd || versionEnd == versionBegin || packetEnd - versionEnd - 1 != sizeof(uint32))
	{
		return false;
	}

	versionString.assign(
		reinterpret_cast<const char*>(versionBegin),
		static_cast<size_t>(versionEnd - versionBegin));

	// 握手协议固定使用小端 uint32，逐字节解码避免依赖客户端 CPU 字节序和未对齐访问。
	// The handshake uses a little-endian uint32; byte-wise decoding avoids host-endian and unaligned-access assumptions.
	const uint8* connBytes = versionEnd + 1;
	connID = static_cast<uint32>(connBytes[0]) |
		(static_cast<uint32>(connBytes[1]) << 8) |
		(static_cast<uint32>(connBytes[2]) << 16) |
		(static_cast<uint32>(connBytes[3]) << 24);
	return connID != 0;
}

void NetworkInterfaceKCP::fireConnectionState_(
	InterfaceConnect* callback,
	const KBString& addr,
	uint16 port,
	bool success,
	int userdata,
	uint64 sessionId)
{
	GameThreadDispatcher::Instance().Post(
		[this, callback, addr, port, success, userdata, sessionId]()
		{
			if (sessionId != sessionId_.load())
			{
				return;
			}

			if (callback)
			{
				callback->onConnectCallback(addr, port, success, userdata);
			}
			if (connectCB_ == callback)
			{
				connectCB_ = nullptr;
			}

			auto pEventData = std::make_shared<UKBEventData_onConnectionState>();
			pEventData->success = success;
			pEventData->address = KBString::Printf(KBTEXT("%s:%d"), *addr, port);
			KBENGINE_EVENT_FIRE_ALL(KBEventTypes::onConnectionState, pEventData);
		});
}

bool NetworkInterfaceKCP::sendDatagram_(const uint8* data, int32 length)
{
	NativeSocket::Socket socket = NativeSocket::InvalidSocket;
	{
		std::lock_guard<std::mutex> lock(socketMutex_);
		socket = socket_;
	}

	if (!NativeSocket::isValid(socket))
	{
		return false;
	}

	std::lock_guard<std::mutex> sendLock(sendMutex_);
	std::string error;
	const int sent = NativeSocket::sendDatagram(socket, data, length, error);
	if (sent < 0)
	{
		ERROR_MSG("NetworkInterfaceKCP::sendDatagram_(): send failed, err=%s", error.c_str());
		return false;
	}

	return sent == length;
}

uint32 NetworkInterfaceKCP::nowMs_()
{
	using namespace std::chrono;
	return static_cast<uint32>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

int NetworkInterfaceKCP::kcpOutput_(const char* buf, int len, ikcpcb* kcp, void* user)
{
	NetworkInterfaceKCP* self = reinterpret_cast<NetworkInterfaceKCP*>(user);
	if (!self)
	{
		return 0;
	}

	return self->sendDatagram_(reinterpret_cast<const uint8*>(buf), len) ? len : -1;
}

}
