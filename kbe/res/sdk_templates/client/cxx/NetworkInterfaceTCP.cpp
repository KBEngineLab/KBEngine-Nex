
#include "NetworkInterfaceTCP.h"

#include "GameThreadDispatcher.h"
#include "MemoryStream.h"
#include "KBEvent.h"
#include "KBDebug.h"
#include "Interfaces.h"
#include "KBEngine.h"
#include "KBEngineArgs.h"
#include "MessageReader.h"

namespace KBEngine
{

NetworkInterfaceTCP::NetworkInterfaceTCP():
	NetworkInterfaceBase(),
	stopRequested_(false),
	connected_(false),
	disconnectedEventPending_(false),
	sessionId_(0),
	socket_(NativeSocket::InvalidSocket),
	recvQueue_(TCP_PACKET_MAX)
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
	KBEngineArgs* args = KBEngineApp::getSingleton().getInitArgs();
	const int configuredCapacity = args ? args->getTCPRecvBufferSize() : static_cast<int>(TCP_PACKET_MAX);
	const std::size_t receiveCapacity = configuredCapacity > 0 ? static_cast<std::size_t>(configuredCapacity) : TCP_PACKET_MAX;
	recvQueue_.reset(receiveCapacity);
	// 复用主线程 drain 缓冲，避免每个 Tick 为固定上限的网络数据重复分配和释放堆内存。
	// Reuse the game-thread drain buffer to avoid allocating and freeing bounded network storage on every tick.
	std::vector<uint8> processBuffer;
	processBuffer.reserve(receiveCapacity);
	processBuffer_.swap(processBuffer);

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
	const bool wasActive = connected_.load() || connectCB_ != nullptr;
	stopWorker_();
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

	if (shouldNotifyDisconnected)
	{
		// 主动关闭和工作线程检测到的断线共享一次通知；重复 close() 不得触发重复重登录。
		// Active close and worker-detected loss share one notification; repeated close() calls must not trigger duplicate relogins.
		INFO_MSG("NetworkInterfaceTCP::close(): network closed!");
		KBENGINE_EVENT_FIRE_ALL(KBEventTypes::onDisconnected, std::make_shared<UKBEventData_onDisconnected>());
	}
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
	if (recvQueue_.drain(processBuffer_) > 0 && pMessageReader_)
	{
		pBuffer_->clear(true);
		pBuffer_->append(processBuffer_.data(), static_cast<MessageLengthEx>(processBuffer_.size()));

		// 先释放有界队列容量，再在主线程执行解密和消息回调，避免业务逻辑阻塞接收线程或持有队列锁。
		// Release bounded queue capacity before decrypting and dispatching on the game thread so application work cannot block the receiver or hold its lock.
		if (pFilter_)
		{
			pFilter_->recv(pMessageReader_, pBuffer_);
		}
		else
		{
			pMessageReader_->process(pBuffer_->data(), 0, pBuffer_->length());
		}
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

	std::vector<uint8> buffer(recvQueue_.capacity());
	while (!stopRequested_.load() && sessionId == sessionId_.load())
	{
		const int received = NativeSocket::recvSome(socket, buffer.data(), static_cast<int32>(buffer.size()), error);
		if (received > 0)
		{
			if (!recvQueue_.write(buffer.data(), static_cast<std::size_t>(received)))
			{
				break;
			}
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
	// 停止队列会唤醒可能因满载而休眠的接收线程，确保关闭和重登录不会在 join() 中死锁。
	// Stopping the queue wakes a receiver sleeping on full capacity so close and relogin cannot deadlock in join().
	recvQueue_.stop();
	closeSocket_(false);

	if (workerThread_.joinable())
	{
		workerThread_.join();
	}
}

void NetworkInterfaceTCP::clearRecvQueue_()
{
	recvQueue_.stop();
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
