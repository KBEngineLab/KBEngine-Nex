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
	constexpr size_t MAX_STATE_BYTES = CellHandoff::CopyBudget::MAX_STATE;
	constexpr int32 MAX_PENDING_BYTES = CellHandoff::CopyBudget::ORDINARY_BYTES;
	constexpr size_t HEADER_ALLOWANCE = CellHandoff::CopyBudget::HEADER;

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

void BoundaryGhosts::send(Network::Channel& channel, const Space& space, CELL_ID target, ENTITY_ID id,
	uint64 generation, Operation operation, const MemoryStream* data)
{
	auto* bundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
	bundle->newMessage(CellappInterface::onBoundaryGhost);
	// CellApp 可再次加入同一个 Space，Layout 版本和连接都可能不变。
	*bundle << space.id() << space.layoutVersion() << space.pCell()->id() << target
		<< id << generation << static_cast<uint8>(operation);
	if (data && data->length() != 0)
		bundle->append(data->data() + data->rpos(), static_cast<int>(data->length()));
	channel.send(bundle);
}

bool BoundaryGhosts::isWitnessed(const Copies& copies)
{
	return std::any_of(copies.begin(), copies.end(), [](const auto& copy) { return copy.second.witnessed; });
}

void BoundaryGhosts::sendMergeFence(Network::Channel& channel, const Space& space, CELL_ID target,
	uint64 attempt, bool acknowledge)
{
	// 实体编号零保留给整条 Ghost 连接的确认。复用同一可靠发送队列，保证 MOVED/REMOVE 排在标记前。
	MemoryStream payload;
	payload << space.boundaryRevision();
	send(channel, space, target, 0, attempt, acknowledge ? MERGE_FENCE_ACK : MERGE_FENCE, &payload);
}

const char* BoundaryGhosts::writeSnapshot(Entity& entity, const Pose& pose, MemoryStream& snapshot)
{
	snapshot << entity.pScriptModule()->getUType() << entity.routingEpoch();
	pose.write(snapshot);
	if (entity.writeBoundaryGhostProperties(snapshot, MAX_STATE_BYTES))
		return nullptr;
	return snapshot.length() > MAX_STATE_BYTES ? "target Ghost snapshot exceeds 256 KiB" :
		"could not serialize target Ghost snapshot";
}

const char* BoundaryGhosts::checkHandoffSnapshot(Entity& entity, COMPONENT_ID target, MemoryStream& snapshot) const
{
	auto* channel = peerChannel(target);
	if (channel && handoffCopyGeneration(entity.id(), target, channel->sessionEpoch()))
		return nullptr;
	const Pose pose{entity.position(), entity.direction(), entity.layer(), entity.isOnGround()};
	return writeSnapshot(entity, pose, snapshot);
}

void BoundaryGhosts::createCopy(Space& space, Entity& entity, const Neighbour& neighbour,
	Network::Channel& channel, const Pose& pose, const MemoryStream& snapshot)
{
	const uint64 generation = ++generation_;
	send(channel, space, neighbour.cell, entity.id(), generation, CREATE, &snapshot);
	outgoing_[entity.id()][neighbour.component] = {channel.sessionEpoch(), generation, pose, neighbour.cell};
}

