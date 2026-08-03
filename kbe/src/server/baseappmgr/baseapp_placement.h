#ifndef KBE_BASEAPPMGR_BASEAPP_PLACEMENT_H
#define KBE_BASEAPPMGR_BASEAPP_PLACEMENT_H

#include <cstddef>

namespace KBEngine
{

/**
 * 实时 CPU 负载与相对客户端份额共同决定新登录的 BaseApp 落点。
 * assignedClients 必须包含已确认客户端和待确认登录，避免异步状态上报覆盖预留后形成突发倾斜。
 * Combine live CPU load with relative client share when placing a new login.
 * assignedClients must include confirmed clients and pending logins so asynchronous
 * status reports cannot erase reservations and concentrate a burst on one BaseApp.
 */
inline double baseappPlacementScore(
	float load, std::size_t assignedClients, std::size_t totalAssignedClients, std::size_t appCount)
{
	if (totalAssignedClients == 0 || appCount == 0)
		return static_cast<double>(load);

	const double relativeClientPressure =
		static_cast<double>(assignedClients) * static_cast<double>(appCount) /
		static_cast<double>(totalAssignedClients);
	return static_cast<double>(load) + relativeClientPressure;
}

}

#endif
