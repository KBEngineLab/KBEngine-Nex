#ifndef KBE_SERVER_SCRIPT_STAGE_TIMING_H
#define KBE_SERVER_SCRIPT_STAGE_TIMING_H

#include "common/timestamp.h"
#include "server/script_stage_metrics.h"

namespace KBEngine
{

inline uint64 scriptStageStartStamps()
{
	return g_performanceProbesEnabled ? timestamp() : 0;
}

inline uint64 scriptStageDurationNanos(uint64 startStamps)
{
	if (!g_performanceProbesEnabled)
		return 0;

	return static_cast<uint64>(
		(static_cast<double>(timestamp() - startStamps) * 1000000000.0) / stampsPerSecondD());
}

}

#endif // KBE_SERVER_SCRIPT_STAGE_TIMING_H
