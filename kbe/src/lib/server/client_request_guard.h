/*
This source file is part of KBEngine.
*/

#ifndef KBE_SERVER_CLIENT_REQUEST_GUARD_H
#define KBE_SERVER_CLIENT_REQUEST_GUARD_H

#include "common/common.h"

namespace KBEngine
{
namespace Security
{

/**
 * Bind an entity ID carried by a client request to the authenticated Channel principal.
 * 将客户端请求中的实体 ID 绑定到已经认证的 Channel 身份。
 *
 * Keeping this check independent from Baseapp prevents account handlers from
 * accidentally trusting a payload ID when new operations are added.
 * 此检查与 Baseapp 解耦，避免后续新增账户操作时再次误信载荷中的实体 ID。
 */
inline bool isBoundClientEntity(ENTITY_ID proxyID, ENTITY_ID requestedEntityID) noexcept
{
	return proxyID > 0 && requestedEntityID > 0 && proxyID == requestedEntityID;
}

/**
 * Authorize the relationship between a client-owned source and a Cell RPC target.
 * 授权客户端所属来源实体与 Cell RPC 目标实体之间的关系。
 *
 * Self calls are always relationship-valid. Cross-entity calls must remain in
 * the same active Space and must target either an entity controlled by the
 * source or an entity currently visible to its Witness. Method-level policies
 * can narrow this transitional engine boundary further, but cannot widen it.
 * self 调用始终满足关系约束。跨实体调用必须位于同一有效 Space，并且目标要么
 * 由来源实体控制，要么当前处于其 Witness 视野内。后续方法级策略只能继续收紧，
 * 不能绕过这一引擎边界。
 */
inline bool isAuthorizedClientCellTarget(ENTITY_ID sourceEntityID,
	ENTITY_ID targetEntityID, SPACE_ID sourceSpaceID, SPACE_ID targetSpaceID,
	bool controlledBySource, bool inSourceView) noexcept
{
	if (sourceEntityID <= 0 || targetEntityID <= 0)
		return false;

	if (sourceEntityID == targetEntityID)
		return true;

	return sourceSpaceID > 0 && sourceSpaceID == targetSpaceID &&
		(controlledBySource || inSourceView);
}

}
}

#endif // KBE_SERVER_CLIENT_REQUEST_GUARD_H
