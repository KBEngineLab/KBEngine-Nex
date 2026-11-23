#ifndef KBE_BOUNDARY_GHOSTS_H
#define KBE_BOUNDARY_GHOSTS_H

#include "common/common.h"
#include "math/math.h"
#include "network/address.h"
#include "server/space_layout.h"

namespace KBEngine {

class Entity;
class Space;
namespace Network { class Channel; }

class BoundaryGhosts
{
public:
	void update(Space& space);
	void remove(Space& space, Entity& entity, COMPONENT_ID keepTarget = 0);
	bool forgetHandoffTarget(ENTITY_ID id, COMPONENT_ID source, uint64 session, uint64 generation);
	uint64 handoffCopyGeneration(ENTITY_ID id, COMPONENT_ID target, uint64 session) const;
	void receive(Space& space, COMPONENT_ID source, Network::Channel& channel, MemoryStream& stream);
	void property(Space& space, const Entity& entity, const MemoryStream& data);
	void broadcast(Space& space, const Entity& entity, const MemoryStream& data);
	void witnessed(Space& space, const Entity& entity, bool observed);
	bool hasCopies(ENTITY_ID id) const { return outgoing_.count(id) != 0; }

private:
	enum Operation : uint8 { CREATE = 1, REMOVE, POSE, PROPERTY, BROADCAST, WITNESSED };

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
		bool witnessed = false;
	};

	struct Incoming
	{
		COMPONENT_ID source;
		Network::Address address;
		uint64 session;
		uint64 generation;
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
	};

	struct SendTarget
	{
		const Neighbour* neighbour = NULL;
		Network::Channel* channel = NULL;
		size_t remaining = 0;
	};

	using Copies = std::map<COMPONENT_ID, Copy>;

	static bool isWitnessed(const Copies& copies);
	void refresh(Space& space, Entity& entity, SendTarget* targets, size_t count);
	void sendToCopies(Space& space, const Entity& entity, Operation operation, const MemoryStream& data);
	void clearIncoming(Space& space);
	static void send(Network::Channel& channel, const Space& space, ENTITY_ID id,
		uint64 generation, Operation operation, const MemoryStream* data = NULL);

	std::map<ENTITY_ID, Copies> outgoing_;
	std::map<ENTITY_ID, Incoming> incoming_;
	std::map<COMPONENT_ID, Source> sources_;
	std::vector<Neighbour> neighbours_;
	uint64 version_ = 0;
	uint64 generation_ = 0;
	uint64 nextCheck_ = 0;
	size_t cursor_ = 0;
};

}

#endif
