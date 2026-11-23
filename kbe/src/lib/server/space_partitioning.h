#ifndef KBE_SERVER_SPACE_PARTITIONING_H
#define KBE_SERVER_SPACE_PARTITIONING_H

#include "common/common.h"

namespace KBEngine {
namespace SpacePartitioning {

constexpr uint8 ENABLED = 1;
constexpr uint8 COORDINATE_SYSTEM = 2;
constexpr uint8 COORDINATE_SYSTEM_Y = 4;
constexpr uint8 KNOWN_FLAGS = ENABLED | COORDINATE_SYSTEM | COORDINATE_SYSTEM_Y;

inline uint8 modeFlags(bool enabled, bool coordinateSystem, bool hasY)
{
	return static_cast<uint8>((enabled ? ENABLED : 0) |
		(coordinateSystem ? COORDINATE_SYSTEM : 0) |
		(hasY ? COORDINATE_SYSTEM_Y : 0));
}

inline bool compatible(uint8 localMode, uint8 remoteMode)
{
	if ((remoteMode & ~KNOWN_FLAGS) != 0 ||
		((localMode ^ remoteMode) & ENABLED) != 0)
	{
		return false;
	}

	// 关闭分割时不限制旧项目的坐标设置；开启后，各节点必须维护一致的空间坐标。
	return (localMode & ENABLED) == 0 ||
		((localMode & COORDINATE_SYSTEM) != 0 && localMode == remoteMode);
}

}
}

#endif
