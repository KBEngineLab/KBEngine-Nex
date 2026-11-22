#ifndef KBE_SERVER_SPACE_LAYOUT_H
#define KBE_SERVER_SPACE_LAYOUT_H

#include "server/bounded_stream_reader.h"

#include <cmath>
#include <limits>
#include <set>

namespace KBEngine {
namespace SpaceLayout {

constexpr size_t MAX_CELLS = 64;
constexpr size_t MAX_NODES = MAX_CELLS * 2 - 1;
constexpr size_t MAX_DEPTH = 32;
constexpr size_t MAX_PACKET_BYTES = 2 * 1024 * 1024;
constexpr size_t MAX_DATA_ENTRIES = 1024;
constexpr size_t MAX_VALUE_BYTES = 64 * 1024;

enum Axis : uint8 { LEAF = 0, X = 1, Z = 2 };
enum State : uint8 { NONE = 0, PREPARING, PREPARED, COMMITTING, ACTIVE, FAILED, RELEASING };
enum Operation : uint8
{
	REQUEST = 1, PREPARE, READY, COMMIT, COMMITTED, COMPLETE,
	ABORT, SHUTDOWN, REMOVE, REMOVED, SUSPEND, SET_DATA, DELETE_DATA, RESOURCES, UPDATE_REJECTED
};

inline const char* stateName(State state)
{
	switch (state)
	{
	case NONE: return "none";
	case PREPARING: return "preparing";
	case PREPARED: return "prepared";
	case COMMITTING: return "committing";
	case ACTIVE: return "active";
	case FAILED: return "failed";
	case RELEASING: return "releasing";
	}
	return "unknown";
}

struct Bounds
{
	float minX = -std::numeric_limits<float>::infinity();
	float minZ = -std::numeric_limits<float>::infinity();
	float maxX = std::numeric_limits<float>::infinity();
	float maxZ = std::numeric_limits<float>::infinity();
};

struct Node
{
	uint8 axis = LEAF;
	float position = 0.f;
	CELL_ID cellID = 0;
	COMPONENT_ID cellappID = 0;
};

// 前序保存 BSP，分支后依次为左右子树；边界使用 <，与 BigWorld 的位置查询规则一致。
struct Layout
{
	COMPONENT_ID owner = 0;
	std::vector<Node> nodes;

	bool validate() const
	{
		if (owner == 0 || nodes.empty() || nodes.size() > MAX_NODES)
			return false;
		rightChildren_.assign(nodes.size(), 0);
		size_t cursor = 0;
		std::set<CELL_ID> cells;
		std::set<COMPONENT_ID> apps;
		return validateTree(cursor, Bounds(), 0, cells, apps) &&
			cursor == nodes.size() && apps.count(owner) == 1;
	}

	const Node* cellForApp(COMPONENT_ID cid) const
	{
		for (const Node& node : nodes)
			if (node.axis == LEAF && node.cellappID == cid)
				return &node;
		return NULL;
	}

	const Node* cellAt(float x, float z) const
	{
		if (!std::isfinite(x) || !std::isfinite(z) || nodes.empty() ||
			rightChildren_.size() != nodes.size())
			return NULL;
		size_t cursor = 0;
		return findAt(cursor, x, z);
	}

	std::set<COMPONENT_ID> participants() const
	{
		std::set<COMPONENT_ID> result;
		for (const Node& node : nodes)
			if (node.axis == LEAF)
				result.insert(node.cellappID);
		return result;
	}

	bool boundsForApp(COMPONENT_ID cid, Bounds& result) const
	{
		size_t cursor = 0;
		return findBounds(cursor, Bounds(), cid, result);
	}

	void write(MemoryStream& stream) const
	{
		stream << owner << static_cast<uint16>(nodes.size());
		for (const Node& node : nodes)
			stream << node.axis << node.position << node.cellID << node.cellappID;
	}

	bool read(BoundedStreamReader& reader)
	{
		uint16 count = 0;
		if (!reader.read(owner) || !reader.read(count) || count == 0 || count > MAX_NODES)
			return false;
		nodes.resize(count);
		for (Node& node : nodes)
			if (!reader.read(node.axis) || !reader.read(node.position) ||
				!reader.read(node.cellID) || !reader.read(node.cellappID))
				return false;
		return validate();
	}

private:
	mutable std::vector<uint16> rightChildren_;

