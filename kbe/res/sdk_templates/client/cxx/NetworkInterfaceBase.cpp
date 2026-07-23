
#include "NetworkInterfaceBase.h"
#include "MemoryStream.h"
#include "KBEvent.h"
#include "KBDebug.h"
#include "Interfaces.h"
#include "KBEngine.h"
#include "MessageReader.h"

namespace KBEngine
{

NetworkInterfaceBase::NetworkInterfaceBase():
	pMessageReader_(new MessageReader()),
	pBuffer_(new MemoryStream()),
	connectCB_(nullptr),
	connectIP_(KBTEXT("")),
	connectPort_(0),
	connectUserdata_(0),
	startTime_(0.0),
	isDestroyed_(false),
	pFilter_(nullptr)
{
}

NetworkInterfaceBase::~NetworkInterfaceBase()
{
	NetworkInterfaceBase::close();
}

void NetworkInterfaceBase::reset()
{
	close();
}

void NetworkInterfaceBase::close()
{
	// INFO_MSG("NetworkInterfaceBase::close(): network closed!");
	// KBENGINE_EVENT_FIRE_ALL(KBEventTypes::onDisconnected, std::make_shared<UKBEventData_onDisconnected>());
	//
	// KBE_SAFE_RELEASE(pFilter_);
	//
	// connectCB_ = nullptr;
	// connectIP_ = KBTEXT("");
	// connectPort_ = 0;
	// connectUserdata_ = 0;
	// startTime_ = 0.0;
}

bool NetworkInterfaceBase::valid() {
	return true;
}

void NetworkInterfaceBase::process()
{
}


bool NetworkInterfaceBase::connectTo(const KBString& addr, uint16 port, InterfaceConnect* callback, int userdata)
{
	INFO_MSG("NetworkInterfaceBase::connectTo(): will connect to %s:%d ...", *addr, port);

	reset();

	connectCB_ = callback;
	connectIP_ = addr;
	connectPort_ = port;
	connectUserdata_ = userdata;
	startTime_ = getTimeSeconds();

	return true;
}

bool NetworkInterfaceBase::send(MemoryStream* pMemoryStream)
{
	if (!valid())
	{
		return false;
	}

	if (pFilter_) {
		return pFilter_->send(this,pMemoryStream);
	}

	return sendTo(pMemoryStream);
}

bool NetworkInterfaceBase::sendTo(MemoryStream *pMemoryStream) {
	return true;
}

}
