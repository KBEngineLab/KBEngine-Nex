#ifndef KBE_TELEPORT_STATUS_H
#define KBE_TELEPORT_STATUS_H

#include "common/common.h"

namespace KBEngine::TeleportStatus
{
enum Value : uint8
{
	WAIT,
	READY,
	REJECTED,
	NOT_INSTALLED
};
}

#endif
