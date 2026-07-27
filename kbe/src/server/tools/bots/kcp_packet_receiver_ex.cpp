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

#include "kcp_packet_receiver_ex.h"
#include "clientobject.h"

namespace KBEngine
{
namespace Network
{

KCPPacketReceiverEx::KCPPacketReceiverEx(EndPoint& endpoint,
	NetworkInterface& networkInterface, ClientObject* pClientObject) :
	KCPPacketReceiver(endpoint, networkInterface),
	pClientObject_(pClientObject)
{
}

KCPPacketReceiverEx::~KCPPacketReceiverEx() = default;

Channel* KCPPacketReceiverEx::getChannel()
{
	return pClientObject_->pServerChannel();
}

Channel* KCPPacketReceiverEx::findChannel(const Address& addr)
{
	return pClientObject_->pServerChannel();
}

}
}