	bool validateTree(size_t& cursor, Bounds bounds, size_t depth,
		std::set<CELL_ID>& cells, std::set<COMPONENT_ID>& apps) const
	{
		if (cursor >= nodes.size() || depth > MAX_DEPTH)
			return false;
		const size_t index = cursor;
		const Node& node = nodes[cursor++];
		if (node.axis == LEAF)
			return node.cellID != 0 && node.cellappID != 0 && node.position == 0.f &&
				cells.insert(node.cellID).second && apps.insert(node.cellappID).second;
		if ((node.axis != X && node.axis != Z) || node.cellID != 0 ||
			node.cellappID != 0 || !std::isfinite(node.position))
			return false;
		const float minimum = node.axis == X ? bounds.minX : bounds.minZ;
		const float maximum = node.axis == X ? bounds.maxX : bounds.maxZ;
		if (node.position <= minimum || node.position >= maximum)
			return false;
		Bounds left = bounds, right = bounds;
		if (node.axis == X)
		{
			left.maxX = node.position;
			right.minX = node.position;
		}
		else
		{
			left.maxZ = node.position;
			right.minZ = node.position;
		}
		if (!validateTree(cursor, left, depth + 1, cells, apps))
			return false;
		rightChildren_[index] = static_cast<uint16>(cursor);
		return validateTree(cursor, right, depth + 1, cells, apps);
	}

	const Node* findAt(size_t& cursor, float x, float z) const
	{
		if (cursor >= nodes.size())
			return NULL;
		const size_t index = cursor;
		const Node& node = nodes[cursor++];
		if (node.axis == LEAF)
			return &node;
		if ((node.axis == X ? x : z) < node.position)
			return findAt(cursor, x, z);
		cursor = rightChildren_[index];
		return findAt(cursor, x, z);
	}

	bool findBounds(size_t& cursor, Bounds bounds, COMPONENT_ID cid, Bounds& result) const
	{
		if (cursor >= nodes.size())
			return false;
		const Node& node = nodes[cursor++];
		if (node.axis == LEAF)
		{
			if (node.cellappID != cid)
				return false;
			result = bounds;
			return true;
		}
		Bounds right = bounds;
		if (node.axis == X)
		{
			bounds.maxX = node.position;
			right.minX = node.position;
		}
		else
		{
			bounds.maxZ = node.position;
			right.minZ = node.position;
		}
		return findBounds(cursor, bounds, cid, result) || findBounds(cursor, right, cid, result);
	}
};

struct Resources
{
	std::string scriptModule;
	SPACE_DATA data;
	std::string geometryPath;
	bool loadGeometry = false;
	std::map<int, std::string> geometryParams;

	void setGeometry(const std::string& path, bool load, const std::map<int, std::string>& params)
	{
		geometryPath = path;
		loadGeometry = load;
		geometryParams = params;
		if (geometryPath.empty())
			data.erase("_mapping");
		else
			data["_mapping"] = geometryPath;
	}

	bool validate() const
	{
		if (scriptModule.empty() || scriptModule.size() > 256 || geometryPath.size() > 4096 ||
			scriptModule.find('\0') != std::string::npos || geometryPath.find('\0') != std::string::npos ||
			geometryParams.size() > 64 || data.size() > MAX_DATA_ENTRIES ||
			(loadGeometry && geometryPath.empty()))
			return false;
		// 预留布局和消息头的空间，发送前就限制总大小，不先生成任意长度的临时缓冲。
		size_t wireBytes = 8192 + scriptModule.size() + geometryPath.size();
		for (const auto& entry : geometryParams)
		{
			if (entry.second.size() > 4096 || entry.second.find('\0') != std::string::npos)
				return false;
			wireBytes += sizeof(int32) + sizeof(ArraySize) + entry.second.size();
		}
		for (const auto& entry : data)
		{
			if (entry.first.empty() || entry.first.size() > 256 ||
				entry.first.find('\0') != std::string::npos || entry.second.size() > MAX_VALUE_BYTES)
				return false;
			wireBytes += sizeof(ArraySize) * 2 + entry.first.size() + entry.second.size();
		}
		const auto mapping = data.find("_mapping");
		return wireBytes <= MAX_PACKET_BYTES && (geometryPath.empty() ?
			(mapping == data.end() || mapping->second.empty()) :
			(mapping != data.end() && mapping->second == geometryPath));
	}

	void write(MemoryStream& stream) const
	{
		stream.appendBlob(scriptModule);
		stream.appendBlob(geometryPath);
		stream << static_cast<uint8>(loadGeometry) << static_cast<uint16>(geometryParams.size());
		for (const auto& entry : geometryParams)
		{
			stream << static_cast<int32>(entry.first);
			stream.appendBlob(entry.second);
		}
		stream << static_cast<uint16>(data.size());
		for (const auto& entry : data)
		{
			stream.appendBlob(entry.first);
			stream.appendBlob(entry.second);
		}
	}

