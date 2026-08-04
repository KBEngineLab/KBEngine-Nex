#ifndef KBE_NETWORK_RECEIVE_WINDOW_POLICY_H
#define KBE_NETWORK_RECEIVE_WINDOW_POLICY_H

#include "common/common.h"

namespace KBEngine
{
namespace Network
{

struct ReceiveWindowOverflowState
{
	ReceiveWindowOverflowState() : lastOverflowEpoch(0), consecutiveTicks(0) {}

	uint64 lastOverflowEpoch;
	uint32 consecutiveTicks;
};

enum ReceiveWindowOverflowDecision
{
	RECEIVE_WINDOW_OVERFLOW_ALREADY_RECORDED,
	RECEIVE_WINDOW_OVERFLOW_DEFER,
	RECEIVE_WINDOW_OVERFLOW_CONDEMN
};

inline ReceiveWindowOverflowDecision evaluateReceiveWindowOverflow(
	bool authenticated, uint64 currentEpoch, ReceiveWindowOverflowState& state)
{
	if (state.lastOverflowEpoch == currentEpoch)
		return RECEIVE_WINDOW_OVERFLOW_ALREADY_RECORDED;

	const bool consecutive = currentEpoch > state.lastOverflowEpoch &&
		currentEpoch - state.lastOverflowEpoch == 1;
	state.consecutiveTicks = consecutive ? state.consecutiveTicks + 1 : 1;
	state.lastOverflowEpoch = currentEpoch;

	// 未认证入口仍在首次突发时关闭；已绑定 Proxy 的连接只有连续三个活跃 Tick 超限才关闭。
	// Unauthenticated ingress still closes on the first burst; Proxy-bound clients close only after three consecutive active overflow ticks.
	if (!authenticated || state.consecutiveTicks >= 3)
		return RECEIVE_WINDOW_OVERFLOW_CONDEMN;

	return RECEIVE_WINDOW_OVERFLOW_DEFER;
}

}
}

#endif // KBE_NETWORK_RECEIVE_WINDOW_POLICY_H
