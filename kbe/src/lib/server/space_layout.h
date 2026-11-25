#ifndef KBE_SERVER_SPACE_LAYOUT_H
#define KBE_SERVER_SPACE_LAYOUT_H

#include "server/bounded_stream_reader.h"

#include <algorithm>
#include <optional>

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
	ABORT, SHUTDOWN, REMOVE, REMOVED, SUSPEND, SET_DATA, DELETE_DATA, RESOURCES, UPDATE_REJECTED,
	REPLACEMENT_PREPARE, REPLACEMENT_READY, REPLACEMENT_CANCEL,
	REPLACEMENT_CHECK, REPLACEMENT_CHECKED, REPLACEMENT_COMMIT,
	BOUNDARY_REQUEST, BOUNDARY_APPLY, BOUNDARY_APPLIED, BOUNDARY_RESULT,
	SPLIT_PREPARE, SPLIT_STAGE, SPLIT_READY, SPLIT_APPLY, SPLIT_APPLIED, SPLIT_COMPLETE, SPLIT_CANCEL, SPLIT_DRAINED,
	SPLIT_DISCARDED, SPLIT_CHECK_TARGET
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

	bool crossedBy(float x, float z, float margin) const
	{
		if (!std::isfinite(x) || !std::isfinite(z))
			return false;
		// 分界线归右侧或 Z 值较大的一侧，和 cellAt 的选择一致。
		return (x < minX && static_cast<double>(minX) - x >= margin) ||
			(x >= maxX && static_cast<double>(x) - maxX >= margin) ||
			(z < minZ && static_cast<double>(minZ) - z >= margin) ||
			(z >= maxZ && static_cast<double>(z) - maxZ >= margin);
	}
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

	bool replaceCellApp(COMPONENT_ID lost, COMPONENT_ID target)
	{
		if (!lost || !target || cellForApp(target))
			return false;
		for (Node& node : nodes)
		{
			if (node.axis != LEAF || node.cellappID != lost)
				continue;
			node.cellappID = target;
			if (owner == lost)
				owner = target;
			// 分区边界和 Cell 编号不变，存活节点仍按原编号核对迁移。
			return true;
		}
		return false;
	}

	bool moveBoundary(uint16 index, float position)
	{
		if (index >= nodes.size() || nodes[index].axis == LEAF ||
			!std::isfinite(position) || nodes[index].position == position)
			return false;
		// 移动上层边界也可能使下层分区无效，必须检查整棵树。
		Layout next = *this;
		next.nodes[index].position = position;
		if (!next.validate())
			return false;
		*this = std::move(next);
		return true;
	}

	bool splitCell(CELL_ID source, uint8 axis, float position, COMPONENT_ID target)
	{
		if ((axis != X && axis != Z) || !std::isfinite(position) || !target ||
			nodes.size() + 2 > MAX_NODES || cellForApp(target))
			return false;
		const auto leaf = std::find_if(nodes.begin(), nodes.end(),
			[source](const Node& node) { return node.axis == LEAF && node.cellID == source; });
		if (leaf == nodes.end())
			return false;
		CELL_ID last = 0;
		for (const auto& node : nodes)
			last = std::max(last, node.cellID);
		if (last == std::numeric_limits<CELL_ID>::max())
			return false;
		Layout next = *this;
		const auto index = leaf - nodes.begin();
		// 原 Cell 保留编号和坐标较小的一侧，新 Cell 使用尚未分配的编号。
		next.nodes[index] = Node{axis, position};
		next.nodes.insert(next.nodes.begin() + index + 1, {*leaf, Node{LEAF, 0.f, last + 1, target}});
		if (!next.validate())
			return false;
		*this = std::move(next);
		return true;
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

struct CellPeerRoute
{
	uint32 internalIP = 0, externalIP = 0, pid = 0;
	uint16 internalPort = 0, externalPort = 0;
	COMPONENT_ORDER globalOrder = 0, groupOrder = 0;
	COMPONENT_GUS gus = 0;
	std::string username, externalAddress;

	void write(MemoryStream& stream) const
	{
		stream << internalIP << internalPort << externalIP << externalPort
			<< pid << globalOrder << groupOrder << gus;
		stream.appendBlob(username);
		stream.appendBlob(externalAddress);
	}

	bool read(BoundedStreamReader& reader)
	{
		return reader.read(internalIP) && internalIP != 0 &&
			reader.read(internalPort) && internalPort != 0 &&
			reader.read(externalIP) && reader.read(externalPort) &&
			reader.read(pid) && reader.read(globalOrder) && reader.read(groupOrder) && reader.read(gus) &&
			reader.readBlob(username, MAX_NAME) && username.find('\0') == std::string::npos &&
			reader.readBlob(externalAddress, MAX_NAME) && externalAddress.find('\0') == std::string::npos;
	}
};

struct Packet
{
	COMPONENT_ID sender = 0;
	uint8 operation = 0;
	uint8 rejectedOperation = 0;
	SPACE_ID spaceID = 0;
	uint64 version = 0;
	// 调整边界的消息使用边界修订号，其余消息仍使用资源修订号。
	uint64 revision = 0;
	uint16 boundaryIndex = 0;
	float boundaryPosition = 0.f;
	uint64 splitAttempt = 0;
	COMPONENT_ID splitSource = 0;
	COMPONENT_ID splitTarget = 0;
	COMPONENT_ID replacedCellApp = 0;
	uint64 replacedLayoutVersion = 0;
	COMPONENT_ID replacementCellApp = 0;
	uint64 replacementProcess = 0;
	Layout layout;
	Resources resources;
	std::map<COMPONENT_ID, CellPeerRoute> cellPeerRoutes;
	bool success = true;
	std::string error, key, value;

	void write(MemoryStream& stream) const
	{
		stream << sender << operation << spaceID << version << revision;
		switch (operation)
		{
		case REPLACEMENT_CHECK: case REPLACEMENT_CHECKED: case REPLACEMENT_COMMIT:
			stream << replacedCellApp << replacedLayoutVersion << replacementCellApp;
			break;
		case REPLACEMENT_PREPARE:
			stream << replacedCellApp << replacedLayoutVersion;
			[[fallthrough]];
		case REQUEST: case PREPARE:
			layout.write(stream);
			resources.write(stream);
			break;
		case SPLIT_PREPARE: case SPLIT_STAGE:
			stream << splitAttempt << splitSource << splitTarget;
			layout.write(stream);
			if (operation == SPLIT_PREPARE)
				resources.write(stream);
			[[fallthrough]];
		case COMPLETE:
			stream << static_cast<uint16>(cellPeerRoutes.size());
			for (const auto& entry : cellPeerRoutes)
			{
				stream << entry.first;
				entry.second.write(stream);
			}
			break;
		case REPLACEMENT_READY:
			stream << replacementProcess;
			[[fallthrough]];
		case SPLIT_READY:
			if (operation == SPLIT_READY)
				stream << splitAttempt;
			[[fallthrough]];
		case READY: case COMMITTED: case ABORT: case SUSPEND: case BOUNDARY_RESULT:
			stream << static_cast<uint8>(success);
			stream.appendBlob(error);
			break;
		case BOUNDARY_REQUEST: case BOUNDARY_APPLY:
			stream << boundaryIndex << boundaryPosition;
			break;
		case SPLIT_CANCEL:
			stream << splitTarget;
			[[fallthrough]];
		case SPLIT_APPLY: case SPLIT_APPLIED: case SPLIT_COMPLETE: case SPLIT_DRAINED: case SPLIT_DISCARDED:
		case SPLIT_CHECK_TARGET:
			stream << splitAttempt;
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
		case REPLACEMENT_CHECK: case REPLACEMENT_CHECKED: case REPLACEMENT_COMMIT:
			if (!reader.read(replacedCellApp) || !replacedCellApp ||
				!reader.read(replacedLayoutVersion) || !replacedLayoutVersion || replacedLayoutVersion >= version ||
				!reader.read(replacementCellApp) || !replacementCellApp || replacementCellApp == replacedCellApp)
				return false;
			break;
		case REPLACEMENT_PREPARE:
			if (!reader.read(replacedCellApp) || !replacedCellApp ||
				!reader.read(replacedLayoutVersion) || !replacedLayoutVersion || replacedLayoutVersion >= version)
				return false;
			[[fallthrough]];
		case REQUEST: case PREPARE:
			if (!layout.read(reader) || !resources.read(reader))
				return false;
			if (operation == REPLACEMENT_PREPARE && layout.cellForApp(replacedCellApp))
				return false;
			break;
		case SPLIT_PREPARE: case SPLIT_STAGE:
			if (!reader.read(splitAttempt) || !splitAttempt || !reader.read(splitSource) || !reader.read(splitTarget) ||
				splitSource == splitTarget || !layout.read(reader) || !layout.cellForApp(splitSource) ||
				!layout.cellForApp(splitTarget) || splitTarget == layout.owner ||
				(operation == SPLIT_PREPARE && !resources.read(reader)))
				return false;
			[[fallthrough]];
		case COMPLETE:
		{
			uint16 count = 0;
			if (!reader.read(count) || count == 0 || count > MAX_CELLS)
				return false;
			cellPeerRoutes.clear();
			for (uint16 index = 0; index < count; ++index)
			{
				COMPONENT_ID cid = 0;
				CellPeerRoute route;
				if (!reader.read(cid) || cid == 0 || !route.read(reader) ||
					!cellPeerRoutes.emplace(cid, std::move(route)).second)
					return false;
			}
			if (operation != COMPLETE &&
				(cellPeerRoutes.size() != layout.participants().size() ||
				 !std::all_of(cellPeerRoutes.begin(), cellPeerRoutes.end(),
					 [&](const auto& route) { return layout.cellForApp(route.first); })))
				return false;
			break;
		}
		case REPLACEMENT_READY:
			if (!reader.read(replacementProcess))
				return false;
			[[fallthrough]];
		case SPLIT_READY:
			if (operation == SPLIT_READY && (!reader.read(splitAttempt) || !splitAttempt))
				return false;
			[[fallthrough]];
		case READY: case COMMITTED: case ABORT: case SUSPEND: case BOUNDARY_RESULT:
			if (!reader.read(accepted) || accepted > 1 || !reader.readBlob(error, 1024))
				return false;
			success = accepted != 0;
			if (operation == REPLACEMENT_READY && success && !replacementProcess)
				return false;
			if (operation == BOUNDARY_RESULT && (!revision || success != error.empty()))
				return false;
			break;
		case BOUNDARY_REQUEST: case BOUNDARY_APPLY:
			if ((operation == BOUNDARY_APPLY && !revision) ||
				(operation == BOUNDARY_REQUEST && revision == std::numeric_limits<uint64>::max()) ||
				!reader.read(boundaryIndex) || boundaryIndex >= MAX_NODES ||
				!reader.read(boundaryPosition) || !std::isfinite(boundaryPosition))
				return false;
			break;
		case BOUNDARY_APPLIED:
			if (!revision)
				return false;
			break;
		case SPLIT_CANCEL:
			if (!reader.read(splitTarget) || !splitTarget)
				return false;
			[[fallthrough]];
		case SPLIT_APPLY: case SPLIT_APPLIED: case SPLIT_COMPLETE: case SPLIT_DRAINED: case SPLIT_DISCARDED:
		case SPLIT_CHECK_TARGET:
			if (!reader.read(splitAttempt) || !splitAttempt)
				return false;
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
		case COMMIT: case SHUTDOWN: case REMOVE: case REMOVED: case REPLACEMENT_CANCEL:
			break;
		default:
			return false;
		}
		return reader.empty();
	}
};

struct Replacement
{
	enum Stage { Preparing, Ready, Checking, Cancelling };

	COMPONENT_ID lost = 0;
	COMPONENT_ID target = 0;
	uint64 targetSession = 0;
	uint64 version = 0;
	uint64 started = 0;
	Stage stage = Preparing;
	std::set<COMPONENT_ID> checked;
	uint64 process = 0;

	bool matches(COMPONENT_ID cid, uint64 session, uint64 attempt) const
	{
		return cid == target && session == targetSession && attempt == version;
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
	uint64 boundaryRevision = 0;
	struct BoundaryConfirmation
	{
		uint64 started;
		std::set<COMPONENT_ID> acknowledged;
	};
	std::optional<BoundaryConfirmation> boundaryConfirmation;
	struct Split
	{
		Layout layout;
		COMPONENT_ID source;
		COMPONENT_ID target;
		uint64 targetSession;
		uint64 started;
		uint64 readyResourceRevision = 0;
		bool published = false;
		std::set<COMPONENT_ID> confirmed;
	};
	std::optional<Split> split;
	uint64 splitAttempt = 0;
	std::string splitError;
	enum class SplitMigration { None, CheckingSource, CheckingTarget, Complete, Interrupted };
	SplitMigration splitMigration = SplitMigration::None;
	COMPONENT_ID splitMigrationSource = 0;
	COMPONENT_ID splitMigrationTarget = 0;
	uint64 started = 0;
	State state = PREPARING;
	bool suspended = false;
	bool shutdownRequested = false;
	bool activated = false;
	std::string error;
	std::set<COMPONENT_ID> acknowledged;
	std::optional<Replacement> replacement;
	struct CommittedReplacement
	{
		COMPONENT_ID lost;
		uint64 originalVersion;
		COMPONENT_ID target;
		uint64 targetSession;
		uint64 process;
	};
	std::optional<CommittedReplacement> committedReplacement;
	uint64 lastReplacementVersion = 0;
	uint64 replacementRetryAt = 0;
	std::map<COMPONENT_ID, uint64> rejectedReplacements;

	bool isSplitMigrating() const
	{
		return splitMigration == SplitMigration::CheckingSource || splitMigration == SplitMigration::CheckingTarget;
	}

	void interruptSplitMigration()
	{
		if (isSplitMigrating())
			splitMigration = SplitMigration::Interrupted;
	}

	bool canConfirmSplitMigration(const Packet& packet) const
	{
		if (state != ACTIVE || split || version != packet.version ||
			boundaryRevision != packet.revision || splitAttempt != packet.splitAttempt)
			return false;
		return (splitMigration == SplitMigration::CheckingSource && splitMigrationSource == packet.sender) ||
			(splitMigration == SplitMigration::CheckingTarget && splitMigrationTarget == packet.sender);
	}

	bool confirmBoundary(COMPONENT_ID cid, uint64 layoutVersion, uint64 confirmedRevision)
	{
		if (state != ACTIVE || !boundaryConfirmation || version != layoutVersion ||
			boundaryRevision != confirmedRevision || !layout.cellForApp(cid))
			return false;
		auto& confirmed = boundaryConfirmation->acknowledged;
		confirmed.insert(cid);
		return confirmed.size() == layout.participants().size();
	}

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
