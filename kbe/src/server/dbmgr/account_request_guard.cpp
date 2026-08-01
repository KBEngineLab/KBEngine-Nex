/*
This source file is part of KBEngine
For the latest info, see http://www.kbengine.org/
*/

#include "account_request_guard.h"
#include "network/common.h"
#include "server/common.h"
#include "server/server_errors.h"

#include <cstring>

namespace KBEngine{
namespace AccountRequestGuard{
namespace
{

class StreamCursor
{
public:
	StreamCursor(const MemoryStream& stream):
	data_(stream.length() > 0 ? stream.data() + stream.rpos() : NULL),
	remaining_(stream.length())
	{
	}

	bool skip(size_t count)
	{
		if (count > remaining_)
			return false;
		if (count == 0)
			return true;

		data_ += count;
		remaining_ -= count;
		return true;
	}

	template <typename T> bool read(T& value)
	{
		if (remaining_ < sizeof(T))
			return false;

		std::memcpy(&value, data_, sizeof(T));
		EndianConvert(value);
		return skip(sizeof(T));
	}

	bool skipString(size_t maximumLength, bool allowEmpty)
	{
		for (size_t length = 0; length < remaining_; ++length)
		{
			const uint8 value = data_[length];
			if (value == 0)
			{
				if ((!allowEmpty && length == 0) || length > maximumLength)
					return false;

				return skip(length + 1);
			}

			// MemoryStream strings are ASCII and NUL terminated. Rejecting other bytes
			// here prevents the normal extractor from treating one byte as an implicit terminator.
			// MemoryStream 字符串使用 ASCII 与 NUL 结尾；提前拒绝其他字节，避免普通提取器把它误作隐式终止符。
			if (value > 0x7f || length >= maximumLength)
				return false;
		}

		return false;
	}

	bool skipBlob(size_t maximumLength)
	{
		if (remaining_ < sizeof(ArraySize))
			return false;

		ArraySize wireLength = 0;
		if (!read(wireLength) || wireLength > maximumLength)
			return false;

		return skip(static_cast<size_t>(wireLength));
	}

	bool empty() const { return remaining_ == 0; }

private:
	const uint8* data_;
	size_t remaining_;
};

bool validateCredentialStream(const MemoryStream& stream, bool hasAccountType)
{
	StreamCursor cursor(stream);
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
	StreamCursor cursor(stream);
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
