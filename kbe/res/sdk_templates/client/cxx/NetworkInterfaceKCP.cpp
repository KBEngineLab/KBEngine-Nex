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
	stopWorker_();
	finiKCP_();
	clearRecvQueue_();

	KBE_SAFE_RELEASE(pMessageReader_);
	KBE_SAFE_RELEASE(pBuffer_);
	KBE_SAFE_RELEASE(pFilter_);

	disconnectedEventPending_ = false;
	connected_ = false;
	connectCB_ = nullptr;
	connectIP_ = KBTEXT("");
	connectPort_ = 0;
	connectUserdata_ = 0;
	startTime_ = 0.0;
	connID_ = 0;
	nextKcpUpdate_ = 0;

	INFO_MSG("NetworkInterfaceKCP::close(): network closed!");
	KBENGINE_EVENT_FIRE_ALL(KBEventTypes::onDisconnected, std::make_shared<UKBEventData_onDisconnected>());
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

	const int result = ikcp_send(kcp_, reinterpret_cast<const char*>(pMemoryStream->data()), pMemoryStream->length());
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
	kcp_->output = &NetworkInterfaceKCP::kcpOutput_;

	// KBE 服务端 UDP 默认 MTU 低于以太网 MTU，保守设置避免 IP 分片。
	ikcp_setmtu(kcp_, 1400);

	KBEngineArgs* args = KBEngineApp::getSingleton().getInitArgs();
	if (args)
	{
		ikcp_wndsize(kcp_, args->getUDPSendBufferSize(), args->getUDPRecvBufferSize());
	}

	// 使用 fast mode，保持原有同步参数，降低移动与战斗同步延迟。
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
				MemoryStream ms;
				ms.append(buffer, received);
				ms.rpos(0);

				KBString helloAck;
				KBString versionString;
				uint32 connID = 0;
				ms >> helloAck >> versionString >> connID;

				bool success = true;
				if (helloAck != UDP_HELLO_ACK)
				{
					ERROR_MSG("NetworkInterfaceKCP::connectTo(): receive hello-ack(%s!=%s) mismatch!",
						*helloAck, *UDP_HELLO_ACK);
					success = false;
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

	ikcp_input(kcp_, reinterpret_cast<const char*>(data), length);
	drainKCPRecvLocked_();
}

void NetworkInterfaceKCP::drainKCPRecvLocked_()
{
	if (!kcp_)
	{
		return;
	}

	char recvBuf[65536];
	int recvLen = ikcp_recv(kcp_, recvBuf, sizeof(recvBuf));
	while (recvLen > 0)
	{
		std::vector<uint8> payload(reinterpret_cast<uint8*>(recvBuf), reinterpret_cast<uint8*>(recvBuf) + recvLen);
		{
			std::lock_guard<std::mutex> lock(recvMutex_);
			recvQueue_.push(std::move(payload));
		}

		recvLen = ikcp_recv(kcp_, recvBuf, sizeof(recvBuf));
	}
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
