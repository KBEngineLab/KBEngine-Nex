#include "cellapp.h"
#include "cellapp_interface.h"
#include "../baseapp/baseapp_interface.h"
#include "../cellappmgr/cellappmgr_interface.h"
#include "server/component_routing_guard.h"

#include <algorithm>
#include <cstring>

namespace KBEngine {
namespace {

bool parseLayoutTree(PyObject* value, SpaceLayout::Layout& layout, CELL_ID& nextCell, size_t depth)
{
	using namespace SpaceLayout;
	if (depth > MAX_DEPTH || layout.nodes.size() >= MAX_NODES)
	{
		PyErr_SetString(PyExc_ValueError, "Space layout exceeds the supported depth or Cell count.");
		return false;
	}
	Node node;
	if (PyLong_Check(value) && !PyBool_Check(value))
	{
		node.cellappID = PyLong_AsUnsignedLongLong(value);
		if (PyErr_Occurred())
			return false;
		node.cellID = nextCell++;
		layout.nodes.push_back(node);
		return true;
	}
	if (!PyTuple_Check(value) || PyTuple_Size(value) != 4 ||
		!PyUnicode_Check(PyTuple_GET_ITEM(value, 0)))
	{
		PyErr_SetString(PyExc_TypeError, "Space layout expects a CellApp ID or (axis, position, left, right).");
		return false;
	}
	const char* axis = PyUnicode_AsUTF8(PyTuple_GET_ITEM(value, 0));
	if (axis == NULL)
		return false;
	node.axis = std::strcmp(axis, "X") == 0 ? X : std::strcmp(axis, "Z") == 0 ? Z : LEAF;
	if (node.axis == LEAF)
	{
		PyErr_SetString(PyExc_ValueError, "Space layout axis must be X or Z.");
		return false;
	}
	const double position = PyFloat_AsDouble(PyTuple_GET_ITEM(value, 1));
	if (PyErr_Occurred())
		return false;
	node.position = static_cast<float>(position);
	layout.nodes.push_back(node);
	return parseLayoutTree(PyTuple_GET_ITEM(value, 2), layout, nextCell, depth + 1) &&
		parseLayoutTree(PyTuple_GET_ITEM(value, 3), layout, nextCell, depth + 1);
}

SpaceLayout::Packet spacePacket(const Space& space, SpaceLayout::Operation operation)
{
	SpaceLayout::Packet packet;
	packet.sender = g_componentID;
	packet.operation = operation;
	packet.spaceID = space.id();
	packet.version = space.layoutVersion();
	packet.revision = space.resourceRevision();
	return packet;
}

}

void Space::pCell(Cell* cell)
{
	if (pCell_ != cell)
	{
		SAFE_RELEASE(pCell_);
		pCell_ = cell;
	}
}

bool Space::canChangeSpaceResources() const
{
	return !layoutShutdownRequested_ && !layoutRemovalPending_ &&
		(!hasManagedCell() || layoutState_ == SpaceLayout::ACTIVE);
}

SpaceLayout::Resources Space::layoutResources() const
{
	SpaceLayout::Resources result;
	result.scriptModule = scriptModuleName_;
	result.data = datas_;
	const auto mapping = datas_.find("_mapping");
	if (mapping != datas_.end())
		result.geometryPath = mapping->second;
	result.loadGeometry = geometryShouldLoad_;
	result.geometryParams = geometryParams_;
	return result;
}

bool Space::prepareLayout(const SpaceLayout::Packet& packet, bool replica, std::string& error)
{
	using namespace SpaceLayout;
	const bool managedPrepare = packet.operation == PREPARE || packet.operation == REPLACEMENT_PREPARE;
	Bounds bounds;
	if (managedPrepare && packet.version == layoutVersion_)
		layoutPreparedVersion_ = packet.version;
	if (!isGood() || layoutRemovalPending_ || !packet.layout.boundsForApp(g_componentID, bounds) ||
		hasPendingMigrationEntities())
	{
		error = "local Space cannot prepare the requested Cell";
		return false;
	}
	if (hasManagedCell())
	{
		if (packet.version != layoutVersion_ ||
			(layoutState_ != PREPARING && layoutState_ != PREPARED))
		{
			error = "local Space already has a layout";
			return false;
		}
		layoutPrepareReceived_ = managedPrepare;
		return true;
	}
	for (const EntityPtr& entity : entities_)
	{
		if (!entity->isReal())
			continue;
		const Node* target = packet.layout.cellAt(entity->position().x, entity->position().z);
		if (target == NULL || target->cellappID != g_componentID)
		{
			error = "existing Real entities must stay in their current Cell during fixed layout creation";
			return false;
		}
	}
	layout_ = packet.layout;
	localCellBounds_ = bounds;
	layoutVersion_ = packet.version;
	boundaryRevision_ = 0;
	pendingBoundaryRevision_ = 0;
	boundaryMoveError_.clear();
	layoutState_ = PREPARING;
	layoutError_.clear();
	layoutPinned_ = true;
	layoutReplica_ = replica;
	layoutReadySent_ = false;
	layoutPrepareReceived_ = managedPrepare;
	if (layoutPrepareReceived_)
		layoutPreparedVersion_ = packet.version;
	layoutStarted_ = timestamp();
	pCell(new Cell(layout_.cellForApp(g_componentID)->cellID));
	applyLayoutResources(packet.resources, packet.revision);
	return true;
}

void Space::applyLayoutResources(const SpaceLayout::Resources& resources, uint64 revision, bool retryGeometry)
{
	applyingLayoutResources_ = true;
	std::vector<std::string> removed;
	for (const auto& entry : datas_)
		if (entry.first != "_mapping" && resources.data.count(entry.first) == 0)
			removed.push_back(entry.first);
	for (const std::string& key : removed)
		delSpaceData(key);
	for (const auto& entry : resources.data)
		if (entry.first != "_mapping")
			setSpaceData(entry.first, entry.second);
	if (resources.geometryPath.empty())
	{
		if (!getGeometryPath().empty())
			unLoadSpaceGeometry();
	}
	else if (getGeometryPath() != resources.geometryPath ||
		geometryShouldLoad_ != resources.loadGeometry || geometryParams_ != resources.geometryParams ||
		(retryGeometry && resources.loadGeometry && pNavHandle_ == NULL && !isGeometryLoading()))
	{
		// 同一路径只调整服务端加载参数时保留客户端映射，由几何加载入口更新本地句柄。
		addSpaceGeometryMapping(resources.geometryPath, resources.loadGeometry, resources.geometryParams);
	}
	resourceRevision_ = revision;
	applyingLayoutResources_ = false;
}

void Space::applyLayoutData(const std::string& key, const std::string& value,
	bool deleted, uint64 revision)
{
	if (revision <= resourceRevision_)
		return;
	applyingLayoutResources_ = true;
	if (deleted)
		delSpaceData(key);
	else
		setSpaceData(key, value);
	resourceRevision_ = revision;
	applyingLayoutResources_ = false;
}

void Space::beginBoundaryMove(uint64 revision)
{
	pendingBoundaryRevision_ = revision;
	boundaryMoveError_.clear();
}

bool Space::applyBoundaryMove(const SpaceLayout::Packet& packet)
{
	if (!hasManagedCell() || layoutState_ != SpaceLayout::ACTIVE ||
		packet.revision != boundaryRevision_ + 1 ||
		!layout_.moveBoundary(packet.boundaryIndex, packet.boundaryPosition))
		return false;
	// 参与节点不变，只重新计算区域；正在交接的实体仍按原布局版本收尾。
	layout_.boundsForApp(g_componentID, localCellBounds_);
	boundaryRevision_ = packet.revision;
	splitMigration_.reset();
	return true;
}

void Space::finishBoundaryMove(uint64 revision, const std::string& error)
{
	if (revision == pendingBoundaryRevision_)
	{
		pendingBoundaryRevision_ = 0;
		boundaryMoveError_ = error;
	}
}

bool Space::beginCellSplit(const SpaceLayout::Packet& packet, const Network::Channel& manager, bool candidate)
{
	using namespace SpaceLayout;
	if (split_ || !isGood() || layoutShutdownRequested_ || layoutRemovalPending_)
		return false;
	const auto* next = packet.layout.cellForApp(g_componentID);
	if (!next || candidate != (g_componentID == packet.splitTarget))
		return false;
	if (!candidate && (!hasManagedCell() || layoutState_ != ACTIVE || layoutVersion_ != packet.version ||
		layout_.owner != packet.layout.owner || !pCell() || pCell()->id() != next->cellID ||
		resourceRevision_ != packet.revision || layout_.cellForApp(packet.splitTarget)))
		return false;
	split_ = SplitPreparation{packet.layout, manager.addr(), manager.sessionEpoch(),
		packet.splitAttempt, packet.splitSource, packet.splitTarget, 0, candidate};
	if (candidate)
		layoutVersion_ = packet.version;
	return true;
}

bool Space::isPendingSplitPeer(COMPONENT_ID cid) const
{
	return split_ && split_->applied && (split_->candidate || split_->target == cid);
}

bool Space::needsSplitPeer(COMPONENT_ID cid) const
{
	return split_ && cid != g_componentID &&
		(split_->candidate ? (split_->applied ? layout_ : split_->layout).cellForApp(cid) != nullptr :
			split_->target == cid);
}

void Space::updateCellSplitPreparation()
{
	auto& app = Cellapp::getSingleton();
	auto* manager = app.networkInterface().findChannel(split_->manager);
	if (!manager || manager->isDestroyed() || manager->condemn() != 0 ||
		manager->sessionEpoch() != split_->managerSession)
	{
		if (isStagedSplitCell())
			removeLayoutSpace();
		else
		{
			if (split_->candidate)
				setLayoutState(SpaceLayout::FAILED, "split manager connection was lost");
			split_.reset();
		}
		return;
	}
	if (split_->applied || layoutRemovalPending_ || isGeometryLoading() ||
		(split_->readyRevision && (!split_->candidate || split_->readyRevision == resourceRevision_)))
		return;
	auto& components = Components::getSingleton();
	for (const auto& node : split_->layout.nodes)
	{
		if (node.axis != SpaceLayout::LEAF || !needsSplitPeer(node.cellappID))
			continue;
		const auto* channel = components.findComponentChannel(CELLAPP_TYPE, node.cellappID);
		if (!channel || channel->isDestroyed() || channel->condemn() != 0)
			return;
	}
	auto ready = spacePacket(*this, SpaceLayout::SPLIT_READY);
	ready.splitAttempt = split_->attempt;
	if (app.sendSpaceLayout(ready, manager))
	{
		split_->readyRevision = resourceRevision_;
		if (split_->candidate)
			setLayoutState(SpaceLayout::PREPARED);
	}
}

bool Space::applyCellSplit(const SpaceLayout::Packet& packet, const Network::Channel& manager)
{
	using namespace SpaceLayout;
	if (!split_ || split_->managerSession != manager.sessionEpoch() || split_->attempt != packet.splitAttempt ||
		split_->applied || !split_->readyRevision ||
		(split_->candidate ? layoutState_ != PREPARED :
			(layoutState_ != ACTIVE || packet.revision != boundaryRevision_ + 1)))
		return false;
	// 扩容不替换已存在的 Cell，也不改变 Ghost 和在途交接使用的 Layout 版本。
	layout_ = std::move(split_->layout);
	layout_.boundsForApp(g_componentID, localCellBounds_);
	boundaryRevision_ = packet.revision;
	split_->applied = true;
	splitMigration_.reset();
	if (split_->candidate)
	{
		layoutPreparedVersion_ = layoutVersion_;
		setLayoutState(COMMITTING);
	}
	return true;
}

bool Space::finishCellSplit(const SpaceLayout::Packet& packet, const Network::Channel& manager, bool cancel)
{
	if (!split_ || split_->managerSession != manager.sessionEpoch() || split_->attempt != packet.splitAttempt ||
		split_->applied == cancel || (!cancel && packet.revision != boundaryRevision_))
		return false;
	if (isStagedSplitCell())
		removeLayoutSpace();
	else
	{
		if (!cancel && split_->source == g_componentID)
			splitMigration_ = SplitMigrationScan{split_->attempt, split_->target, SplitMigrationScan::CheckingSource};
		else if (!cancel && split_->candidate)
			splitMigration_ = SplitMigrationScan{split_->attempt, split_->source, SplitMigrationScan::WaitingForSource};
		split_.reset();
	}
	return true;
}

void Space::checkSplitMigrationTarget(const SpaceLayout::Packet& packet)
{
	if (splitMigration_ && splitMigration_->stage == SplitMigrationScan::WaitingForSource &&
		layoutState_ == SpaceLayout::ACTIVE && packet.version == layoutVersion_ &&
		packet.revision == boundaryRevision_ && packet.splitAttempt == splitMigration_->attempt)
		splitMigration_->stage = SplitMigrationScan::CheckingTarget;
}

bool Space::hasSplitSourceEntityToMigrate(Entity& entity, COMPONENT_ID target) const
{
	if (entity.isDestroyed() || entity.isSpace())
		return false;
	if (entity.isCellHandoffSource())
		return entity.realCell() == target;
	const auto& pos = entity.position();
	if (!entity.isReal() || !localCellBounds_.crossedBy(pos.x, pos.z, Cellapp::getSingleton().cellHandoffMargin()))
		return false;
	const auto* destination = layout_.cellAt(pos.x, pos.z);
	return destination && destination->cellappID == target;
}

void Space::updateCellSplitMigration()
{
	if (!splitMigration_ || layoutState_ != SpaceLayout::ACTIVE || !canAcceptEntities() ||
		splitMigration_->stage == SplitMigrationScan::WaitingForSource ||
		timestamp() < splitMigration_->nextCheck)
		return;
	auto& scan = *splitMigration_;
	auto& app = Cellapp::getSingleton();
	const auto retry = [&]()
	{
		scan.cursor = 0;
		scan.nextCheck = timestamp() + stampsPerSecond();
	};
	if (scan.stage == SplitMigrationScan::CheckingSource)
	{
		const auto end = std::min(entities_.size(), scan.cursor + 128);
		for (; scan.cursor < end; ++scan.cursor)
		{
			if (hasSplitSourceEntityToMigrate(*entities_[scan.cursor], scan.peer))
			{
				retry();
				return;
			}
		}
		if (scan.cursor < entities_.size())
			return;
		// 分批扫描期间可能刚发起交接，等目标安装或源端回退后才能请求目标检查。
		if (app.hasUnreleasedCellHandoff(id_, layoutVersion_, scan.peer))
		{
			retry();
			return;
		}
	}
	else if (app.hasUnfinishedIncomingCellHandoff(id_, layoutVersion_, scan.peer))
	{
		retry();
		return;
	}
	auto result = spacePacket(*this, SpaceLayout::SPLIT_DRAINED);
	result.revision = boundaryRevision_;
	result.splitAttempt = scan.attempt;
	if (app.sendSpaceLayout(result))
		splitMigration_.reset();
}

void Space::setLayoutState(SpaceLayout::State state, const std::string& error)
{
	if (state == SpaceLayout::FAILED || state == SpaceLayout::RELEASING)
	{
		splitMigration_.reset();
		if (isStagedSplitCell())
			removeLayoutSpace();
		else
			split_.reset();
	}
	layoutState_ = state;
	layoutError_ = error;
}

void Space::abortLayout(const std::string& error)
{
	setLayoutState(SpaceLayout::FAILED, error);
	if (layoutReplica_)
	{
		removeLayoutSpace();
		return;
	}
	layoutPinned_ = false;
	pCell(NULL);
	// 申请未被管理器接受时不会有 REMOVE，仍要完成已请求的本地删除。
	if (layoutShutdownRequested_)
		removeLayoutSpace();
}

void Space::updateLayoutPreparation()
{
	using namespace SpaceLayout;
	if (split_)
		updateCellSplitPreparation();
	if (split_ && split_->candidate)
		return;
	if (!hasManagedCell())
		return;
	if (isPreparingReplacement())
	{
		auto* manager = replacementManagerChannel();
		if (!manager)
		{
			// 管理器失联后临时区域不能留给下一次恢复使用。
			removeLayoutSpace();
			return;
		}
		if (layoutRemovalPending_ || layoutState_ != PREPARING || isGeometryLoading())
			return;
		auto& app = Cellapp::getSingleton();
		auto ready = spacePacket(*this, REPLACEMENT_READY);
		ready.replacementProcess = CellRecovery::recoveryProcessID();
		if (app.sendSpaceLayout(ready, manager))
			setLayoutState(PREPARED);
		return;
	}
	if ((layoutState_ == PREPARING || layoutState_ == PREPARED || layoutState_ == COMMITTING) &&
		timestamp() - layoutStarted_ > stampsPerSecond() * 45)
	{
		// 管理器失联时保留区域，不能自行认定创建失败后重新分配实体归属。
		setLayoutState(FAILED, "manager confirmation timed out");
		return;
	}
	if (layoutState_ != PREPARING || !layoutPrepareReceived_ || layoutReadySent_ || isGeometryLoading())
		return;
	// READY 等待加载任务结束，不要求存在导航句柄。加载失败也要完成几何映射回调。
	Packet packet = spacePacket(*this, READY);
	if (Cellapp::getSingleton().sendSpaceLayout(packet))
	{
		layoutReadySent_ = true;
		setLayoutState(PREPARED);
	}
}

void Space::bindReplacementManager(const Network::Channel& channel, const SpaceLayout::Packet& packet)
{
	replacementTemporary_ = true;
	replacementManagerAddress_ = channel.addr();
	replacementManagerSession_ = channel.sessionEpoch();
	layoutVersion_ = packet.version;
	resourceRevision_ = packet.revision;
	replacedCellApp_ = packet.replacedCellApp;
	replacedLayoutVersion_ = packet.replacedLayoutVersion;
}

bool Space::canStageRecovery(const CellRecovery::StageKey& key) const
{
	return isGood() && !layoutShutdownRequested_ && !layoutRemovalPending_ &&
		isPreparingReplacement() && layoutState_ == SpaceLayout::PREPARED &&
		matchesRecovery(key) && replacementManagerChannel();
}

bool Space::matchesRecovery(const CellRecovery::StageKey& key) const
{
	return key.space == id_ && key.lost == replacedCellApp_ &&
		key.originalVersion == replacedLayoutVersion_ && key.replacementVersion == layoutVersion_;
}

bool Space::checkReplacement(const SpaceLayout::Packet& packet)
{
	if (!isGood() || !hasManagedCell() || layoutShutdownRequested_ || layoutRemovalPending_ ||
		layoutState_ != SpaceLayout::FAILED || layoutVersion_ != packet.replacedLayoutVersion ||
		resourceRevision_ != packet.revision ||
		!layout_.cellForApp(packet.replacedCellApp) || layout_.cellForApp(packet.replacementCellApp))
		return false;
	// 保留旧版直到在途迁移结束，同时拒绝新的迁入和实体创建。
	replacementCheckVersion_ = packet.version;
	Cellapp::getSingleton().resolvePeerCellHandoffsAfterExit(packet.replacedCellApp, 0, id_, layoutVersion_);
	return !hasPendingMigrationEntities(true);
}

bool Space::commitReplacement(const SpaceLayout::Packet& packet, Network::Channel* manager)
{
	using namespace SpaceLayout;
	if (isPreparingReplacement())
	{
		if (replacementManagerChannel() != manager || layoutState_ != PREPARED ||
			packet.version != layoutVersion_ || packet.replacedCellApp != replacedCellApp_ ||
			packet.replacedLayoutVersion != replacedLayoutVersion_ || packet.replacementCellApp != g_componentID)
			return false;
		replacementTemporary_ = false;
		replacementCheckVersion_ = packet.version;
		layoutReplica_ = layout_.owner != g_componentID;
		Cellapp::getSingleton().discardStagedRecoveryBackups(id_, layoutVersion_);
	}
	else
	{
		if (replacementCheckVersion_ != packet.version || layoutVersion_ != packet.replacedLayoutVersion ||
			!layout_.replaceCellApp(packet.replacedCellApp, packet.replacementCellApp))
			return false;
		layoutVersion_ = packet.version;
		layoutReplica_ = layout_.owner != g_componentID;
	}
	boundaryRevision_ = 0;
	pendingBoundaryRevision_ = 0;
	boundaryMoveError_.clear();
	layoutStarted_ = timestamp();
	setLayoutState(COMMITTING);
	return true;
}

void Space::cancelReplacementCheck(uint64 version)
{
	if (replacementCheckVersion_ == version)
		replacementCheckVersion_ = 0;
}

Network::Channel* Space::replacementManagerChannel() const
{
	if (!isRecoveryTarget())
		return nullptr;
	auto* channel = Cellapp::getSingleton().networkInterface().findChannel(replacementManagerAddress_);
	return channel && !channel->isDestroyed() && channel->condemn() == 0 &&
		channel->sessionEpoch() == replacementManagerSession_ ? channel : nullptr;
}

bool Cellapp::sendSpaceLayout(const SpaceLayout::Packet& packet)
{
	return sendSpaceLayout(packet, Components::getSingleton().getCellappmgrChannel());
}

bool Cellapp::sendSpaceLayout(const SpaceLayout::Packet& packet, Network::Channel* channel)
{
	if (!spacePartitioningEnabled() || !spacePartitioningConfirmed() ||
		channel == NULL || channel->isDestroyed() || channel->condemn() != 0)
		return false;
	MemoryStream stream;
	if ((packet.operation == SpaceLayout::REQUEST || packet.operation == SpaceLayout::RESOURCES) &&
		!packet.resources.validate())
		return false;
	packet.write(stream);
	SpaceLayout::Packet checked;
	if (!checked.read(stream))
		return false;
	Network::Bundle* bundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
	bundle->newMessage(CellappmgrInterface::onSpaceLayoutRequest);
	bundle->append(&stream);
	channel->send(bundle);
	return true;
}

void Cellapp::queueLayoutPeerConnections(const SpaceLayout::Packet& packet, COMPONENT_ID only)
{
	auto& components = Components::getSingleton();
	for (const auto& entry : packet.cellPeerRoutes)
	{
		if (entry.first <= componentID_ || (only && entry.first != only))
			continue;
		const auto* current = components.findComponent(entry.first);
		if (current && (current->componentType != CELLAPP_TYPE ||
			(current->pChannel && !current->pChannel->isDestroyed() && current->pChannel->condemn() == 0)))
			continue;
		const auto& address = entry.second;
		Components::ComponentInfos peer;
		peer.uid = getUserUID();
		peer.cid = entry.first;
		peer.componentType = CELLAPP_TYPE;
		peer.pid = address.pid;
		peer.globalOrderid = address.globalOrder;
		peer.groupOrderid = address.groupOrder;
		peer.gus = address.gus;
		peer.extradata3 = 0;
		peer.pIntAddr.reset(new Network::Address(address.internalIP, address.internalPort));
		peer.pExtAddr.reset(new Network::Address(address.externalIP, address.externalPort));
		std::memcpy(peer.username, address.username.c_str(), address.username.size() + 1);
		std::memcpy(peer.externalAddressEx, address.externalAddress.c_str(), address.externalAddress.size() + 1);
		queueCellPeerReconnect(peer);
	}
}

void Cellapp::reportLayoutPresence(Space& space)
{
	auto* manager = Components::getSingleton().getCellappmgrChannel();
	if (!manager)
		return;
	auto* presence = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
	presence->newMessage(CellappmgrInterface::updateSpaceData);
	(*presence) << componentID_ << space.id() << space.getScriptModuleName() << false << space.getGeometryPath();
	manager->send(presence);
}

void Cellapp::onSpaceLayout(Network::Channel* channel, MemoryStream& stream)
{
	using namespace SpaceLayout;
	if (!spacePartitioningEnabled() || !spacePartitioningConfirmed() ||
		channel == NULL || channel->isExternal() || channel->isDestroyed() || channel->condemn() != 0)
	{
		stream.done();
		return;
	}
	Components& components = Components::getSingleton();
	Components::ComponentInfos* source = components.findComponent(channel);
	if (source == NULL && channel->componentID() != 0)
		source = components.findComponent(CELLAPPMGR_TYPE, channel->componentID());
	if (source == NULL || source->componentType != CELLAPPMGR_TYPE ||
		!Security::isBoundBidirectionalComponentSource(source->cid, source, channel, channel->componentID()))
	{
		WARNING_MSG(fmt::format("Cellapp::onSpaceLayout: rejected unbound channel({}).\n", channel->c_str()));
		stream.done();
		return;
	}
	const COMPONENT_ID sender = source->cid;
	Packet packet;
	const bool parsed = packet.read(stream);
	stream.done();
	if (!parsed || packet.sender != sender)
		return;
	Space* space = Spaces::findSpace(packet.spaceID);
	if (packet.operation == SPLIT_CHECK_TARGET)
	{
		if (space)
			space->checkSplitMigrationTarget(packet);
		return;
	}
	if (packet.operation == SPLIT_CANCEL)
	{
		const bool removing = space && space->layoutVersion() == packet.version &&
			space->finishCellSplit(packet, *channel, true);
		if (packet.splitTarget == componentID_ && !removing)
		{
			// 候选未创建临时 Space 时不必等待下一次 Tick。
			packet.sender = componentID_;
			packet.operation = SPLIT_DISCARDED;
			sendSpaceLayout(packet, channel);
		}
		return;
	}
	if (packet.operation == SPLIT_PREPARE || packet.operation == SPLIT_STAGE)
	{
		const bool candidate = packet.operation == SPLIT_PREPARE;
		bool created = false;
		if (!isShuttingdown() && candidate && !space && packet.splitTarget == componentID_)
		{
			space = Spaces::createNewSpace(packet.spaceID, packet.resources.scriptModule, false);
			created = space != nullptr;
		}
		std::string error;
		if (!isShuttingdown() && space && (!candidate || created) &&
			space->beginCellSplit(packet, *channel, candidate))
		{
			if (!candidate || space->prepareLayout(packet, true, error))
			{
				queueLayoutPeerConnections(packet, candidate ? 0 : packet.splitTarget);
				space->updateLayoutPreparation();
				return;
			}
		}
		if (created)
			space->removeLayoutSpace();
		packet.sender = componentID_;
		packet.operation = SPLIT_READY;
		packet.success = false;
		packet.error = error.empty() ? "CellApp cannot prepare the requested split" : error;
		sendSpaceLayout(packet, channel);
		return;
	}
	if (packet.operation == SPLIT_APPLY || packet.operation == SPLIT_COMPLETE)
	{
		if (!space || space->layoutVersion() != packet.version)
			return;
		if (packet.operation == SPLIT_APPLY && space->applyCellSplit(packet, *channel))
		{
			if (space->canReceiveSplitGhosts())
				reportLayoutPresence(*space);
			packet.sender = componentID_;
			packet.operation = SPLIT_APPLIED;
			sendSpaceLayout(packet, channel);
		}
		else if (packet.operation == SPLIT_COMPLETE && space->finishCellSplit(packet, *channel, false) &&
			space->layoutState() == COMMITTING)
		{
			space->setLayoutState(ACTIVE);
		}
		return;
	}
	if (packet.operation == REPLACEMENT_PREPARE)
	{
		Packet response;
		response.sender = componentID_;
		response.operation = REPLACEMENT_READY;
		response.spaceID = packet.spaceID;
		response.version = packet.version;
		response.revision = packet.revision;
		response.success = false;
		if (isShuttingdown() || space || !packet.layout.cellForApp(componentID_))
			response.error = "CellApp cannot prepare this replacement Space";
		else if ((space = Spaces::createNewSpace(packet.spaceID, packet.resources.scriptModule, false)))
		{
			// 几何和 SpaceData 在准备阶段只装入本地，不派发业务回调。
			space->bindReplacementManager(*channel, packet);
			if (space->prepareLayout(packet, true, response.error))
			{
				space->updateLayoutPreparation();
				return;
			}
			space->removeLayoutSpace();
		}
		else
			response.error = "could not create the replacement Space";
		sendSpaceLayout(response, channel);
		return;
	}
	if (packet.operation == REPLACEMENT_CANCEL)
	{
		if (space && !space->isPreparingReplacement())
		{
			if (space->layoutVersion() < packet.version)
				space->cancelReplacementCheck(packet.version);
			return;
		}
		if (space && space->isPreparingReplacement() && packet.version == space->layoutVersion())
		{
			if (space->replacementManagerChannel() == channel)
				space->removeLayoutSpace();
			return;
		}
		packet.sender = componentID_;
		packet.operation = REMOVED;
		sendSpaceLayout(packet, channel);
		return;
	}
	if (packet.operation == REPLACEMENT_CHECK || packet.operation == REPLACEMENT_COMMIT)
	{
		if (!space || !space->isGood())
			return;
		if (packet.operation == REPLACEMENT_CHECK && space->checkReplacement(packet))
		{
			packet.sender = componentID_;
			packet.operation = REPLACEMENT_CHECKED;
			sendSpaceLayout(packet, channel);
		}
		else if (packet.operation == REPLACEMENT_COMMIT && space->commitReplacement(packet, channel))
			sendSpaceLayout(spacePacket(*space, COMMITTED), channel);
		return;
	}
	// 原布局的新旧确认都不能激活或删除临时区域。
	if (space && space->isPreparingReplacement())
		return;
	if (packet.operation == PREPARE)
	{
		Packet response;
		response.sender = componentID_;
		response.operation = READY;
		response.spaceID = packet.spaceID;
		response.version = packet.version;
		response.revision = packet.revision;
		response.success = false;
		const bool replica = packet.layout.owner != componentID_;
		bool created = false;
		if (space == NULL && replica)
		{
			space = Spaces::createNewSpace(packet.spaceID, packet.resources.scriptModule, false);
			created = space != NULL;
		}
		if (space == NULL)
			response.error = "the existing Space owner is unavailable";
		else if (replica && !created && (!space->hasManagedCell() || space->layoutVersion() != packet.version))
			response.error = "this CellApp already holds another local Space with the same ID";
		else if (space->prepareLayout(packet, replica, response.error))
		{
			space->updateLayoutPreparation();
			return;
		}
		if (created)
			space->removeLayoutSpace();
		sendSpaceLayout(response);
		return;
	}
	if (space == NULL || packet.version != space->layoutVersion())
	{
		if (packet.operation == REMOVE)
		{
			packet.sender = componentID_;
			packet.operation = REMOVED;
			sendSpaceLayout(packet);
		}
		return;
	}
	switch (packet.operation)
	{
	case BOUNDARY_APPLY:
		if (space->applyBoundaryMove(packet))
		{
			packet.sender = componentID_;
			packet.operation = BOUNDARY_APPLIED;
			sendSpaceLayout(packet, channel);
		}
		else
			WARNING_MSG(fmt::format("Cellapp::spaceLayout: boundary update not applied, space={}, "
				"version={}, revision={}, localRevision={}.\n", space->id(), packet.version,
				packet.revision, space->boundaryRevision()));
		break;
	case BOUNDARY_RESULT:
		space->finishBoundaryMove(packet.revision, packet.error);
		break;
	case COMMIT:
		if (space->layoutState() == PREPARED || space->layoutState() == COMMITTING)
		{
			space->setLayoutState(COMMITTING);
			Packet response = spacePacket(*space, COMMITTED);
			sendSpaceLayout(response);
		}
		break;
	case COMPLETE:
		if (space->layoutState() == COMMITTING || space->layoutState() == ACTIVE)
		{
			if (packet.cellPeerRoutes.size() != space->layout().participants().size() ||
				!std::all_of(packet.cellPeerRoutes.begin(), packet.cellPeerRoutes.end(),
					[&](const auto& entry) { return space->layout().cellForApp(entry.first) != NULL; }))
			{
				WARNING_MSG(fmt::format("Cellapp::spaceLayout: peer routes do not match space={}, version={}.\n",
					space->id(), packet.version));
				break;
			}
			space->setLayoutState(ACTIVE);
			space->cancelReplacementCheck(packet.version);
			if (!isShuttingdown() && space->canAcceptEntities())
				queueLayoutPeerConnections(packet);
			reportLayoutPresence(*space);
			INFO_MSG(fmt::format("Cellapp::spaceLayout: active space={}, version={}, cell={}.\n",
				space->id(), packet.version, space->pCell()->id()));
		}
		break;
	case ABORT:
		if (space->layoutState() == PREPARING || space->layoutState() == PREPARED)
			space->abortLayout(packet.error);
		break;
	case SUSPEND:
		space->setLayoutState(FAILED, packet.error);
		break;
	case REMOVE:
		space->setLayoutState(RELEASING);
		space->removeLayoutSpace();
		break;
	case SET_DATA: case DELETE_DATA:
		if (space->layoutState() == ACTIVE || space->isStagedSplitCell() || space->canReceiveSplitGhosts())
			space->applyLayoutData(packet.key, packet.value, packet.operation == DELETE_DATA, packet.revision);
		break;
	case RESOURCES:
		if ((space->layoutState() == ACTIVE || space->isStagedSplitCell() || space->canReceiveSplitGhosts()) &&
			packet.revision > space->resourceRevision())
			space->applyLayoutResources(packet.resources, packet.revision, true);
		break;
	case UPDATE_REJECTED:
	{
		const char* operation = packet.rejectedOperation == SET_DATA ? "setSpaceData" :
			(packet.rejectedOperation == DELETE_DATA ? "delSpaceData" : "addSpaceGeometryMapping");
		WARNING_MSG(fmt::format("Cellapp::spaceLayout: {} rejected, space={}, version={}, revision={}, key={}: {}.\n",
			operation, packet.spaceID, packet.version, packet.revision, packet.key, packet.error));
		break;
	}
	default:
		break;
	}
}

void Cellapp::reqStageRecoveryCellBackup(Network::Channel* channel, MemoryStream& stream)
{
	using CellRecovery::RestoreAction;
	if (!spacePartitioningEnabled() || !spacePartitioningConfirmed() ||
		!channel || channel->isExternal() || channel->isDestroyed() || channel->condemn() != 0)
	{
		stream.done();
		return;
	}
	BoundedStreamReader reader(stream);
	CellRecovery::StageKey key;
	CellRecovery::RecoveryProcesses processes;
	uint8 action = 0;
	uint8 flags = 0;
	auto& components = Components::getSingleton();
	const auto* source = components.findComponent(channel);
	if (!source && channel->componentID())
		source = components.findComponent(BASEAPP_TYPE, channel->componentID());
	if (!source || source->componentType != BASEAPP_TYPE ||
		!Security::isBoundBidirectionalComponentSource(source->cid, source, channel, channel->componentID()) ||
		!key.read(reader) || !processes.read(reader) ||
		!reader.read(action) || action > static_cast<uint8>(RestoreAction::Discard) ||
		!reader.read(flags) || flags > 1)
	{
		stream.done();
		return;
	}
	bool accepted = false;
	auto* space = Spaces::findSpace(key.space);
	const auto operation = static_cast<RestoreAction>(action);
	if ((operation != RestoreAction::Stage && processes.target != CellRecovery::recoveryProcessID()) ||
		(operation == RestoreAction::Stage && processes.target))
	{
		stream.done();
		return;
	}
	if (operation == RestoreAction::Stage || operation == RestoreAction::Install)
	{
		ENTITY_SCRIPT_UID type = 0;
		uint64 routingEpoch = 0;
		std::string data;
		if (!reader.read(type) || !reader.read(routingEpoch) ||
			!reader.readBlob(data, CellRecovery::MAX_STAGED_BACKUP_BYTES) || !reader.empty())
		{
			stream.done();
			return;
		}
		const auto* module = EntityDef::findScriptModule(type);
		if (space && space->matchesRecovery(key) && module && module->hasBase() && module->hasCell())
		{
			if (operation == RestoreAction::Install)
			{
				// Base 和管理器走不同连接，安装请求可能早于 COMPLETE 到达。
				if (space->layoutState() == SpaceLayout::COMMITTING || space->layoutState() == SpaceLayout::PREPARED)
				{
					stream.done();
					return;
				}
				const auto result = space->installRecoveryEntity(key, source->cid, processes.base,
					type, routingEpoch, flags != 0, data);
				// 安装名额会在激活或清理后释放。暂不回包，让 Base 按现有间隔重试。
				if (result == Space::RecoveryInstallResult::Busy)
				{
					stream.done();
					return;
				}
				accepted = result == Space::RecoveryInstallResult::Installed;
			}
			else if (!isShuttingdown() && space->canStageRecovery(key) && !findEntity(key.entity))
				accepted = stagedRecoveryBackups_.stage(key, source->cid, type, std::move(data));
		}
	}
	else if (!reader.empty())
	{
		stream.done();
		return;
	}
	else if (space && space->matchesRecovery(key))
		accepted = space->finishRecoveryEntity(key, source->cid, processes.base,
			operation == RestoreAction::Activate, flags != 0);
	else
		accepted = operation == RestoreAction::Discard;
	stream.done();
	auto* bundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
	bundle->newMessage(BaseappInterface::onStageRecoveryCellBackup);
	key.write(*bundle);
	processes.write(*bundle);
	(*bundle) << action << static_cast<uint8>(accepted);
	channel->send(bundle);
}

Space::RecoveryInstallResult Space::installRecoveryEntity(const CellRecovery::StageKey& key,
	COMPONENT_ID base, uint64 baseProcess,
	ENTITY_SCRIPT_UID type, uint64 routingEpoch, bool ownsSpace, const std::string& data)
{
	using Result = RecoveryInstallResult;
	const auto existing = recoveryInstallations_.find(key.entity);
	if (existing != recoveryInstallations_.end())
		return existing->second.base == base && existing->second.baseProcess == baseProcess &&
			(existing->second.entity || existing->second.activated) ? Result::Installed : Result::Rejected;
	auto& app = Cellapp::getSingleton();
	if (app.isShuttingdown() || !canAcceptEntities() || layoutState_ != SpaceLayout::ACTIVE ||
		!replacementManagerChannel() || app.findEntity(key.entity) || data.empty())
		return Result::Rejected;

	auto* module = EntityDef::findScriptModule(type);
	if (ownsSpace && (layout_.owner != g_componentID || scriptModuleName_ != module->getName()))
		return Result::Rejected;
	if (pendingRecoveryActivations_ >= 256)
		return Result::Busy;
	EntityDef::context().currEntityID = key.entity;
	auto* object = module->createObject();
	if (!object)
		return Result::Rejected;
	// 安装确认前只持有本地对象，避免实体出现在脚本查询、坐标系统和 AOI 中。
	EntityPtr entity(app.onCreateEntity(object, module, key.entity), EntityPtr::STEAL_REF);
	MemoryStream stream;
	stream.append(reinterpret_cast<const uint8*>(data.data()), data.size());
	PyObjectPtr attributes;
	try
	{
		attributes = PyObjectPtr(entity->createCellDataFromStream(&stream, true), PyObjectPtr::STEAL_REF);
	}
	catch (const MemoryStreamException&)
	{
	}
	if (!attributes)
	{
		SCRIPT_ERROR_CHECK();
		return Result::Rejected;
	}
	entity->createNamespace(attributes.get(), true);
	entity->routingEpoch(routingEpoch);
	entity->isSpace(ownsSpace);
	entity->baseEntityCall(new EntityCall(module, nullptr, base, key.entity, ENTITYCALL_TYPE_BASE));
	recoveryInstallations_.emplace(key.entity, RecoveryInstallation{base, baseProcess, entity});
	++pendingRecoveryActivations_;
	return Result::Installed;
}

bool Space::finishRecoveryEntity(const CellRecovery::StageKey& key, COMPONENT_ID base, uint64 baseProcess,
	bool activate, bool hasClient)
{
	const auto existing = recoveryInstallations_.find(key.entity);
	if (existing == recoveryInstallations_.end())
		return !activate;
	auto& installation = existing->second;
	if (installation.base != base || installation.baseProcess != baseProcess)
		return false;
	if (!installation.entity)
		return installation.activated == activate;
	auto& app = Cellapp::getSingleton();
	EntityPtr entity(installation.entity);
	installation.entity.clear();
	--pendingRecoveryActivations_;
	if (!activate || !isGood() || app.findEntity(key.entity))
		// 保留处理结果，避免迟到的安装请求在清理后重新占用空间。
		return !activate;

	installation.activated = true;
	app.pEntities()->add(key.entity, entity.get());
	// 先完成坐标安装，再让脚本处理进场和恢复回调。
	Entity::bufferCallback(true);
	addEntity(entity.get());
	if (!entity->isDestroyed())
		entity->onRestore();
	if (!entity->isDestroyed())
		addEntityToNode(entity.get());
	Entity::bufferCallback(false);
	if (!entity->isDestroyed() && hasClient && entity->pScriptModule()->hasClient())
		entity->onGetWitness(true);
	return true;
}

void Cellapp::requestSpaceLayoutShutdown(Space& space)
{
	if (space.layout().owner != componentID_ || space.layoutShutdownRequested() ||
		space.layoutState() == SpaceLayout::RELEASING)
		return;
	if (sendSpaceLayout(spacePacket(space, SpaceLayout::SHUTDOWN)))
		space.markLayoutShutdownRequested();
}

void Cellapp::publishSpaceData(Space& space, const std::string& key,
	const std::string& value, bool deleted)
{
	SpaceLayout::Packet packet = spacePacket(space, deleted ? SpaceLayout::DELETE_DATA : SpaceLayout::SET_DATA);
	packet.key = key;
	packet.value = value;
	if (space.layoutState() != SpaceLayout::ACTIVE || !sendSpaceLayout(packet))
		space.setLayoutState(SpaceLayout::FAILED, "SpaceData update could not be submitted");
}

bool Cellapp::publishSpaceResources(const Space& space, SpaceLayout::Resources resources)
{
	SpaceLayout::Packet packet = spacePacket(space, SpaceLayout::RESOURCES);
	packet.resources = std::move(resources);
	return sendSpaceLayout(packet);
}

PyObject* Cellapp::__py_setSpaceLayout(PyObject*, PyObject* args)
{
	using namespace SpaceLayout;
	SPACE_ID spaceID = 0;
	PyObject* specification = NULL;
	if (!PyArg_ParseTuple(args, "IO:setSpaceLayout", &spaceID, &specification))
		return NULL;
	Cellapp& app = Cellapp::getSingleton();
	Space* space = Spaces::findSpace(spaceID);
	if (!app.spacePartitioningEnabled() || !app.spacePartitioningConfirmed())
	{
		PyErr_SetString(PyExc_RuntimeError, "Space partitioning is disabled or the CellApp is not confirmed.");
		return NULL;
	}
	if (space == NULL || !space->canAcceptEntities() || space->hasManagedCell() || space->isGeometryLoading() ||
		space->layoutVersion() == std::numeric_limits<uint64>::max())
	{
		PyErr_SetString(PyExc_RuntimeError, "Space is missing, busy, or already has a fixed layout.");
		return NULL;
	}
	bool hasOwner = false;
	for (const EntityPtr& entity : space->entities())
		if (entity->isReal() && entity->isSpace())
			hasOwner = true;
	if (!hasOwner)
	{
		PyErr_SetString(PyExc_RuntimeError, "The local CellApp must hold the Real Space owner.");
		return NULL;
	}
	Packet packet;
	packet.sender = app.componentID();
	packet.operation = REQUEST;
	packet.spaceID = spaceID;
	packet.version = space->layoutVersion() + 1;
	packet.revision = 1;
	packet.layout.owner = app.componentID();
	CELL_ID nextCell = 1;
	if (!parseLayoutTree(specification, packet.layout, nextCell, 0))
		return NULL;
	if (!packet.layout.validate())
	{
		PyErr_SetString(PyExc_ValueError, "Invalid BSP, duplicate CellApp IDs, missing owner, or non-finite boundary.");
		return NULL;
	}
	packet.resources = space->layoutResources();
	if (!packet.resources.validate())
	{
		PyErr_SetString(PyExc_ValueError, "Space resources exceed the supported count or byte limits.");
		return NULL;
	}
	MemoryStream encoded;
	packet.write(encoded);
	Packet checked;
	if (!checked.read(encoded))
	{
		PyErr_SetString(PyExc_ValueError, "Space resources exceed the supported count or byte limits.");
		return NULL;
	}
	std::string error;
	if (!space->prepareLayout(packet, false, error))
	{
		PyErr_SetString(PyExc_RuntimeError, error.c_str());
		return NULL;
	}
	if (!app.sendSpaceLayout(packet))
	{
		space->abortLayout("manager connection is unavailable");
		PyErr_SetString(PyExc_RuntimeError, "Manager connection is unavailable.");
		return NULL;
	}
	return PyLong_FromUnsignedLongLong(packet.version);
}

PyObject* Cellapp::__py_moveSpaceBoundary(PyObject*, PyObject* args)
{
	using namespace SpaceLayout;
	SPACE_ID spaceID = 0;
	PyObject *versionArg = NULL, *revisionArg = NULL, *indexArg = NULL;
	float position = 0.f;
	if (!PyArg_ParseTuple(args, "IOOOf:moveSpaceBoundary", &spaceID, &versionArg, &revisionArg, &indexArg, &position))
		return NULL;
	const uint64 version = PyLong_AsUnsignedLongLong(versionArg);
	if (PyErr_Occurred())
		return NULL;
	const uint64 revision = PyLong_AsUnsignedLongLong(revisionArg);
	if (PyErr_Occurred())
		return NULL;
	const auto index = PyLong_AsUnsignedLong(indexArg);
	if (PyErr_Occurred())
		return NULL;
	Cellapp& app = Cellapp::getSingleton();
	if (!app.spacePartitioningEnabled() || !app.autoCellHandoffEnabled())
	{
		PyErr_SetString(PyExc_RuntimeError, "Space partitioning and automatic Cell handoff must both be enabled.");
		return NULL;
	}
	Space* space = Spaces::findSpace(spaceID);
	if (app.isShuttingdown() || !space || !space->canAcceptEntities() || space->layoutState() != ACTIVE ||
		space->layout().owner != app.componentID() || space->boundaryMovePending())
	{
		PyErr_SetString(PyExc_RuntimeError, "Space must be active on its owner CellApp and have no pending boundary move.");
		return NULL;
	}
	if (version != space->layoutVersion() || revision != space->boundaryRevision() ||
		revision == std::numeric_limits<uint64>::max())
	{
		PyErr_SetString(PyExc_ValueError, "Space layout version or boundary revision is not current.");
		return NULL;
	}
	Layout next = space->layout();
	if (index >= MAX_NODES || !next.moveBoundary(static_cast<uint16>(index), position))
	{
		PyErr_SetString(PyExc_ValueError, "Boundary index or new position does not define a valid change.");
		return NULL;
	}
	Packet packet = spacePacket(*space, BOUNDARY_REQUEST);
	packet.revision = revision;
	packet.boundaryIndex = static_cast<uint16>(index);
	packet.boundaryPosition = position;
	if (!app.sendSpaceLayout(packet))
	{
		PyErr_SetString(PyExc_RuntimeError, "Manager connection is unavailable.");
		return NULL;
	}
	space->beginBoundaryMove(revision + 1);
	return PyLong_FromUnsignedLongLong(revision + 1);
}

PyObject* Cellapp::__py_getSpaceLayout(PyObject*, PyObject* args)
{
	SPACE_ID spaceID = 0;
	if (!PyArg_ParseTuple(args, "I:getSpaceLayout", &spaceID))
		return NULL;
	Space* space = Spaces::findSpace(spaceID);
	if (space == NULL)
		Py_RETURN_NONE;
	PyObjectPtr cells(PyList_New(0), PyObjectPtr::STEAL_REF);
	PyObjectPtr boundaries(PyList_New(0), PyObjectPtr::STEAL_REF);
	if (!cells.get() || !boundaries.get())
		return NULL;
	const auto& nodes = space->layout().nodes;
	for (size_t index = 0; index < nodes.size(); ++index)
	{
		const auto& node = nodes[index];
		if (node.axis != SpaceLayout::LEAF)
		{
			PyObjectPtr boundary(Py_BuildValue("(Isf)", static_cast<unsigned int>(index),
				node.axis == SpaceLayout::X ? "X" : "Z", node.position), PyObjectPtr::STEAL_REF);
			if (!boundary.get() || PyList_Append(boundaries.get(), boundary.get()) < 0)
				return NULL;
			continue;
		}
		SpaceLayout::Bounds bounds;
		space->layout().boundsForApp(node.cellappID, bounds);
		PyObjectPtr cell(Py_BuildValue("{s:I,s:K,s:(ffff)}",
			"cellID", node.cellID, "cellappID", static_cast<unsigned long long>(node.cellappID),
			"bounds", bounds.minX, bounds.minZ, bounds.maxX, bounds.maxZ), PyObjectPtr::STEAL_REF);
		if (!cell.get() || PyList_Append(cells.get(), cell.get()) < 0)
			return NULL;
	}
	return Py_BuildValue("{s:K,s:s,s:s,s:K,s:K,s:I,s:O,s:O,s:O,s:O,s:K,s:O,s:s,s:O}",
		"version", static_cast<unsigned long long>(space->layoutVersion()),
		"state", SpaceLayout::stateName(space->layoutState()), "error", space->layoutError().c_str(),
		"resourceRevision", static_cast<unsigned long long>(space->resourceRevision()),
		"ownerCellAppID", static_cast<unsigned long long>(space->layout().owner),
		"localCellID", space->pCell() == NULL ? 0u : space->pCell()->id(),
		"acceptsEntities", space->canAcceptEntities() ? Py_True : Py_False,
		"resourcesReady", space->isGeometryLoading() ? Py_False : Py_True,
		"hasNavigation", space->pNavHandle() == NULL ? Py_False : Py_True,
		"cells", cells.get(),
		"boundaryRevision", static_cast<unsigned long long>(space->boundaryRevision()),
		"boundaryMovePending", space->boundaryMovePending() ? Py_True : Py_False,
		"boundaryMoveError", space->boundaryMoveError().c_str(),
		"boundaries", boundaries.get());
}

PyObject* Cellapp::__py_cellForPosition(PyObject*, PyObject* args)
{
	SPACE_ID spaceID = 0;
	float x = 0.f, z = 0.f;
	if (!PyArg_ParseTuple(args, "Iff:cellForPosition", &spaceID, &x, &z))
		return NULL;
	Space* space = Spaces::findSpace(spaceID);
	if (space == NULL || space->layoutState() != SpaceLayout::ACTIVE || space->isGeometryLoading())
	{
		PyErr_SetString(PyExc_RuntimeError, "Space layout or resources are not ready.");
		return NULL;
	}
	const SpaceLayout::Node* cell = space->layout().cellAt(x, z);
	if (cell == NULL)
	{
		PyErr_SetString(PyExc_ValueError, "Position must contain finite X and Z coordinates.");
		return NULL;
	}
	return Py_BuildValue("(IK)", cell->cellID, static_cast<unsigned long long>(cell->cellappID));
}

}
