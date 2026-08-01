/*
This source file is part of KBEngine
For the latest info, see http://www.kbengine.org/
*/

#include "account_request_guard.h"
#include "server/bounded_stream_reader.h"
#include "network/common.h"
#include "server/common.h"
#include "server/server_errors.h"

namespace KBEngine{
namespace AccountRequestGuard{
namespace
{

bool validateCredentialStream(const MemoryStream& stream, bool hasAccountType)
{
	BoundedStreamReader cursor(stream);
	if (!cursor.skipString(ACCOUNT_NAME_MAX_LENGTH, false) ||
		!cursor.skipString(ACCOUNT_PASSWD_MAX_LENGTH, true))
	{
		return false;
	}

	if (hasAccountType && !cursor.skip(sizeof(uint8)))
		return false;

	return cursor.skipBlob(ACCOUNT_DATA_MAX_LENGTH) && cursor.empty();
}

}

bool validateCreateAccountStream(const MemoryStream& stream)
{
	return validateCredentialStream(stream, true);
}

bool validateLoginStream(const MemoryStream& stream)
{
	return validateCredentialStream(stream, false);
}

bool validateInterfacesCallbackStream(const MemoryStream& stream)
{
	BoundedStreamReader cursor(stream);
	COMPONENT_ID componentID = 0;
	SERVER_ERROR_CODE errorCode = SERVER_ERR_MAX;
	return cursor.read(componentID) && componentID != 0 &&
		cursor.skipString(ACCOUNT_NAME_MAX_LENGTH, false) &&
		cursor.skipString(ACCOUNT_NAME_MAX_LENGTH, true) &&
		cursor.skipString(ACCOUNT_PASSWD_MAX_LENGTH, true) &&
		cursor.read(errorCode) && errorCode < SERVER_ERR_MAX &&
		cursor.skipBlob(NETWORK_MESSAGE_MAX_SIZE) &&
		cursor.skipBlob(NETWORK_MESSAGE_MAX_SIZE) &&
		cursor.empty();
}

bool isValidAccountName(const std::string& value, bool allowEmpty)
{
	return (allowEmpty || !value.empty()) && value.size() <= ACCOUNT_NAME_MAX_LENGTH;
}

bool isValidPassword(const std::string& value)
{
	// Empty passwords retain the historical configuration-dependent behavior.
	// 空密码继续保留历史上的配置相关语义，这里只收口存储字段上限。
	return value.size() <= ACCOUNT_PASSWD_MAX_LENGTH;
}

bool isValidVerificationCode(const std::string& value)
{
	return !value.empty() && value.size() <= VERIFICATION_CODE_MAX_LENGTH;
}

bool isValidAccountData(const std::string& value)
{
	return value.size() <= ACCOUNT_DATA_MAX_LENGTH;
}

bool isValidInterfacesData(const std::string& value)
{
	return value.size() <= NETWORK_MESSAGE_MAX_SIZE;
}

bool isValidAccountType(uint8 value)
{
	return value == ACCOUNT_TYPE_NORMAL || value == ACCOUNT_TYPE_MAIL ||
		value == ACCOUNT_TYPE_SMART;
}

}
}
