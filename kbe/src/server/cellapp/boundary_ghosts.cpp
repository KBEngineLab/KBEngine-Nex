#include "boundary_ghosts.h"
#include "cellapp.h"
#include "cellapp_interface.h"
#include "clients_remote_entity_method.h"
#include "entity.h"
#include "space.h"

#include <algorithm>
#include <array>
#include <optional>

namespace KBEngine {
namespace
{
	constexpr size_t MAX_STATE_BYTES = 256 * 1024;
	constexpr int32 MAX_PENDING_BYTES = 512 * 1024;

	Network::Channel* peerChannel(COMPONENT_ID component)
	{
		auto* peer = Components::getSingleton().findComponent(CELLAPP_TYPE, component);
		auto* channel = peer ? peer->pChannel : NULL;
		return channel && !channel->isDestroyed() && channel->condemn() == 0 ? channel : NULL;
	}

	bool intersects(const SpaceLayout::Bounds& bounds, const Position3D& position, float distance)
	{
		return position.x >= bounds.minX - distance && position.x <= bounds.maxX + distance &&
			position.z >= bounds.minZ - distance && position.z <= bounds.maxZ + distance;
	}
}

bool BoundaryGhosts::Pose::operator==(const Pose& other) const
{
	return position.x == other.position.x && position.y == other.position.y && position.z == other.position.z &&
		direction.dir.x == other.direction.dir.x && direction.dir.y == other.direction.dir.y &&
		direction.dir.z == other.direction.dir.z && layer == other.layer && onGround == other.onGround;
}

bool BoundaryGhosts::Pose::read(MemoryStream& stream)
{
	uint8 ground = 0;
	stream >> position.x >> position.y >> position.z >> direction.dir.x >> direction.dir.y >>
		direction.dir.z >> layer >> ground;
	onGround = ground != 0;
	return ground <= 1 && std::isfinite(position.x) && std::isfinite(position.y) &&
		std::isfinite(position.z) && std::isfinite(direction.dir.x) &&
		std::isfinite(direction.dir.y) && std::isfinite(direction.dir.z);
}

void BoundaryGhosts::Pose::write(MemoryStream& stream) const
{
	stream << position.x << position.y << position.z << direction.dir.x << direction.dir.y <<
		direction.dir.z << layer << static_cast<uint8>(onGround);
}

void BoundaryGhosts::Pose::apply(Entity& entity, bool initial) const
{
	EntityPtr retained(&entity);
	const bool groundChanged = entity.isOnGround() != onGround;
	entity.setBoundaryGhostLayer(layer);
	entity.isOnGround(onGround);
	entity.setPositionAndDirection(position, direction);
	if (initial)
		entity.updateLastPos();
	else if (groundChanged && !entity.isDestroyed())
		entity.onDirectionChanged();
}

void BoundaryGhosts::send(Network::Channel& channel, const Space& space, ENTITY_ID id,
	uint64 generation, Operation operation, const MemoryStream* data)
{
	auto* bundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
	bundle->newMessage(CellappInterface::onBoundaryGhost);
	*bundle << space.id() << space.layoutVersion() << id << generation << static_cast<uint8>(operation);
	if (data && data->length() != 0)
		bundle->append(data->data() + data->rpos(), static_cast<int>(data->length()));
	channel.send(bundle);
}

bool BoundaryGhosts::isWitnessed(const Copies& copies)
{
	return std::any_of(copies.begin(), copies.end(), [](const auto& copy) { return copy.second.witnessed; });
}

void BoundaryGhosts::refresh(Space& space, Entity& entity, SendTarget* targets, size_t count)
{
	if (!entity.isReal() || entity.isDestroyed() || entity.isSpace() ||
		entity.hasFlags(ENTITY_FLAGS_TELEPORT_START))
		return;

	const Pose pose{entity.position(), entity.direction(), entity.layer(), entity.isOnGround()};
	const float distance = g_kbeSrvConfig.getCellApp().ghostDistance;
	auto entry = outgoing_.find(entity.id());
	std::optional<MemoryStream> snapshot;
	bool snapshotValid = false;
	bool lostWitness = false;

	for (size_t i = 0; i < count; ++i)
	{
		auto& target = targets[i];
		const auto& neighbour = *target.neighbour;
		auto* channel = target.channel;
		auto* copies = entry != outgoing_.end() ? &entry->second : NULL;
		auto copy = copies ? copies->find(neighbour.component) : Copies::iterator();
		bool exists = copies && copy != copies->end();
		if (exists && (!channel || copy->second.session != channel->sessionEpoch()))
		{
			lostWitness |= copy->second.witnessed;
			copies->erase(copy);
			exists = false;
		}
		if (!channel)
			continue;

		// 新副本在兴趣范围内创建；已有副本稍晚删除，避免贴着边界移动时反复建删。
		if (!intersects(neighbour.bounds, pose.position, distance + (exists ? distance * 0.1f : 0.f)))
		{
			if (exists)
			{
				lostWitness |= copy->second.witnessed;
				send(*channel, space, entity.id(), copy->second.generation, REMOVE);
				copies->erase(copy);
			}
			continue;
		}

		constexpr size_t headerAllowance = 64;
		if (target.remaining <= headerAllowance)
			continue;
		if (exists)
		{
			if (!(copy->second.pose == pose))
			{
				MemoryStream data;
				pose.write(data);
				if (data.length() > target.remaining - headerAllowance)
					continue;
				send(*channel, space, entity.id(), copy->second.generation, POSE, &data);
				target.remaining -= data.length() + headerAllowance;
				copy->second.pose = pose;
			}
			continue;
		}

		if (!snapshot)
		{
			snapshot.emplace();
			*snapshot << entity.pScriptModule()->getUType();
			pose.write(*snapshot);
			snapshotValid = entity.writeBoundaryGhostProperties(*snapshot, MAX_STATE_BYTES);
		}
		if (!snapshotValid || snapshot->length() > target.remaining - headerAllowance)
			continue;
		if (entry == outgoing_.end())
			entry = outgoing_.emplace(entity.id(), Copies()).first;
		const uint64 generation = ++generation_;
		send(*channel, space, entity.id(), generation, CREATE, &*snapshot);
		target.remaining -= snapshot->length() + headerAllowance;
		entry->second.emplace(neighbour.component, Copy{channel->sessionEpoch(), generation, pose});
	}

	if (entry != outgoing_.end())
	{
		const bool noRemoteWitness = lostWitness && !isWitnessed(entry->second);
		if (entry->second.empty())
			outgoing_.erase(entry);
		if (noRemoteWitness)
			entity.setBoundaryWitnessed(false);
	}
}

void BoundaryGhosts::update(Space& space)
{
	if (space.canAcceptEntities() && space.layoutState() == SpaceLayout::COMMITTING)
		return;

	const bool active = space.canAcceptEntities() && space.layoutState() == SpaceLayout::ACTIVE;
	if (!active || (version_ != 0 && version_ != space.layoutVersion()))
	{
		if (incoming_.empty() && outgoing_.empty() && !active)
		{
			neighbours_.clear();
			version_ = 0;
			return;
		}

		// 回调可能销毁实体，先移除旧副本的记录，再通知脚本。
		decltype(outgoing_) previous;
		previous.swap(outgoing_);
		neighbours_.clear();
		version_ = 0;
		Entity::bufferCallback(true);
		clearIncoming(space);
		for (const auto& entry : previous)
		{
			auto* entity = Cellapp::getSingleton().findEntity(entry.first);
			if (entity && entity->spaceID() == space.id() && isWitnessed(entry.second))
				entity->setBoundaryWitnessed(false);
		}
		Entity::bufferCallback(false);
		if (!active || !space.canAcceptEntities() || space.layoutState() != SpaceLayout::ACTIVE)
			return;
	}

	if (version_ != space.layoutVersion())
	{
		neighbours_.clear();
		for (const auto& node : space.layout().nodes)
		{
			SpaceLayout::Bounds bounds;
			if (node.axis == SpaceLayout::LEAF && node.cellappID != g_componentID &&
				space.layout().boundsForApp(node.cellappID, bounds))
				neighbours_.push_back({node.cellappID, bounds});
		}
		version_ = space.layoutVersion();
	}
	if (neighbours_.empty() || timestamp() < nextCheck_)
		return;

	auto& app = Cellapp::getSingleton();
	const auto& config = g_kbeSrvConfig.getCellApp();
	nextCheck_ = timestamp() + stampsPerSecond() / config.ghostUpdateHertz;

	for (auto it = incoming_.begin(); it != incoming_.end();)
	{
		const auto& copy = it->second;
		auto* channel = app.networkInterface().findChannel(copy.address);
		if (channel && !channel->isDestroyed() && channel->condemn() == 0 &&
			channel->sessionEpoch() == copy.session)
		{
			++it;
			continue;
		}
		const ENTITY_ID id = it->first;
		auto* entity = app.findEntity(id);
		if (entity && entity->isBoundaryGhost() && entity->spaceID() == space.id())
			app.destroyEntity(id, false);
		else
			incoming_.erase(id);
		it = incoming_.upper_bound(id);
	}

	std::array<SendTarget, SpaceLayout::MAX_CELLS> targets;
	for (size_t i = 0; i < neighbours_.size(); ++i)
	{
		auto* channel = peerChannel(neighbours_[i].component);
		const int32 queued = channel ? channel->bundlesLength() : MAX_PENDING_BYTES;
		targets[i] = {&neighbours_[i], channel,
			static_cast<size_t>(std::max(0, MAX_PENDING_BYTES - queued))};
	}
	const auto& entities = space.entities();
	const size_t count = std::min<size_t>(entities.size(), config.ghostingMaxPerCheck);
	const bool autoHandoff = app.autoCellHandoffEnabled();
	for (size_t checked = 0; checked < count && !entities.empty(); ++checked)
	{
		cursor_ %= entities.size();
		auto& entity = *entities[cursor_++];
		if (autoHandoff)
			app.considerAutoCellHandoff(space, entity);
		refresh(space, entity, targets.data(), neighbours_.size());
	}
}

void BoundaryGhosts::clearIncoming(Space& space)
{
	auto& app = Cellapp::getSingleton();
	while (!incoming_.empty())
	{
		const ENTITY_ID id = incoming_.begin()->first;
		auto* entity = app.findEntity(id);
		if (entity && entity->isBoundaryGhost() && entity->spaceID() == space.id())
			app.destroyEntity(id, false);
		else
			incoming_.erase(id);
	}
}

void BoundaryGhosts::remove(Space& space, Entity& entity, COMPONENT_ID keepTarget)
{
	if (entity.isBoundaryGhost())
	{
		if (entity.isWitnessed())
			witnessed(space, entity, false);
		incoming_.erase(entity.id());
		return;
	}

	const auto entry = outgoing_.find(entity.id());
	if (entry == outgoing_.end())
		return;
	for (const auto& copy : entry->second)
	{
		if (copy.first == keepTarget)
			continue;
		auto* channel = peerChannel(copy.first);
		if (channel && channel->sessionEpoch() == copy.second.session)
			send(*channel, space, entity.id(), copy.second.generation, REMOVE);
	}
	outgoing_.erase(entry);
	entity.setBoundaryWitnessed(false);
}

bool BoundaryGhosts::forgetHandoffTarget(ENTITY_ID id, COMPONENT_ID source, uint64 session, uint64 generation)
{
	const auto entry = incoming_.find(id);
	if (entry == incoming_.end() || entry->second.source != source ||
		entry->second.session != session || entry->second.generation != generation)
		return false;
	incoming_.erase(entry);
	return true;
}

uint64 BoundaryGhosts::handoffCopyGeneration(ENTITY_ID id, COMPONENT_ID target, uint64 session) const
{
	const auto entry = outgoing_.find(id);
	if (entry == outgoing_.end())
		return 0;
	const auto copy = entry->second.find(target);
	return copy != entry->second.end() && copy->second.session == session ? copy->second.generation : 0;
}

void BoundaryGhosts::witnessed(Space& space, const Entity& entity, bool observed)
{
	const auto current = incoming_.find(entity.id());
	if (current == incoming_.end())
		return;

	const auto& copy = current->second;
	auto* channel = Cellapp::getSingleton().networkInterface().findChannel(copy.address);
	if (!channel || channel->isDestroyed() || channel->condemn() != 0 ||
		channel->sessionEpoch() != copy.session)
		return;

	MemoryStream data;
	data << static_cast<uint8>(observed);
	send(*channel, space, entity.id(), copy.generation, WITNESSED, &data);
}

void BoundaryGhosts::sendToCopies(Space& space, const Entity& entity, Operation operation, const MemoryStream& data)
{
	const auto entry = outgoing_.find(entity.id());
	if (entry == outgoing_.end() || data.length() > MAX_STATE_BYTES || !space.canAcceptEntities() ||
		space.layoutState() != SpaceLayout::ACTIVE || version_ != space.layoutVersion())
		return;

	for (const auto& copy : entry->second)
	{
		auto* channel = peerChannel(copy.first);
		if (channel && channel->sessionEpoch() == copy.second.session)
			send(*channel, space, entity.id(), copy.second.generation, operation, &data);
	}
}

void BoundaryGhosts::property(Space& space, const Entity& entity, const MemoryStream& data)
{
	sendToCopies(space, entity, PROPERTY, data);
}

void BoundaryGhosts::broadcast(Space& space, const Entity& entity, const MemoryStream& data)
{
	sendToCopies(space, entity, BROADCAST, data);
}

void BoundaryGhosts::receive(Space& space, COMPONENT_ID source, Network::Channel& channel, MemoryStream& stream)
{
	try
	{
		uint64 version = 0, generation = 0;
		ENTITY_ID id = 0;
		uint8 operation = 0;
		stream >> version >> id >> generation >> operation;
		if (version != space.layoutVersion() || id <= 0 || generation == 0 ||
			stream.length() > MAX_STATE_BYTES || !space.layout().cellForApp(source))
			return;

		auto& app = Cellapp::getSingleton();
		auto* entity = app.findEntity(id);
		if (operation == WITNESSED)
		{
			if (!entity || !entity->isReal() || entity->spaceID() != space.id() ||
				space.layoutState() != SpaceLayout::ACTIVE || version != version_ ||
				stream.length() != sizeof(uint8))
				return;

			const auto entry = outgoing_.find(id);
			if (entry == outgoing_.end())
				return;
			auto copy = entry->second.find(source);
			if (copy == entry->second.end() || copy->second.session != channel.sessionEpoch() ||
				copy->second.generation != generation)
				return;

			uint8 observed = 0;
			stream >> observed;
			if (observed > 1 || copy->second.witnessed == (observed != 0))
				return;
			copy->second.witnessed = observed != 0;
			entity->setBoundaryWitnessed(observed != 0 || isWitnessed(entry->second));
			return;
		}

		const auto current = incoming_.find(id);
		if (operation == CREATE)
		{
			auto& last = sources_[source];
			if (last.session > channel.sessionEpoch() ||
				(last.session == channel.sessionEpoch() && last.version == version && generation <= last.lastCreate))
				return;

			ENTITY_SCRIPT_UID type = 0;
			Pose pose;
			stream >> type;
			auto* module = EntityDef::findScriptModule(type);
			if (!module || !module->hasCell() || !pose.read(stream))
				return;

			if (entity)
			{
				if (!entity->isBoundaryGhost() || current == incoming_.end() || current->second.source != source)
					return;
				app.destroyEntity(id, false);
			}

			entity = app.createEntity(module->getName(), NULL, false, id, false);
			if (!entity)
				return;

			entity->setBoundaryGhost(source);
			space.addEntity(entity);
			if (!entity->initBoundaryGhostProperties(stream))
			{
				app.destroyEntity(id, false);
				return;
			}
			last = {channel.sessionEpoch(), version, generation};
			incoming_.emplace(id, Incoming{source, channel.addr(), channel.sessionEpoch(), generation});
			pose.apply(*entity, true);
			Entity::bufferCallback(true);
			space.addEntityToNode(entity);
			Entity::bufferCallback(false);
			return;
		}

		if (!entity || !entity->isBoundaryGhost() || current == incoming_.end() ||
			current->second.source != source || current->second.session != channel.sessionEpoch() ||
			current->second.generation != generation)
			return;

		switch (operation)
		{
		case REMOVE:
			if (stream.length() == 0)
				app.destroyEntity(id, false);
			break;
		case POSE:
		{
			Pose pose;
			if (pose.read(stream) && stream.length() == 0)
				pose.apply(*entity, false);
			break;
		}
		case PROPERTY:
			entity->applyBoundaryGhostProperty(stream, true);
			break;
		case BROADCAST:
			ClientsRemoteEntityMethod::forwardToWitnesses(*entity, stream, NULL);
			break;
		}
	}
	catch (const MemoryStreamException&)
	{
		WARNING_MSG(fmt::format("BoundaryGhosts::receive: incomplete update, space={}, source={}.\n",
			space.id(), source));
	}
}

}
