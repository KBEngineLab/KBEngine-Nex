#ifndef KBE_SERVER_CELL_RECOVERY_H
#define KBE_SERVER_CELL_RECOVERY_H

#include "server/bounded_stream_reader.h"
#include "entitydef/common.h"

#include <map>
#include <random>

namespace KBEngine::CellRecovery
{
struct CellBackup;

constexpr uint16 MAX_SPACES_PER_QUERY = 64;

enum class Scope : uint8
{
	UNKNOWN,
	LEGACY,
	MANAGED,
	RETIRED,
	CONFIRMED_EXIT,
	REMOVED
};

struct LostReal
{
	COMPONENT_ID cellapp;
	uint64 cellSession;
	SPACE_ID space;
	uint64 layoutVersion;
	uint64 managerSession;

	bool operator==(const LostReal& other) const
	{
		return cellapp == other.cellapp && cellSession == other.cellSession &&
			space == other.space && layoutVersion == other.layoutVersion &&
			managerSession == other.managerSession;
	}
};

struct ScopeHeader
{
	COMPONENT_ID lost = 0;
	uint64 session = 0;

	bool readHeader(BoundedStreamReader& reader, uint16& count)
	{
		return reader.read(lost) && lost && reader.read(session) && session &&
			reader.read(count) && count && count <= MAX_SPACES_PER_QUERY;
	}
};

struct Query : ScopeHeader
{
	std::vector<SPACE_ID> spaces;

	bool read(BoundedStreamReader& reader)
	{
		uint16 count = 0;
		if (!readHeader(reader, count))
			return false;
		spaces.resize(count);
		SPACE_ID previous = 0;
		for (auto& space : spaces)
		{
			if (!reader.read(space) || space <= previous)
				return false;
			previous = space;
		}
		return reader.empty();
	}

};

struct ScopeResult
{
	SPACE_ID space = 0;
	Scope scope = Scope::UNKNOWN;
	uint64 layoutVersion = 0;
	COMPONENT_ID replacement = 0;
	uint64 replacementVersion = 0;
	bool committed = false;
	uint64 replacementProcess = 0;
	bool replacementExited = false;

	template<class Stream>
	void write(Stream& stream) const
	{
		stream << space << static_cast<uint8>(scope) << layoutVersion << replacement << replacementVersion
			<< static_cast<uint8>(committed) << replacementProcess << static_cast<uint8>(replacementExited);
	}

	bool read(BoundedStreamReader& reader)
	{
		uint8 value = 0;
		uint8 active = 0, exited = 0;
		if (!reader.read(space) || !space || !reader.read(value) ||
			value > static_cast<uint8>(Scope::REMOVED) || !reader.read(layoutVersion) ||
			!reader.read(replacement) || !reader.read(replacementVersion) || !reader.read(active) ||
			!reader.read(replacementProcess) || !reader.read(exited) || active > 1 || exited > 1 ||
			(active && (!replacement || !replacementProcess || exited)) || (exited && replacement))
			return false;
		committed = active != 0;
		replacementExited = exited != 0;
		scope = static_cast<Scope>(value);
		return (scope == Scope::CONFIRMED_EXIT) == (layoutVersion != 0) &&
			(!replacementExited || layoutVersion) && (!replacementProcess || replacement) &&
			(replacement ? layoutVersion && replacementVersion > layoutVersion : !replacementVersion);
	}
};

struct Reply : ScopeHeader
{
	std::vector<ScopeResult> scopes;

	bool read(BoundedStreamReader& reader)
	{
		uint16 count = 0;
		if (!readHeader(reader, count))
			return false;
		scopes.resize(count);
		SPACE_ID previous = 0;
		for (auto& scope : scopes)
		{
			if (!scope.read(reader) || scope.space <= previous)
				return false;
			previous = scope.space;
		}
		return reader.empty();
	}
};

constexpr size_t MAX_STAGED_BACKUP_BYTES = MemoryStream::MAX_SIZE - 128;
enum class RestoreAction : uint8 { Stage, Install, Activate, Discard };

inline uint64 recoveryProcessID()
{
	static const uint64 id = []()
	{
		std::random_device random;
		uint64 value = 0;
		do
		{
			value = (uint64(static_cast<uint32>(random())) << 32) | static_cast<uint32>(random());
		} while (!value);
		return value;
	}();
	return id;
}

struct RecoveryProcesses
{
	uint64 base = 0;
	uint64 target = 0;

	template<class Stream>
	void write(Stream& stream) const { stream << base << target; }

	bool read(BoundedStreamReader& reader) { return reader.read(base) && base && reader.read(target); }
};

struct StageKey
{
	COMPONENT_ID lost = 0;
	SPACE_ID space = 0;
	uint64 originalVersion = 0;
	uint64 replacementVersion = 0;
	ENTITY_ID entity = 0;

	bool operator==(const StageKey& other) const
	{
		return lost == other.lost && space == other.space && originalVersion == other.originalVersion &&
			replacementVersion == other.replacementVersion && entity == other.entity;
	}

	template<class Stream>
	void write(Stream& stream) const
	{
		stream << lost << space << originalVersion << replacementVersion << entity;
	}

	bool read(BoundedStreamReader& reader)
	{
		return reader.read(lost) && lost && reader.read(space) && space &&
			reader.read(originalVersion) && originalVersion &&
			reader.read(replacementVersion) && replacementVersion > originalVersion &&
			reader.read(entity) && entity > 0;
	}
};

class StagedBackups
{
public:
	bool stage(const StageKey& key, COMPONENT_ID base, ENTITY_SCRIPT_UID type, std::string data)
	{
		if (data.empty() || data.size() > MAX_STAGED_BACKUP_BYTES)
			return false;

		const auto attempt = std::make_pair(key.space, key.replacementVersion);
		const auto space = spaces_.find(attempt);
		if (space != spaces_.end())
		{
			const auto entity = space->second.find(key.entity);
			if (entity != space->second.end())
				return entity->second.base == base && entity->second.type == type && entity->second.data == data;
		}
		if (count_ >= MAX_ENTITIES || data.size() > MAX_BYTES - bytes_)
			return false;

		bytes_ += data.size();
		++count_;
		spaces_[attempt].emplace(key.entity, Entry{base, type, std::move(data)});
		return true;
	}

	void erase(SPACE_ID space, uint64 version)
	{
		const auto attempt = spaces_.find({space, version});
		if (attempt == spaces_.end())
			return;
		for (const auto& entry : attempt->second)
			bytes_ -= entry.second.data.size();
		count_ -= attempt->second.size();
		spaces_.erase(attempt);
	}

	size_t size() const { return count_; }

private:
	static constexpr size_t MAX_ENTITIES = 65536;
	static constexpr size_t MAX_BYTES = 64 * 1024 * 1024;

	struct Entry
	{
		COMPONENT_ID base;
		ENTITY_SCRIPT_UID type;
		std::string data;
	};
	std::map<std::pair<SPACE_ID, uint64>, std::map<ENTITY_ID, Entry>> spaces_;
	size_t bytes_ = 0;
	size_t count_ = 0;
};
}

#endif
