/*
This source file is part of KBEngine
For the latest info, see http://www.kbengine.org/
*/

#include "interfaces_payload_guard.h"
#include "bounded_stream_reader.h"
#include "network/common.h"
#include "server/common.h"
#include "server/server_errors.h"

namespace KBEngine{
namespace InterfacesPayloadGuard{
namespace
{

bool validateChargeFields(BoundedStreamReader& reader)
{
	DBID dbid = 0;
	CALLBACK_ID callbackID = 0;
	return reader.skipString(CHARGE_ID_MAX_LENGTH, false) &&
		reader.read(dbid) && dbid != 0 &&
		reader.skipBlob(NETWORK_MESSAGE_MAX_SIZE) &&
		reader.read(callbackID) && callbackID != 0;
}

}

bool validateChargeRequestStream(const MemoryStream& stream)
{
	BoundedStreamReader reader(stream);
	return validateChargeFields(reader) && reader.empty();
}

bool validateInterfacesChargeStream(const MemoryStream& stream)
{
	BoundedStreamReader reader(stream);
	COMPONENT_ID componentID = 0;
	return reader.read(componentID) && componentID != 0 &&
		validateChargeFields(reader) && reader.empty();
}

bool validateChargeCallbackStream(const MemoryStream& stream)
{
	BoundedStreamReader reader(stream);
	COMPONENT_ID componentID = 0;
	DBID dbid = 0;
	CALLBACK_ID callbackID = 0;
	SERVER_ERROR_CODE errorCode = SERVER_ERR_MAX;

	if (!reader.read(componentID) ||
		!reader.skipString(CHARGE_ID_MAX_LENGTH, false) ||
		!reader.read(dbid) ||
		!reader.skipBlob(NETWORK_MESSAGE_MAX_SIZE) ||
		!reader.read(callbackID) ||
		!reader.read(errorCode) || !reader.empty() || !isValidErrorCode(errorCode))
	{
		return false;
	}

	// An unknown provider order uses the historical all-zero route sentinel.
	// 未知平台订单保留历史上的全零路由哨兵，其他组合必须绑定完整回调路由。
	const bool isLostOrder = componentID == 0 && dbid == 0 && callbackID == 0;
	const bool isBoundOrder = componentID != 0 && dbid != 0 && callbackID != 0;
	return isLostOrder || isBoundOrder;
}

bool validateBaseappChargeCallbackStream(const MemoryStream& stream)
{
	BoundedStreamReader reader(stream);
	DBID dbid = 0;
	CALLBACK_ID callbackID = 0;
	SERVER_ERROR_CODE errorCode = SERVER_ERR_MAX;
	return reader.skipString(CHARGE_ID_MAX_LENGTH, false) &&
		reader.read(dbid) &&
		reader.skipBlob(NETWORK_MESSAGE_MAX_SIZE) &&
		reader.read(callbackID) &&
		reader.read(errorCode) && isValidErrorCode(errorCode) && reader.empty();
}

bool isValidChargeID(const std::string& value)
{
	return !value.empty() && value.size() <= CHARGE_ID_MAX_LENGTH;
}

bool isValidChargeData(const std::string& value)
{
	return value.size() <= NETWORK_MESSAGE_MAX_SIZE;
}

bool isValidClientRequestKey(const std::string& value)
{
	return !value.empty() && value.size() <= ACCOUNT_NAME_MAX_LENGTH;
}

bool isValidErrorCode(SERVER_ERROR_CODE value)
{
	return value < SERVER_ERR_MAX;
}

}
}