	bool read(BoundedStreamReader& reader)
	{
		uint8 load = 0;
		uint16 count = 0;
		if (!reader.readBlob(scriptModule, 256) || scriptModule.empty() ||
			!reader.readBlob(geometryPath, 4096) || !reader.read(load) || load > 1 ||
			!reader.read(count) || count > 64 || (load != 0 && geometryPath.empty()))
			return false;
		loadGeometry = load != 0;
		for (uint16 i = 0; i < count; ++i)
		{
			int32 key = 0;
			std::string value;
			if (!reader.read(key) || !reader.readBlob(value, 4096) ||
				!geometryParams.emplace(key, value).second)
				return false;
		}
		if (!reader.read(count) || count > MAX_DATA_ENTRIES)
			return false;
		for (uint16 i = 0; i < count; ++i)
		{
			std::string key, value;
			if (!reader.readBlob(key, 256) || key.empty() ||
				!reader.readBlob(value, MAX_VALUE_BYTES) || !data.emplace(key, value).second)
				return false;
		}
		return validate();
	}
};

struct Packet
{
	COMPONENT_ID sender = 0;
	uint8 operation = 0;
	uint8 rejectedOperation = 0;
	SPACE_ID spaceID = 0;
	uint64 version = 0;
	uint64 revision = 0;
	Layout layout;
	Resources resources;
	bool success = true;
	std::string error, key, value;

	void write(MemoryStream& stream) const
	{
		stream << sender << operation << spaceID << version << revision;
		switch (operation)
		{
		case REQUEST: case PREPARE:
			layout.write(stream);
			resources.write(stream);
			break;
		case READY: case COMMITTED: case ABORT: case SUSPEND:
			stream << static_cast<uint8>(success);
			stream.appendBlob(error);
			break;
		case SET_DATA: case DELETE_DATA:
			stream.appendBlob(key);
			if (operation == SET_DATA)
				stream.appendBlob(value);
			break;
		case RESOURCES:
			resources.write(stream);
			break;
		case UPDATE_REJECTED:
			stream << rejectedOperation;
			if (rejectedOperation == SET_DATA || rejectedOperation == DELETE_DATA)
				stream.appendBlob(key);
			stream.appendBlob(error);
			break;
		default:
			break;
		}
	}

	bool read(const MemoryStream& stream)
	{
		if (stream.length() > MAX_PACKET_BYTES)
			return false;
		BoundedStreamReader reader(stream);
		if (!reader.read(sender) || sender == 0 || !reader.read(operation) ||
			!reader.read(spaceID) || spaceID == 0 || !reader.read(version) || version == 0 ||
			!reader.read(revision))
			return false;
		uint8 accepted = 0;
		switch (operation)
		{
		case REQUEST: case PREPARE:
			if (!layout.read(reader) || !resources.read(reader))
				return false;
			break;
		case READY: case COMMITTED: case ABORT: case SUSPEND:
			if (!reader.read(accepted) || accepted > 1 || !reader.readBlob(error, 1024))
				return false;
			success = accepted != 0;
			break;
		case SET_DATA: case DELETE_DATA:
			if (!reader.readBlob(key, 256) || key.empty() || key == "_mapping" ||
				(operation == SET_DATA && !reader.readBlob(value, MAX_VALUE_BYTES)))
				return false;
			break;
		case RESOURCES:
			if (!resources.read(reader))
				return false;
			break;
		case UPDATE_REJECTED:
			if (!reader.read(rejectedOperation))
				return false;
			if (rejectedOperation == SET_DATA || rejectedOperation == DELETE_DATA)
			{
				if (!reader.readBlob(key, 256) || key.empty() || key == "_mapping")
					return false;
			}
			else if (rejectedOperation != RESOURCES)
				return false;
			if (!reader.readBlob(error, 1024) || error.empty())
				return false;
			break;
		case COMMIT: case COMPLETE: case SHUTDOWN: case REMOVE: case REMOVED:
			break;
		default:
			return false;
		}
		return reader.empty();
	}
};

// 管理器只统计当前版本、当前阶段的参与者确认，重复确认不会推进下一阶段。
struct Transaction
{
	enum class Removal { Ignore, Erase, Fail };

	Layout layout;
	Resources resources;
	uint64 version = 0;
	uint64 revision = 1;
	uint64 started = 0;
	State state = PREPARING;
	bool suspended = false;
	std::string error;
	std::set<COMPONENT_ID> acknowledged;

	bool confirm(COMPONENT_ID cid, uint64 confirmedVersion, State expected)
	{
		if (confirmedVersion != version || state != expected || !layout.cellForApp(cid))
			return false;
		acknowledged.insert(cid);
		return acknowledged.size() == layout.participants().size();
	}

	Removal recordRemoval(COMPONENT_ID cid, uint64 removedVersion)
	{
		if (removedVersion != version || !layout.cellForApp(cid))
			return Removal::Ignore;
		if (state == RELEASING)
			return confirm(cid, removedVersion, RELEASING) ? Removal::Erase : Removal::Ignore;
		if (state == FAILED)
			return !suspended && cid == layout.owner ? Removal::Erase : Removal::Ignore;
		return Removal::Fail;
	}

	void advance(State next, uint64 now)
	{
		state = next;
		started = now;
		acknowledged.clear();
	}
};

}
}
#endif
