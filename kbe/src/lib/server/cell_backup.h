#ifndef KBE_SERVER_CELL_BACKUP_H
#define KBE_SERVER_CELL_BACKUP_H

#include "server/bounded_stream_reader.h"

#include <array>

namespace KBEngine::CellRecovery
{
using BackupDigest = std::array<uint32, 5>;

struct BackupStamp
{
	SPACE_ID space = 0;
	uint64 generation = 0;
	uint64 request = 0;

	template<class Stream>
	void write(Stream& stream) const
	{
		stream << space << generation << request;
	}

	bool read(BoundedStreamReader& reader)
	{
		return reader.read(space) && space && reader.read(generation) && generation &&
			reader.read(request) && request;
	}

	bool operator==(const BackupStamp& other) const
	{
		return space == other.space && generation == other.generation && request == other.request;
	}
};

inline bool readBackupDigest(BoundedStreamReader& reader, BackupDigest& digest)
{
	for (auto& word : digest)
		if (!reader.read(word))
			return false;
	return true;
}

template<class Stream>
void writeBackupDigest(Stream& stream, const BackupDigest& digest)
{
	for (const auto word : digest)
		stream << word;
}

struct CellBackup
{
	COMPONENT_ID cell = 0;
	uint64 session = 0;
	SPACE_ID space = 0;
	uint64 generation = 0;
	uint64 request = 0;
	uint64 retryAt = 0;
	BackupDigest digest{};
	std::vector<uint8> data;

	BackupStamp stamp() const { return {space, generation, request}; }

	bool matches(COMPONENT_ID source, uint64 connection, const BackupStamp& stamp) const
	{
		return request && cell == source && session == connection && this->stamp() == stamp;
	}

	bool owns(COMPONENT_ID source, uint64 connection, SPACE_ID currentSpace, uint64 currentGeneration) const
	{
		return cell == source && session == connection && space == currentSpace && generation == currentGeneration;
	}
};
}

#endif
