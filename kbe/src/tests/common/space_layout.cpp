#include "server/space_layout.h"

#include <cstdio>
#include <limits>

namespace {
unsigned checks = 0;
bool check(bool condition, const char* name)
{
	++checks;
	if (!condition)
		std::fprintf(stderr, "SPACE_LAYOUT_TEST_FAIL check=%s\n", name);
	return condition;
}
}

int main()
{
	using namespace KBEngine;
	using namespace KBEngine::SpaceLayout;
	Layout layout;
	layout.owner = 8001;
	layout.nodes = {
		{X, 10.f, 0, 0},
		{Z, 20.f, 0, 0}, {LEAF, 0.f, 1, 8001}, {LEAF, 0.f, 2, 8002},
		{Z, 20.f, 0, 0}, {LEAF, 0.f, 3, 8003}, {LEAF, 0.f, 4, 8004}
	};
	if (!check(layout.validate(), "four_cells") ||
		!check(layout.cellAt(9.f, 19.f)->cellappID == 8001, "left_bottom") ||
		!check(layout.cellAt(9.f, 20.f)->cellappID == 8002, "z_boundary") ||
		!check(layout.cellAt(10.f, 19.f)->cellappID == 8003, "x_boundary") ||
		!check(layout.cellAt(10.f, 20.f)->cellappID == 8004, "intersection") ||
		!check(layout.cellAt(std::numeric_limits<float>::quiet_NaN(), 0) == NULL, "nan_position") ||
		!check(layout.cellAt(0, std::numeric_limits<float>::infinity()) == NULL, "infinite_position"))
		return 1;
	Bounds bounds;
	if (!check(layout.boundsForApp(8002, bounds) && bounds.maxX == 10.f &&
		bounds.minZ == 20.f && std::isinf(bounds.minX) && std::isinf(bounds.maxZ), "cell_bounds") ||
		!check(!layout.boundsForApp(9999, bounds), "unknown_cell"))
		return 1;
	if (!check(layout.boundsForApp(8002, bounds), "handoff_bounds") ||
		!check(!bounds.crossedBy(10.f, 20.f, 2.f), "inside_handoff_margin_x") ||
		!check(!bounds.crossedBy(9.f, 19.f, 2.f), "inside_handoff_margin_z") ||
		!check(bounds.crossedBy(12.f, 21.f, 2.f), "handoff_margin_x") ||
		!check(bounds.crossedBy(9.f, 18.f, 2.f), "handoff_margin_z") ||
		!check(bounds.crossedBy(10.f, 20.f, 0.f), "right_owns_x_boundary") ||
		!check(!bounds.crossedBy(9.f, 20.f, 0.f), "upper_owns_z_boundary") ||
		!check(!bounds.crossedBy(-std::numeric_limits<float>::infinity(), 20.f, 0.f), "non_finite_handoff_position"))
		return 1;
	Layout invalid = layout;
	invalid.nodes[3].cellappID = 8001;
	if (!check(!invalid.validate(), "one_cell_per_app"))
		return 1;
	invalid = layout;
	invalid.nodes[3].cellID = 1;
	if (!check(!invalid.validate(), "duplicate_cell_id"))
		return 1;
	invalid = layout;
	invalid.nodes[4].axis = X;
	invalid.nodes[4].position = 9.f;
	if (!check(!invalid.validate(), "boundary_outside_parent"))
		return 1;
	invalid = layout;
	invalid.nodes.pop_back();
	if (!check(!invalid.validate(), "missing_subtree"))
		return 1;
	invalid = layout;
	invalid.nodes.push_back(Node{LEAF, 0.f, 5, 8005});
	if (!check(!invalid.validate(), "extra_subtree"))
		return 1;
	invalid = layout;
	invalid.owner = 9999;
	if (!check(!invalid.validate(), "missing_owner"))
		return 1;
	for (float position : {std::numeric_limits<float>::infinity(),
		-std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
	{
		invalid = layout;
		invalid.nodes[0].position = position;
		if (!check(!invalid.validate(), "non_finite_boundary"))
			return 1;
	}

	Packet request;
	request.sender = 8001;
	request.operation = REQUEST;
	request.spaceID = 100;
	request.version = 9;
	request.revision = 1;
	request.layout = layout;
	request.resources.scriptModule = "Space";
	request.resources.geometryPath = "spaces/test";
	request.resources.loadGeometry = true;
	request.resources.geometryParams[0] = "world.navmesh";
	request.resources.data["_mapping"] = request.resources.geometryPath;
	request.resources.data["weather"] = "\xe9\x9b\xa8";
	MemoryStream encoded;
	request.write(encoded);
	Packet decoded;
	if (!check(decoded.read(encoded) && decoded.layout.validate() &&
		decoded.resources.data == request.resources.data &&
		decoded.resources.geometryParams == request.resources.geometryParams, "resource_roundtrip"))
		return 1;
	// 截断每一个字节位置，确保未完成的布局和资源消息都在读取前被拒绝。
	for (size_t count = 0; count < encoded.length(); ++count)
	{
		MemoryStream truncated;
		truncated.append(encoded.data(), count);
		Packet partial;
		if (!check(!partial.read(truncated), "truncated_packet"))
			return 1;
	}
	encoded << uint8(0);
	Packet trailing;
	if (!check(!trailing.read(encoded), "trailing_bytes"))
		return 1;
	Packet excessive = request;
	excessive.resources.data["large"] = std::string(MAX_VALUE_BYTES + 1, 'x');
	MemoryStream large;
	excessive.write(large);
	Packet refused;
	if (!check(!refused.read(large), "oversized_value"))
		return 1;
	excessive = request;
	excessive.resources.data["_mapping"] = "spaces/other";
	MemoryStream inconsistent;
	excessive.write(inconsistent);
	Packet mismatched;
	if (!check(!mismatched.read(inconsistent), "inconsistent_mapping"))
		return 1;
	CellPeerRoute peerRoute;
	peerRoute.internalIP = 0x7f000001;
	peerRoute.internalPort = 20013;
	peerRoute.groupOrder = 2;
	peerRoute.username = "cell";
	for (uint8 operation = REQUEST; operation <= RESOURCES; ++operation)
	{
		Packet packet = request;
		packet.operation = operation;
		packet.error = "preparation failed";
		packet.key = "weather";
		packet.value = "rain";
		if (operation == COMPLETE)
			packet.cellPeerRoutes.emplace(8002, peerRoute);
		MemoryStream stream;
		packet.write(stream);
		Packet result;
		if (!check(result.read(stream) && result.operation == operation, "operation_roundtrip"))
			return 1;
		if (operation == COMPLETE)
		{
			const auto peer = result.cellPeerRoutes.find(8002);
			if (!check(peer != result.cellPeerRoutes.end() &&
				peer->second.internalIP == peerRoute.internalIP &&
				peer->second.internalPort == peerRoute.internalPort &&
				peer->second.groupOrder == peerRoute.groupOrder &&
				peer->second.username == peerRoute.username, "complete_peer_route_roundtrip"))
				return 1;
			MemoryStream truncated;
			truncated.append(stream.data(), stream.length() - 1);
			Packet partial;
			if (!check(!partial.read(truncated), "truncated_peer_route"))
				return 1;
		}
	}
	MemoryStream duplicatePeers;
	duplicatePeers << COMPONENT_ID(8001) << uint8(COMPLETE) << SPACE_ID(100) << uint64(9)
		<< uint64(1) << uint16(2);
	for (int i = 0; i < 2; ++i)
	{
		duplicatePeers << COMPONENT_ID(8002);
		peerRoute.write(duplicatePeers);
	}
	Packet invalidPeers;
	if (!check(!invalidPeers.read(duplicatePeers), "duplicate_peer_route"))
		return 1;
	Packet invalidPeer = request;
	invalidPeer.operation = COMPLETE;
	peerRoute.internalPort = 0;
	invalidPeer.cellPeerRoutes.emplace(8002, peerRoute);
	MemoryStream zeroPort;
	invalidPeer.write(zeroPort);
	if (!check(!invalidPeers.read(zeroPort), "zero_peer_port"))
		return 1;
	for (uint8 operation : {SET_DATA, DELETE_DATA, RESOURCES})
	{
		Packet rejected;
		rejected.sender = 5001;
		rejected.operation = UPDATE_REJECTED;
		rejected.rejectedOperation = operation;
		rejected.spaceID = request.spaceID;
		rejected.version = request.version;
		rejected.revision = 4;
		rejected.key = operation == RESOURCES ? "" : "weather";
		rejected.error = "resource limit exceeded";
		MemoryStream stream;
		rejected.write(stream);
		Packet result;
		if (!check(result.read(stream) && result.operation == UPDATE_REJECTED &&
			result.sender == rejected.sender && result.rejectedOperation == operation &&
			result.spaceID == rejected.spaceID && result.version == rejected.version &&
			result.revision == rejected.revision && result.key == rejected.key &&
			result.error == rejected.error, "resource_rejection_roundtrip"))
			return 1;
		MemoryStream truncated;
		truncated.append(stream.data(), stream.length() - 1);
		Packet partial;
		if (!check(!partial.read(truncated), "truncated_resource_rejection"))
			return 1;
	}
	Packet invalidRejection = request;
	invalidRejection.operation = UPDATE_REJECTED;
	invalidRejection.rejectedOperation = SUSPEND;
	invalidRejection.error = "resource limit exceeded";
	MemoryStream wrongOperation;
	invalidRejection.write(wrongOperation);
	Packet wrongRejection;
	if (!check(!wrongRejection.read(wrongOperation), "invalid_rejected_operation"))
		return 1;
	Transaction transaction;
	transaction.layout = layout;
	transaction.version = 9;
	if (!check(!transaction.confirm(8001, 8, PREPARING), "stale_confirmation") ||
		!check(!transaction.confirm(9999, 9, PREPARING), "unknown_confirmation") ||
		!check(!transaction.confirm(8001, 9, COMMITTING), "wrong_phase") ||
		!check(!transaction.confirm(8001, 9, PREPARING), "first_confirmation") ||
		!check(!transaction.confirm(8001, 9, PREPARING), "duplicate_confirmation") ||
		!check(!transaction.confirm(8002, 9, PREPARING), "second_confirmation") ||
		!check(!transaction.confirm(8003, 9, PREPARING), "third_confirmation") ||
		!check(transaction.confirm(8004, 9, PREPARING), "all_prepared"))
		return 1;
	transaction.advance(COMMITTING, 100);
	if (!check(transaction.acknowledged.empty() && transaction.started == 100, "advance_phase") ||
		!check(!transaction.confirm(8001, 9, PREPARING), "late_preparation") ||
		!check(!transaction.confirm(8001, 9, COMMITTING), "first_commit") ||
		!check(!transaction.confirm(8002, 9, COMMITTING), "second_commit") ||
		!check(!transaction.confirm(8003, 9, COMMITTING), "third_commit") ||
		!check(transaction.confirm(8004, 9, COMMITTING), "all_committed"))
		return 1;

	using Removal = Transaction::Removal;
	transaction.advance(ACTIVE, 101);
	if (!check(transaction.recordRemoval(8002, 8) == Removal::Ignore, "old_replica_removal") ||
		!check(transaction.recordRemoval(9999, 9) == Removal::Ignore, "nonparticipant_removal") ||
		!check(transaction.recordRemoval(8002, 9) == Removal::Fail, "active_participant_removal"))
		return 1;
	transaction.advance(RELEASING, 102);
	if (!check(transaction.recordRemoval(8001, 8) == Removal::Ignore &&
		transaction.acknowledged.empty(), "old_removal_not_acknowledged") ||
		!check(transaction.recordRemoval(8001, 9) == Removal::Ignore, "first_removal") ||
		!check(transaction.recordRemoval(8001, 9) == Removal::Ignore, "duplicate_removal") ||
		!check(transaction.recordRemoval(8002, 9) == Removal::Ignore, "second_removal") ||
		!check(transaction.recordRemoval(8003, 9) == Removal::Ignore, "third_removal") ||
		!check(transaction.recordRemoval(8004, 9) == Removal::Erase, "all_removed"))
		return 1;
	transaction.advance(FAILED, 103);
	if (!check(transaction.recordRemoval(8002, 9) == Removal::Ignore, "failed_replica_removed") ||
		!check(transaction.recordRemoval(8001, 8) == Removal::Ignore, "old_failed_owner_removal") ||
		!check(transaction.recordRemoval(8001, 9) == Removal::Erase, "failed_owner_removal"))
		return 1;
	transaction.suspended = true;
	if (!check(transaction.recordRemoval(8001, 9) == Removal::Ignore, "suspended_layout_retained"))
		return 1;

	Resources next = request.resources;
	next.setGeometry(next.geometryPath, true, {{0, std::string(4097, 'x')}});
	if (!check(!next.validate() && request.resources.validate(), "invalid_geometry_candidate"))
		return 1;
	next.setGeometry("spaces/second", true, {{0, "alternate.bin"}});
	if (!check(next.validate() && next.data.at("_mapping") == "spaces/second" &&
		next.data.at("weather") == request.resources.data.at("weather"), "mapping_update_keeps_spacedata"))
		return 1;
	std::printf("SPACE_LAYOUT_TEST_PASS checks=%u\n", checks);
	return 0;
}
