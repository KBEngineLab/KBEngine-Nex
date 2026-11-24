#ifndef KBE_TELEPORT_STATUS_H
#define KBE_TELEPORT_STATUS_H

#include "server/bounded_stream_reader.h"

namespace KBEngine::TeleportStatus
{
enum Value : uint8
{
	WAIT,
	READY,
	REJECTED,
	NOT_INSTALLED
};

struct Dispatch
{
	COMPONENT_ID target = 0;
	SPACE_ID origin = 0;
	uint64 epoch = 0;
	uint64 targetProcess = 0;

	template<class Stream>
	void write(Stream& stream) const
	{
		stream << target << origin << epoch << targetProcess;
	}

	bool read(BoundedStreamReader& reader)
	{
		return reader.read(target) && target && reader.read(origin) && origin &&
			reader.read(epoch) && epoch && reader.read(targetProcess) && targetProcess;
	}
};
}

#endif
