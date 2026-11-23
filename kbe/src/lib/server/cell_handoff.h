#ifndef KBE_CELL_HANDOFF_H
#define KBE_CELL_HANDOFF_H

#include "server/bounded_stream_reader.h"

namespace KBEngine::CellHandoff
{
enum Action : uint8
{
	NONE = 0,
	START,
	IDENTIFY_TARGET,
	TARGET_IDENTIFIED,
	DISPATCH,
	SOURCE_UNSENT,
	TARGET_INSTALLED,
	CHECK_TARGET,
	TARGET_READY,
	TARGET_REJECTED,
	RESTORE_SOURCE,
	SOURCE_RESTORED,
	RELEASE_SOURCE,
	SOURCE_RELEASED,
	FINISH_TARGET,
	TARGET_FINISHED
};

struct Route
{
	COMPONENT_ID source = 0;
	COMPONENT_ID target = 0;
	uint64 epoch = 0;
	uint32 sourcePid = 0;
	uint32 targetPid = 0;

	bool sameAttempt(const Route& other) const
	{
		return source == other.source && target == other.target && epoch == other.epoch &&
			sourcePid == other.sourcePid;
	}

	bool operator==(const Route& other) const
	{
		return sameAttempt(other) && targetPid == other.targetPid;
	}

	template<class Stream>
	void write(Stream& stream) const
	{
		stream << source << target << epoch << sourcePid << targetPid;
	}

	bool read(BoundedStreamReader& reader)
	{
		return reader.read(source) && reader.read(target) && reader.read(epoch) &&
			reader.read(sourcePid) && reader.read(targetPid) &&
			source != 0 && target != 0 && source != target && epoch != 0 && sourcePid != 0;
	}
};
}

#endif
