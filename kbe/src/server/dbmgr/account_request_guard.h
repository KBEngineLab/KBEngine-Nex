/*
This source file is part of KBEngine
For the latest info, see http://www.kbengine.org/
*/

#ifndef KBE_DBMGR_ACCOUNT_REQUEST_GUARD_H
#define KBE_DBMGR_ACCOUNT_REQUEST_GUARD_H

#include "common/common.h"
#include "common/memorystream.h"

namespace KBEngine{
namespace AccountRequestGuard{

const size_t VERIFICATION_CODE_MAX_LENGTH = 256;

bool validateCreateAccountStream(const MemoryStream& stream);
bool validateLoginStream(const MemoryStream& stream);
bool validateInterfacesCallbackStream(const MemoryStream& stream);

bool isValidAccountName(const std::string& value, bool allowEmpty = false);
bool isValidPassword(const std::string& value);
bool isValidVerificationCode(const std::string& value);
bool isValidAccountData(const std::string& value);
bool isValidInterfacesData(const std::string& value);
bool isValidAccountType(uint8 value);

}
}

#endif // KBE_DBMGR_ACCOUNT_REQUEST_GUARD_H
