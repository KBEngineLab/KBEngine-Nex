#ifndef KBE_CELLAPPMGR_CELLAPP_PLACEMENT_H
#define KBE_CELLAPPMGR_CELLAPP_PLACEMENT_H

#include <cstddef>
#include <cstdint>

namespace KBEngine
{

/**
 * 实时 CPU 负载与相对 Space 份额共同决定新 Space 的落点。
 * 已确认和待确认 Space 都必须计入 assignedSpaces，避免批量异步创建时重复选择同一 CellApp。
 * Combine live CPU load with relative Space share when placing a new Space.
 * assignedSpaces must include confirmed and pending Spaces so asynchronous bursts cannot target one CellApp repeatedly.
 */
inline double cellappPlacementScore(
	float load, std::size_t assignedSpaces, std::size_t totalAssignedSpaces, std::size_t appCount,
	std::uint64_t pendingWitnesses = 0, std::uint64_t activeWitnesses = 0,
	double witnessPendingWeight = 0.0)
{
	const double witnessPendingPressure = activeWitnesses == 0 ? 0.0 :
		static_cast<double>(pendingWitnesses) / static_cast<double>(activeWitnesses);
	if (totalAssignedSpaces == 0 || appCount == 0)
		return static_cast<double>(load) + witnessPendingPressure * witnessPendingWeight;

	const double relativeSpacePressure =
		static_cast<double>(assignedSpaces) * static_cast<double>(appCount) /
		static_cast<double>(totalAssignedSpaces);
	return static_cast<double>(load) + relativeSpacePressure +
		witnessPendingPressure * witnessPendingWeight;
}

inline bool cellappPlacementWithinSkew(
	std::size_t assignedSpaces, std::size_t minimumAssignedSpaces, std::size_t maximumSkew)
{
	return maximumSkew == 0 || assignedSpaces <= minimumAssignedSpaces ||
		assignedSpaces - minimumAssignedSpaces < maximumSkew;
}

}

#endif
