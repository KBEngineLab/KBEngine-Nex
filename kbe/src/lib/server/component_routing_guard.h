/*
This source file is part of KBEngine.
*/

#ifndef KBE_SERVER_COMPONENT_ROUTING_GUARD_H
#define KBE_SERVER_COMPONENT_ROUTING_GUARD_H

#include "common/common.h"

#include <cmath>

namespace KBEngine
{
namespace Network
{
class Channel;
}

namespace Security
{

/**
 * Convert component discovery output into a concrete message-routing target.
 * 将组件发现结果转换为确定的消息路由目标。
 *
 * Component ID 0 is a valid wildcard for service discovery, but accepting it
 * from a packet would silently select the first component. Keep this guard
 * independent from Components so malformed-ID behavior remains cheap to test.
 * 组件 ID 0 在服务发现中是合法通配符，但来自封包时会静默选中第一个组件。
 * 守卫与 Components 解耦，使畸形 ID 行为可以低成本独立回归。
 */
template<typename ComponentInfosT>
Network::Channel* concreteComponentChannel(COMPONENT_ID componentID, ComponentInfosT* infos) noexcept
{
	if (componentID == 0 || infos == NULL)
		return NULL;

	return infos->pChannel;
}

/** Verify that a payload-reported component is bound to the actual source Channel.
 *  验证载荷声明的组件确实绑定到实际来源 Channel。
 */
template<typename ComponentInfosT>
bool isBoundComponentSource(COMPONENT_ID componentID, ComponentInfosT* infos,
	const Network::Channel* sourceChannel) noexcept
{
	return sourceChannel != NULL &&
		concreteComponentChannel(componentID, infos) == sourceChannel;
}

/** Verify a peer on either side of the engine's historical dual connection.
 *  验证引擎历史双连接中任一方向上的对端身份。
 *
 * EntityApps keep one inbound Channel registered in ComponentInfos while also
 * creating an outbound Channel whose component ID is assigned locally during
 * connectComponent(). Replies can arrive on that outbound Channel. The second
 * binding is safe only because it is local connection state, not a packet field.
 * EntityApp 在 ComponentInfos 中保存一条入站 Channel，同时还会主动建立一条由
 * connectComponent() 在本地写入组件 ID 的出站 Channel。响应可从该出站 Channel
 * 返回；第二种绑定之所以可信，是因为它来自本地连接状态而非网络载荷字段。
 */
template<typename ComponentInfosT>
bool isBoundBidirectionalComponentSource(COMPONENT_ID componentID,
	ComponentInfosT* infos, const Network::Channel* sourceChannel,
	COMPONENT_ID locallyBoundChannelComponentID) noexcept
{
	if (componentID == 0 || infos == NULL || sourceChannel == NULL)
		return false;

	return isBoundComponentSource(componentID, infos, sourceChannel) ||
		locallyBoundChannelComponentID == componentID;
}

/** Verify both the registered component type and the concrete source Channel.
 *  同时验证已注册组件类型和具体来源 Channel。
 */
template<typename ComponentInfosT>
bool isExpectedComponentSource(COMPONENT_TYPE expectedType, ComponentInfosT* infos,
	const Network::Channel* sourceChannel) noexcept
{
	return infos != NULL && infos->componentType == expectedType &&
		isBoundComponentSource(infos->cid, infos, sourceChannel);
}

/** Reject NaN, infinity and negative load/progress values before state mutation.
 *  在修改状态前拒绝 NaN、无穷大和负载/进度负值。
 */
template<typename FloatT>
bool isValidComponentMetric(FloatT value) noexcept
{
	return std::isfinite(value) && value >= static_cast<FloatT>(0);
}

/** Database entity operations require a concrete persistent identifier.
 *  数据库实体操作必须携带确定且非零的持久化标识。
 */
inline bool isValidPersistentEntityID(DBID entityDBID) noexcept
{
	return entityDBID > 0;
}

/** Keep packet-carried query modes inside the protocol's defined range.
 *  将封包携带的查询模式限制在协议已定义的范围内。
 */
inline bool isValidDatabaseQueryMode(int8 queryMode) noexcept
{
	return queryMode >= 0 && queryMode <= 2;
}

}
}

#endif // KBE_SERVER_COMPONENT_ROUTING_GUARD_H
