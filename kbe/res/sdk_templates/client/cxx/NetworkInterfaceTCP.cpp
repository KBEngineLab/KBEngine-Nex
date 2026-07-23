
#include "NetworkInterfaceTCP.h"

#include "GameThreadDispatcher.h"
#include "MemoryStream.h"
#include "KBEvent.h"
#include "KBDebug.h"
#include "Interfaces.h"
#include "MessageReader.h"

namespace KBEngine
{

NetworkInterfaceTCP::NetworkInterfaceTCP():
	NetworkInterfaceBase(),
	stopRequested_(false),
	connected_(false),
	disconnectedEventPending_(false),
	sessionId_(0),
	socket_(NativeSocket::InvalidSocket)
{
}

NetworkInterfaceTCP::~NetworkInterfaceTCP()
{
	NetworkInterfaceTCP::reset();
}

bool NetworkInterfaceTCP::connectTo(const KBString& addr, uint16 port, InterfaceConnect* callback, int userdata)
{
	INFO_MSG("NetworkInterfaceTCP::connectTo(): will connect to %s:%d ...", *addr, port);
	reset();

	connectCB_ = callback;
	connectIP_ = addr;
	connectPort_ = port;
	connectUserdata_ = userdata;
	startTime_ = getTimeSeconds();

	stopRequested_ = false;
	connected_ = false;
	disconnectedEventPending_ = false;

	const uint64 sessionId = ++sessionId_;
	workerThread_ = std::thread(&NetworkInterfaceTCP::workerLoop_, this, addr, port, callback, userdata, sessionId);
	return true;
}

void NetworkInterfaceTCP::reset()
{
	stopWorker_();
	clearRecvQueue_();

	disconnectedEventPending_ = false;
	connected_ = false;
	connectCB_ = nullptr;
	connectIP_ = KBTEXT("");
	connectPort_ = 0;
	connectUserdata_ = 0;
	startTime_ = 0.0;
}

void NetworkInterfaceTCP::close()
{
	stopWorker_();
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

	INFO_MSG("NetworkInterfaceTCP::close(): network closed!");
	KBENGINE_EVENT_FIRE_ALL(KBEventTypes::onDisconnected, std::make_shared<UKBEventData_onDisconnected>());
}

bool NetworkInterfaceTCP::valid()
{
	return connected_.load();
}

bool NetworkInterfaceTCP::sendTo(MemoryStream* pMemoryStream)
{
	if (!pMemoryStream || pMemoryStream->length() == 0)
	{
		return true;
	}

	NativeSocket::Socket socket = NativeSocket::InvalidSocket;
	{
		std::lock_guard<std::mutex> lock(socketMutex_);
		socket = socket_;
	}

	if (!connected_.load() || !NativeSocket::isValid(socket))
	{
		ERROR_MSG("NetworkInterfaceTCP::sendTo(): socket is invalid!");
		return false;
	}

	std::lock_guard<std::mutex> sendLock(sendMutex_);
	std::string error;
	if (!NativeSocket::sendAll(socket, pMemoryStream->data(), pMemoryStream->length(), error))
	{
		ERROR_MSG("NetworkInterfaceTCP::sendTo(): send failed, err=%s", error.c_str());
		closeSocket_(true);
		return false;
	}

	return true;
}

void NetworkInterfaceTCP::process()
{
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

void NetworkInterfaceTCP::workerLoop_(KBString addr, uint16 port, InterfaceConnect* callback, int userdata, uint64 sessionId)
{
	NativeSocket::Socket socket = NativeSocket::InvalidSocket;
	std::string error;
	if (!NativeSocket::connectTcp(addr, port, 30000, socket, error))
	{
		ERROR_MSG("NetworkInterfaceTCP::connectTo(): connect to %s:%d failed, err=%s", *addr, port, error.c_str());
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
		connected_ = true;
	}

	INFO_MSG("NetworkInterfaceTCP::connectTo(): connect to %s:%d success!", *addr, port);
	fireConnectionState_(callback, addr, port, true, userdata, sessionId);

	uint8 buffer[65536];
	while (!stopRequested_.load() && sessionId == sessionId_.load())
	{
		const int received = NativeSocket::recvSome(socket, buffer, sizeof(buffer), error);
		if (received > 0)
		{
			std::vector<uint8> data(buffer, buffer + received);
			std::lock_guard<std::mutex> lock(recvMutex_);
			recvQueue_.push(std::move(data));
			continue;
		}

		if (received == 0)
		{
			INFO_MSG("NetworkInterfaceTCP::workerLoop_(): peer closed connection.");
			break;
		}

		if (!stopRequested_.load())
		{
			ERROR_MSG("NetworkInterfaceTCP::workerLoop_(): recv failed, err=%s", error.c_str());
		}
		break;
	}

	closeSocket_(!stopRequested_.load() && sessionId == sessionId_.load());
}

void NetworkInterfaceTCP::stopWorker_()
{
	++sessionId_;
	stopRequested_ = true;
	closeSocket_(false);

	if (workerThread_.joinable())
	{
		workerThread_.join();
	}
}

void NetworkInterfaceTCP::clearRecvQueue_()
{
	std::lock_guard<std::mutex> lock(recvMutex_);
	std::queue<std::vector<uint8>> empty;
	std::swap(recvQueue_, empty);
}

void NetworkInterfaceTCP::closeSocket_(bool fireDisconnectedEvent)
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

void NetworkInterfaceTCP::fireConnectionState_(
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

}