void BoundaryGhosts::refresh(Space& space, Entity& entity, SendTarget* targets, size_t count)
{
	if (!entity.isReal() || entity.isDestroyed() || entity.isSpace() ||
		entity.hasFlags(ENTITY_FLAGS_TELEPORT_START))
		return;

	const Pose pose{entity.position(), entity.direction(), entity.layer(), entity.isOnGround()};
	const float distance = g_kbeSrvConfig.getCellApp().ghostDistance;
	const auto* drainTarget = space.isDrainingCell() && !space.isCellMergeHandoffStopped() ?
		space.handoffDestination(pose.position.x, pose.position.z, g_componentID) : nullptr;
	auto entry = outgoing_.find(entity.id());
	std::optional<MemoryStream> snapshot;
	const char* snapshotError = nullptr;
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
		if ((!drainTarget || drainTarget->cellappID != neighbour.component) &&
			!intersects(neighbour.bounds, pose.position, distance + (exists ? distance * 0.1f : 0.f)))
		{
			if (exists)
			{
				lostWitness |= copy->second.witnessed;
				send(*channel, space, copy->second.cell, entity.id(), copy->second.generation, REMOVE);
				copies->erase(copy);
			}
			continue;
		}

		if (target.remaining <= HEADER_ALLOWANCE)
			continue;
		if (exists)
		{
			if (!(copy->second.pose == pose))
			{
				MemoryStream data;
				pose.write(data);
				if (data.length() > target.remaining - HEADER_ALLOWANCE)
					continue;
				send(*channel, space, copy->second.cell, entity.id(), copy->second.generation, POSE, &data);
				target.remaining -= data.length() + HEADER_ALLOWANCE;
				copy->second.pose = pose;
			}
			continue;
		}

		if (!snapshot)
		{
			snapshot.emplace();
			snapshotError = writeSnapshot(entity, pose, *snapshot);
		}
		if (snapshotError && drainTarget && drainTarget->cellappID == neighbour.component)
			space.reportCellMergeBlock(entity.id(), snapshotError);
		if (snapshotError || snapshot->length() > target.remaining - HEADER_ALLOWANCE)
			continue;
		createCopy(space, entity, neighbour, *channel, pose, *snapshot);
		if (entry == outgoing_.end())
			entry = outgoing_.find(entity.id());
		target.remaining -= snapshot->length() + HEADER_ALLOWANCE;
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
	if ((space.canAcceptEntities() || space.canReceiveSplitGhosts()) && space.layoutState() == SpaceLayout::COMMITTING)
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

	if (version_ != space.layoutVersion() || boundaryRevision_ != space.boundaryRevision())
	{
		neighbours_.clear();
		for (const auto& node : space.layout().nodes)
		{
			SpaceLayout::Bounds bounds;
			if (node.axis == SpaceLayout::LEAF && node.cellappID != g_componentID &&
				space.layout().boundsForApp(node.cellappID, bounds))
				neighbours_.push_back({node.cellappID, bounds, node.cellID});
		}
		version_ = space.layoutVersion();
		boundaryRevision_ = space.boundaryRevision();
	}
	if (neighbours_.empty() || timestamp() < nextCheck_)
		return;

	auto& app = Cellapp::getSingleton();
	const auto& config = g_kbeSrvConfig.getCellApp();
	const auto now = timestamp();
	nextCheck_ = now + stampsPerSecond() / config.ghostUpdateHertz;

	for (auto it = incoming_.begin(); it != incoming_.end();)
	{
		auto& copy = it->second;
		auto* channel = app.networkInterface().findChannel(copy.address);
		if (channel && !channel->isDestroyed() && channel->condemn() == 0 &&
			channel->sessionEpoch() == copy.session)
		{
			// 扫描或发送积压不能证明实体已离开。超时只向同一会话查询，按权威答复清理。
			if (copy.nextHandoffCheck && now >= copy.nextHandoffCheck)
			{
				auto* entity = app.findEntity(it->first);
				if (entity && channel->bundlesLength() < MAX_PENDING_BYTES)
					send(*channel, space, copy.cell, it->first, entity->routingEpoch(), CHECK_HANDOFF_COPY);
				copy.nextHandoffCheck = now + 5 * stampsPerSecond();
			}
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
		auto* channel = space.isPendingSplitPeer(neighbours_[i].component) ? nullptr :
			peerChannel(neighbours_[i].component);
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

void BoundaryGhosts::forgetPeer(Space& space, COMPONENT_ID peer)
{
	auto& app = Cellapp::getSingleton();
	Entity::bufferCallback(true);
	for (auto it = incoming_.begin(); it != incoming_.end();)
	{
		if (it->second.source != peer)
		{
			++it;
			continue;
		}
		const auto id = it->first;
		auto* entity = app.findEntity(id);
		if (entity && entity->isBoundaryGhost() && entity->spaceID() == space.id())
			app.destroyEntity(id, false);
		else
			incoming_.erase(it);
		it = incoming_.upper_bound(id);
	}
	sources_.erase(peer);
	for (auto it = outgoing_.begin(); it != outgoing_.end();)
	{
		auto& copies = it->second;
		const auto copy = copies.find(peer);
		if (copy != copies.end())
		{
			const bool witnessed = copy->second.witnessed;
			copies.erase(copy);
			if (witnessed && !isWitnessed(copies))
				if (auto* entity = app.findEntity(it->first); entity && entity->spaceID() == space.id())
					entity->setBoundaryWitnessed(false);
		}
		if (copies.empty())
			it = outgoing_.erase(it);
		else
			++it;
	}
	Entity::bufferCallback(false);
}

CellHandoff::CopyPeers BoundaryGhosts::handoffPeers(ENTITY_ID id, COMPONENT_ID target) const
{
	CellHandoff::CopyPeers peers;
	const auto entry = outgoing_.find(id);
	if (entry != outgoing_.end())
		for (const auto& [app, copy] : entry->second)
			if (app != target)
				peers.emplace_back(app, copy.cell);
	return peers;
}

void BoundaryGhosts::remove(Space& space, Entity& entity, COMPONENT_ID successor)
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
	MemoryStream moved(0);
	const auto* next = successor ? space.layout().cellForApp(successor) : nullptr;
	if (next)
		moved << successor << next->cellID << entity.routingEpoch();
	for (const auto& copy : entry->second)
	{
		if (copy.first == successor)
			continue;
		auto* channel = peerChannel(copy.first);
		if (channel && channel->sessionEpoch() == copy.second.session)
			send(*channel, space, copy.second.cell, entity.id(), copy.second.generation,
				next ? HANDOFF_COPY_MOVED : REMOVE, next ? &moved : nullptr);
	}
	outgoing_.erase(entry);
	entity.setBoundaryWitnessed(false);
}

bool BoundaryGhosts::retainHandoffSource(Space& space, Entity& entity, uint64 session)
{
	// 正在迁入的 Witness 可能尚未计入本地观察者，但客户端仍持有它的引用。
	if (!intersects(space.localCellBounds(), entity.position(), g_kbeSrvConfig.getCellApp().ghostDistance))
		return false;

	const auto source = entity.realCell();
	const auto* cell = space.layout().cellForApp(source);
	const auto* channel = peerChannel(source);
	if (!cell || !channel || channel->sessionEpoch() != session)
		return false;

	// 源已失去权威，只保留坐标节点和已有客户端关系。代次 0 不接受普通增量。
	const auto inserted = incoming_.emplace(entity.id(), Incoming{source, channel->addr(), session, 0,
		cell->cellID, timestamp() + 5 * stampsPerSecond()});
	if (!inserted.second)
		return false;
	entity.retainCellHandoffGhost();
	return true;
}

void BoundaryGhosts::prepareHandoffCopies(Space& space, Entity& entity, const CellHandoff::CopyPeers& peers)
{
	const Pose pose{entity.position(), entity.direction(), entity.layer(), entity.isOnGround()};
	const float distance = g_kbeSrvConfig.getCellApp().ghostDistance;
	std::optional<MemoryStream> snapshot;
	const char* snapshotError = nullptr;
	for (const auto& [app, cell] : peers)
	{
		const auto* member = space.layout().cellForApp(app);
		auto* channel = peerChannel(app);
		if (!channel || !member || member->cellID != cell || handoffCopyGeneration(entity.id(), app, channel->sessionEpoch()))
			continue;

		Neighbour neighbour{app, {}, cell};
		// 继承的正式副本沿用退场滞回距离；源端占位按创建距离保留。
		const float range = app == entity.migrationRelayCell() ? distance : distance + distance * 0.1f;
		if (!space.layout().boundsForApp(app, neighbour.bounds) || !intersects(neighbour.bounds, pose.position, range))
		{
			send(*channel, space, cell, entity.id(), entity.routingEpoch(), HANDOFF_COPY_ABSENT);
			continue;
		}
		if (!snapshot)
		{
			snapshot.emplace();
			snapshotError = writeSnapshot(entity, pose, *snapshot);
			if (snapshotError)
				WARNING_MSG(fmt::format("BoundaryGhosts::prepareHandoffCopies: entity={}, {}.\n", entity.id(), snapshotError));
		}
		if (snapshotError)
		{
			// 已提交的目标无法回滚。脚本在迁入期间生成不可复制状态时，沿用普通 Ghost 的状态上限。
			send(*channel, space, cell, entity.id(), entity.routingEpoch(), HANDOFF_COPY_ABSENT);
			continue;
		}
		const auto queued = channel->bundlesLength();
		if (queued + snapshot->length() + HEADER_ALLOWANCE > MAX_PENDING_BYTES)
			INFO_MSG(fmt::format("BoundaryGhosts::prepareHandoffCopies: reserved send, entity={}, peer={}, queued={}.\n",
				entity.id(), app, queued));
		createCopy(space, entity, neighbour, *channel, pose, *snapshot);
	}
}

void BoundaryGhosts::resolveHandoffCopy(Space& space, COMPONENT_ID previous, ENTITY_ID id, uint64 epoch)
{
	auto* channel = peerChannel(previous);
	const auto* cell = space.layout().cellForApp(previous);
	if (!channel || !cell || !space.canAcceptEntities() || space.layoutState() != SpaceLayout::ACTIVE ||
		version_ != space.layoutVersion())
		return;
	auto* entity = Cellapp::getSingleton().findEntity(id);
	if (entity && entity->spaceID() == space.id() && !entity->isDestroyed())
	{
		// 在途交接由提交端通知新权威。旧目标已变成 Ghost 时不能代替新 Real 撤回占位。
		if (!entity->isReal() || entity->routingEpoch() != epoch || entity->hasFlags(ENTITY_FLAGS_TELEPORT_START))
			return;
		// 查询不占用已接纳交接的预留额度，余量不足时由占位端下一次查询重试。
		if (channel->bundlesLength() <= MAX_PENDING_BYTES - static_cast<int32>(MAX_STATE_BYTES + HEADER_ALLOWANCE))
			prepareHandoffCopies(space, *entity, {{previous, cell->cellID}});
		return;
	}
	send(*channel, space, cell->cellID, id, epoch, HANDOFF_COPY_ABSENT);
}

bool BoundaryGhosts::forgetHandoffTarget(ENTITY_ID id, COMPONENT_ID source, uint64 session, uint64 generation)
{
	const auto entry = incoming_.find(id);
	if (entry == incoming_.end() || generation == 0 || entry->second.source != source ||
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
	if (current == incoming_.end() || current->second.generation == 0)
		return;

	const auto& copy = current->second;
	auto* channel = Cellapp::getSingleton().networkInterface().findChannel(copy.address);
	if (!channel || channel->isDestroyed() || channel->condemn() != 0 ||
		channel->sessionEpoch() != copy.session)
		return;

	MemoryStream data;
	data << static_cast<uint8>(observed);
	send(*channel, space, copy.cell, entity.id(), copy.generation, WITNESSED, &data);
}

void BoundaryGhosts::sendToCopies(Space& space, const Entity& entity, Operation operation, const MemoryStream& data)
{
	const auto entry = outgoing_.find(entity.id());
	if (!entity.isReal() || entry == outgoing_.end() || data.length() > MAX_STATE_BYTES || !space.canAcceptEntities() ||
		space.layoutState() != SpaceLayout::ACTIVE || version_ != space.layoutVersion())
		return;

	for (const auto& copy : entry->second)
	{
		auto* channel = peerChannel(copy.first);
		if (channel && channel->sessionEpoch() == copy.second.session)
			send(*channel, space, copy.second.cell, entity.id(), copy.second.generation, operation, &data);
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
		CELL_ID sourceCell = 0, targetCell = 0;
		ENTITY_ID id = 0;
		uint8 operation = 0;
		stream >> version >> sourceCell >> targetCell >> id >> generation >> operation;
		const auto* sender = space.layout().cellForApp(source);
		if (version != space.layoutVersion() || generation == 0 ||
			stream.length() > MAX_STATE_BYTES || !sender || sender->cellID != sourceCell ||
			!space.pCell() || space.pCell()->id() != targetCell)
			return;

		if (operation == MERGE_FENCE || operation == MERGE_FENCE_ACK)
		{
			uint64 revision;
			if (id == 0 && stream.length() == sizeof(revision))
			{
				stream >> revision;
				if (revision == space.boundaryRevision())
					space.onCellMergeFence(channel, source, generation, operation == MERGE_FENCE_ACK);
			}
			return;
		}
		if (id <= 0)
			return;

		auto& app = Cellapp::getSingleton();
		auto* entity = app.findEntity(id);
		if (operation == WITNESSED)
		{
			if (!entity || (!entity->isReal() && !entity->isCellHandoffSource()) || entity->spaceID() != space.id() ||
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
		if (operation == CHECK_HANDOFF_COPY)
		{
			if (!stream.length())
				resolveHandoffCopy(space, source, id, generation);
			return;
		}
		if (operation == HANDOFF_COPY_ABSENT)
		{
			const auto* peer = peerChannel(source);
			if (!stream.length() && entity && entity->isBoundaryGhost() && entity->spaceID() == space.id() &&
				current != incoming_.end() && peer)
			{
				auto& copy = current->second;
				if (copy.earlyAbsence.receive({source, sourceCell, peer->sessionEpoch(), generation},
					{copy.source, copy.cell, copy.session, entity->routingEpoch()}, !copy.generation))
					app.destroyEntity(id, false);
			}
			return;
		}
		if (operation == CREATE)
		{
			auto& last = sources_[source];
			if (last.session > channel.sessionEpoch() ||
				(last.session == channel.sessionEpoch() && last.version == version && generation <= last.lastCreate))
				return;

			ENTITY_SCRIPT_UID type = 0;
			uint64 routingEpoch = 0;
			Pose pose;
			stream >> type >> routingEpoch;
			auto* module = EntityDef::findScriptModule(type);
			if (!module || !module->hasCell() || routingEpoch == 0 || !pose.read(stream))
				return;

			HandoffCopyAbsence earlyAbsence;
			if (entity)
			{
				if (!entity->isBoundaryGhost() || current == incoming_.end() || entity->spaceID() != space.id())
					return;
				// 新旧 Real 通过不同连接发送消息。新副本先到时，以路由代次确认归属，
				// 旧 Real 随后到达的 REMOVE 仍须匹配副本来源和创建代次。
				const auto currentEpoch = entity->routingEpoch();
				if (routingEpoch < currentEpoch ||
					(routingEpoch == currentEpoch && current->second.source != source))
					return;
				if (routingEpoch == currentEpoch && !current->second.generation)
				{
					// CREATE 已通过入站连接绑定；交接使用的连接也必须仍属于同一会话。
					const auto* handoffChannel = peerChannel(source);
					if (!handoffChannel || handoffChannel->sessionEpoch() != current->second.session ||
						sourceCell != current->second.cell || module != entity->pScriptModule())
						return;
				}
				// 中间一跳的 CREATE 也可能晚到，只保留比这份正式副本更新的缺席通告。
				if (current->second.earlyAbsence.epoch > routingEpoch)
					earlyAbsence = current->second.earlyAbsence;
				app.destroyEntity(id, false);
			}

			entity = app.createEntity(module->getName(), NULL, false, id, false);
			if (!entity)
				return;

			entity->setBoundaryGhost(source);
			entity->routingEpoch(routingEpoch);
			space.addEntity(entity);
			if (!entity->initBoundaryGhostProperties(stream))
			{
				app.destroyEntity(id, false);
				return;
			}
			last = {channel.sessionEpoch(), version, generation};
			incoming_.emplace(id, Incoming{source, channel.addr(), channel.sessionEpoch(), generation, sourceCell, 0, earlyAbsence});
			entity->boundaryGhostSnapshot(++nextIncomingSnapshot_);
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
		case HANDOFF_COPY_MOVED:
		{
			COMPONENT_ID successor = 0;
			CELL_ID nextCell = 0;
			uint64 epoch = 0;
			stream >> successor >> nextCell >> epoch;
			const auto* member = space.layout().cellForApp(successor);
			if (stream.length() || epoch <= entity->routingEpoch() || !member || member->cellID != nextCell)
				break;
			auto* next = peerChannel(successor);
			const auto earlyAbsence = current->second.earlyAbsence;
			if (!next || earlyAbsence.matches({successor, nextCell, next->sessionEpoch(), epoch}))
			{
				app.destroyEntity(id, false);
				break;
			}
			// 这里未移除坐标节点；后到的旧来源消息须重新匹配来源和代次，不能覆盖新快照。
			current->second = {successor, next->addr(), next->sessionEpoch(), 0, nextCell,
				timestamp() + 5 * stampsPerSecond(), earlyAbsence.epoch > epoch ? earlyAbsence : HandoffCopyAbsence{}};
			entity->setBoundaryGhost(successor);
			entity->routingEpoch(epoch);
			entity->boundaryGhostSnapshot(0);
			break;
		}
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
