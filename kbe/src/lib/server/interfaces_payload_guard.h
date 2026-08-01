/*
This source file is part of KBEngine
For the latest info, see http://www.kbengine.org/
*/

#ifndef KBE_SERVER_INTERFACES_PAYLOAD_GUARD_H
#define KBE_SERVER_INTERFACES_PAYLOAD_GUARD_H

#include "common/common.h"
#include "common/memorystream.h"
#include "server/server_errors.h"

namespace KBEngine{
namespace InterfacesPayloadGuard{

const size_t CHARGE_ID_MAX_LENGTH = 128;

bool validateChargeRequestStream(const MemoryStream& stream);
bool validateInterfacesChargeStream(const MemoryStream& stream);
bool validateChargeCallbackStream(const MemoryStream& stream);
bool validateBaseappChargeCallbackStream(const MemoryStream& stream);

bool isValidChargeID(const std::string& value);
bool isValidChargeData(const std::string& value);
bool isValidClientRequestKey(const std::string& value);
bool isValidErrorCode(SERVER_ERROR_CODE value);

}
}

#endif // KBE_SERVER_INTERFACES_PAYLOAD_GUARD_H
