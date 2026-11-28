#ifndef KBE_BOUNDARY_GHOSTS_H
#define KBE_BOUNDARY_GHOSTS_H

#include "common/common.h"
#include "math/math.h"
#include "network/address.h"
#include "server/space_layout.h"
#include "cell_handoff_snapshot.h"

namespace KBEngine {

class Entity;
class Space;
namespace Network { class Channel; }

// 两个 Cell 的消息可交错。只有当前副本通过 MOVED 确认了同一归属，缺席通告才生效。
struct HandoffCopyAbsence
{
	COMPONENT_ID source = 0;
	CELL_ID cell = 0;
	uint64 session = 0;
	uint64 epoch = 0;

	bool matches(const HandoffCopyAbsence& other) const
	{
		return epoch && epoch == other.epoch && source == other.source &&
			cell == other.cell && session == other.session;
	}

	bool receive(const HandoffCopyAbsence& notice, const HandoffCopyAbsence& owner, bool placeholder)
	{
		if (notice.epoch > owner.epoch && notice.epoch >= epoch)
			*this = notice;
		return placeholder && notice.matches(owner);
	}
};

class BoundaryGhosts
{
public:
	void update(Space& space);
	void forgetPeer(Space& space, COMPONENT_ID peer);
	static void sendMergeFence(Network::Channel& channel, const Space& space, CELL_ID target,
		uint64 attempt, bool acknowledge);
	void remove(Space& space, Entity& entity, COMPONENT_ID successor = 0);
	CellHandoff::CopyPeers handoffPeers(ENTITY_ID id, COMPONENT_ID target) const;
	void prepareHandoffCopies(Space& space, Entity& entity, const CellHandoff::CopyPeers& peers);
	bool retainHandoffSource(Space& space, Entity& entity, uint64 session);
	void resolveHandoffCopy(Space& space, COMPONENT_ID previous, ENTITY_ID id, uint64 epoch);
	bool forgetHandoffTarget(ENTITY_ID id, COMPONENT_ID source, uint64 session, uint64 generation);
	uint64 handoffCopyGeneration(ENTITY_ID id, COMPONENT_ID target, uint64 session) const;
	const char* checkHandoffSnapshot(Entity& entity, COMPONENT_ID target, MemoryStream& snapshot) const;
	void receive(Space& space, COMPONENT_ID source, Network::Channel& channel, MemoryStream& stream);
	void property(Space& space, const Entity& entity, const MemoryStream& data);
	void broadcast(Space& space, const Entity& entity, const MemoryStream& data);
	void witnessed(Space& space, const Entity& entity, bool observed);
	bool hasCopies(ENTITY_ID id) const { return outgoing_.count(id) != 0; }

private:
	enum Operation : uint8 { CREATE = 1, REMOVE, POSE, PROPERTY, BROADCAST, WITNESSED,
		CHECK_HANDOFF_COPY, HANDOFF_COPY_ABSENT, HANDOFF_COPY_MOVED, MERGE_FENCE, MERGE_FENCE_ACK };

	struct Pose
	{
		Position3D position;
		Direction3D direction;
		int8 layer = 0;
		bool onGround = false;

		bool operator==(const Pose& other) const;
		bool read(MemoryStream& stream);
		void write(MemoryStream& stream) const;
		void apply(Entity& entity, bool initial) const;
	};

	struct Copy
	{
		uint64 session = 0;
		uint64 generation = 0;
		Pose pose;
		CELL_ID cell = 0;
		bool witnessed = false;
	};

	struct Incoming
	{
		COMPONENT_ID source;
		Network::Address address;
		uint64 session;
		uint64 generation;
		CELL_ID cell;
		uint64 nextHandoffCheck = 0;
		HandoffCopyAbsence earlyAbsence;
	};

	struct Source
	{
		uint64 session = 0;
		uint64 version = 0;
		uint64 lastCreate = 0;
	};

	struct Neighbour
	{
		COMPONENT_ID component;
		SpaceLayout::Bounds bounds;
		CELL_ID cell;
	};

	struct SendTarget
	{
		const Neighbour* neighbour = NULL;
		Network::Channel* channel = NULL;
		size_t remaining = 0;
	};

	using Copies = std::map<COMPONENT_ID, Copy>;

	static bool isWitnessed(const Copies& copies);
	static const char* writeSnapshot(Entity& entity, const Pose& pose, MemoryStream& snapshot);
	void createCopy(Space& space, Entity& entity, const Neighbour& neighbour, Network::Channel& channel,
		const Pose& pose, const MemoryStream& snapshot);
	void refresh(Space& space, Entity& entity, SendTarget* targets, size_t count);
	void sendToCopies(Space& space, const Entity& entity, Operation operation, const MemoryStream& data);
	void clearIncoming(Space& space);
	static void send(Network::Channel& channel, const Space& space, CELL_ID target, ENTITY_ID id,
		uint64 generation, Operation operation, const MemoryStream* data = NULL);

	std::map<ENTITY_ID, Copies> outgoing_;
	std::map<ENTITY_ID, Incoming> incoming_;
	std::map<COMPONENT_ID, Source> sources_;
	std::vector<Neighbour> neighbours_;
	uint64 version_ = 0;
	uint64 boundaryRevision_ = 0;
	uint64 generation_ = 0;
	uint64 nextIncomingSnapshot_ = 0;
	uint64 nextCheck_ = 0;
	size_t cursor_ = 0;
};

}

#endif
