#include "cellappmgr.h"
#include "cellappmgr_interface.h"
#include "../cellapp/cellapp_interface.h"
#include "../baseapp/baseapp_interface.h"
#include "server/cell_recovery.h"
#include "server/component_routing_guard.h"
#include "helper/console_helper.h"

#include <algorithm>

namespace KBEngine {

bool Cellappmgr::sendSpaceLayout(COMPONENT_ID cid, const SpaceLayout::Packet& packet)
{
	Components::ComponentInfos* target = Components::getSingleton().findComponent(CELLAPP_TYPE, cid);
	if (!componentReady(cid) || target == NULL || target->pChannel == NULL ||
		target->pChannel->isDestroyed() || target->pChannel->condemn() != 0)
		return false;
	MemoryStream stream;
	packet.write(stream);
	if (stream.length() > SpaceLayout::MAX_PACKET_BYTES)
		return false;
	Network::Bundle* bundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
	bundle->newMessage(CellappInterface::onSpaceLayout);
	bundle->append(&stream);
	target->pChannel->send(bundle);
	return true;
}

bool Cellappmgr::collectCellPeerRoutes(const SpaceLayout::Layout& layout, SpaceLayout::Packet& packet)
{
	auto& components = Components::getSingleton();
	for (COMPONENT_ID cid : layout.participants())
	{
		const auto* component = components.findComponent(CELLAPP_TYPE, cid);
		if (!component || !component->pIntAddr || !component->pExtAddr ||
			component->pIntAddr->ip == 0 || component->pIntAddr->port == 0)
			return false;
		auto& route = packet.cellPeerRoutes[cid];
		route.internalIP = component->pIntAddr->ip;
		route.internalPort = component->pIntAddr->port;
		route.externalIP = component->pExtAddr->ip;
		route.externalPort = component->pExtAddr->port;
		route.pid = component->pid;
		route.globalOrder = component->globalOrderid;
		route.groupOrder = component->groupOrderid;
		route.gus = component->gus;
		route.username.assign(component->username,
			std::find(component->username, component->username + MAX_NAME, '\0'));
		route.externalAddress.assign(component->externalAddressEx,
			std::find(component->externalAddressEx, component->externalAddressEx + MAX_NAME, '\0'));
	}
	return true;
}

void Cellappmgr::broadcastSpaceLayout(SPACE_ID spaceID, SpaceLayout::Transaction& transaction,
	SpaceLayout::Operation operation)
{
	SpaceLayout::Packet packet;
	packet.sender = componentID_;
	packet.operation = operation;
	packet.spaceID = spaceID;
	packet.version = transaction.version;
	packet.revision = transaction.revision;
	packet.success = transaction.error.empty();
	packet.error = transaction.error;
	if (operation == SpaceLayout::PREPARE)
	{
		packet.layout = transaction.layout;
		packet.resources = transaction.resources;
	}
	else if (operation == SpaceLayout::RESOURCES)
		packet.resources = transaction.resources;
	const auto participants = transaction.layout.participants();
	// 激活时以管理器当前的注册地址为准，不能依赖 CellApp 上可能已经丢失的地址。
	if (operation == SpaceLayout::COMPLETE && !collectCellPeerRoutes(transaction.layout, packet))
	{
		failSpaceLayout(spaceID, transaction, "participant endpoint is unavailable", true);
		return;
	}
	bool sent = true;
	for (COMPONENT_ID cid : participants)
		sent = sendSpaceLayout(cid, packet) && sent;
	if (operation == SpaceLayout::RESOURCES)
		sendSplitResource(packet, transaction);
	if (!sent && (operation == SpaceLayout::PREPARE ||
		operation == SpaceLayout::COMMIT || operation == SpaceLayout::COMPLETE))
	{
		failSpaceLayout(spaceID, transaction, "participant connection is unavailable",
			transaction.state != SpaceLayout::PREPARING);
	}
}

void Cellappmgr::failSpaceLayout(SPACE_ID spaceID, SpaceLayout::Transaction& transaction,
	const std::string& error, bool suspend)
{
	stopSpaceSplit(spaceID, transaction, error.c_str());
	stopSpaceMerge(spaceID, transaction, error.c_str());
	transaction.interruptSplitMigration();
	transaction.advance(SpaceLayout::FAILED, timestamp());
	transaction.suspended = suspend;
	transaction.error = error;
	broadcastSpaceLayout(spaceID, transaction, suspend ? SpaceLayout::SUSPEND : SpaceLayout::ABORT);
	finishBoundaryMove(spaceID, transaction, "boundary committed, but layout stopped before all confirmations");
	WARNING_MSG(fmt::format("Cellappmgr::spaceLayout: space={}, version={}, failed={}.\n",
		spaceID, transaction.version, error));
}

void Cellappmgr::reportBoundaryMove(SPACE_ID spaceID, COMPONENT_ID owner, uint64 version, uint64 revision,
	const char* error)
{
	SpaceLayout::Packet result;
	result.sender = componentID_;
	result.operation = SpaceLayout::BOUNDARY_RESULT;
	result.spaceID = spaceID;
	result.version = version;
	result.revision = revision;
	result.success = !error;
	if (error)
		result.error = error;
	sendSpaceLayout(owner, result);
}

void Cellappmgr::finishBoundaryMove(SPACE_ID spaceID, SpaceLayout::Transaction& transaction, const char* error)
{
	if (!transaction.boundaryConfirmation)
		return;
	transaction.boundaryConfirmation.reset();
	reportBoundaryMove(spaceID, transaction.layout.owner, transaction.version, transaction.boundaryRevision, error);
}

void Cellappmgr::requestBoundaryMove(SpaceLayout::Packet& packet, SpaceLayout::Transaction* transaction)
{
	using namespace SpaceLayout;
	const char* error = nullptr;
	std::set<COMPONENT_ID> participants;
	if (!transaction || transaction->layout.owner != packet.sender)
		error = "only the current Space owner can move a boundary";
	else if (isShuttingdown() || !g_kbeSrvConfig.getCellAppMgr().cellappmgr_space_partitioning_auto_handoff ||
		transaction->state != ACTIVE || transaction->shutdownRequested)
		error = "Space must be active with automatic Cell handoff enabled";
	else if (transaction->version != packet.version || transaction->boundaryRevision != packet.revision)
		error = "Space layout version or boundary revision is not current";
	else if (transaction->boundaryConfirmation)
		error = "the previous boundary move is still awaiting confirmation";
	else if (transaction->split || transaction->merge)
		error = "another layout adjustment is in progress";
	else
	{
		participants = transaction->layout.participants();
		if (!std::all_of(participants.begin(), participants.end(), [&](COMPONENT_ID cid) { return componentReady(cid); }))
			error = "a participating CellApp is unavailable";
		else if (!transaction->layout.moveBoundary(packet.boundaryIndex, packet.boundaryPosition))
			error = "boundary index or new position does not define a valid change";
	}
	if (error)
	{
		reportBoundaryMove(packet.spaceID, packet.sender, packet.version, packet.revision + 1, error);
		return;
	}

	// 各 Cell 在管理器连接上按序收到边界与关闭消息；边界发布后不因确认迟到而回退。
	transaction->interruptSplitMigration();
	transaction->boundaryConfirmation = Transaction::BoundaryConfirmation{timestamp(), {}};
	packet.sender = componentID_;
	packet.operation = BOUNDARY_APPLY;
	packet.revision = ++transaction->boundaryRevision;
	bool sent = true;
	for (const auto cid : participants)
		sent = sendSpaceLayout(cid, packet) && sent;
	if (!sent)
		failSpaceLayout(packet.spaceID, *transaction, "boundary update participant is unavailable", true);
	INFO_MSG(fmt::format("Cellappmgr::spaceLayout: boundary moved, space={}, version={}, revision={}, "
		"index={}, position={}.\n", packet.spaceID, packet.version, packet.revision,
		packet.boundaryIndex, packet.boundaryPosition));
}

void Cellappmgr::manageSpaceLayout(Network::Channel* channel, MemoryStream& stream)
{
	using namespace SpaceLayout;
	if (!channel || channel->isExternal() || channel->isDestroyed() || channel->condemn() != 0)
	{
		stream.done();
		return;
	}
	BoundedStreamReader auth(stream);
	if (!auth.skipString(512, false) || !validateManagementAdminToken(channel, stream, "manageSpaceLayout"))
	{
		stream.done();
		return;
	}
	BoundedStreamReader request(stream);
	uint8 action = 0, axis = LEAF;
	SPACE_ID spaceID = 0;
	CELL_ID cell = 0, keeping = 0;
	uint64 version = 0, revision = 0;
	float position = 0;
	const bool valid = request.read(action) && request.read(spaceID) && spaceID &&
		(action == 0 || action == 3 ||
		 ((action == 1 || action == 2) && request.read(version) && request.read(revision) && request.read(cell) &&
			(action == 1 ? request.read(axis) && request.read(position) : request.read(keeping)))) && request.empty();
	stream.done();
	auto found = spaceLayouts_.find(spaceID);
	const char* error = nullptr;
	if (!valid)
		error = "invalid Space management request";
	else if (!spacePartitioningEnabled())
		error = "Space partitioning is disabled";
	else if (found == spaceLayouts_.end())
		error = "Space has no managed layout";
	else if (action == 1)
		error = requestSpaceSplit(spaceID, found->second, version, revision, cell, axis, position);
	else if (action == 2)
		error = requestSpaceMerge(spaceID, found->second, version, revision, cell, keeping);

	static ConsoleInterface::ConsoleQuerySpacesHandler handler;
	auto* bundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
	bundle->newMessage(handler);
	(*bundle) << static_cast<uint8>(!error) << (error ? error : "");
	if (!error)
	{
		const auto& transaction = found->second;
		const auto& split = transaction.split;
		const char* status = split ? (split->published ? "publishing" : "preparing") :
			(!transaction.splitError.empty() ? "failed" : transaction.splitAttempt ? "active" : "none");
		using Migration = Transaction::SplitMigration;
		const auto migration = transaction.splitMigration;
		const char* migrationStatus = transaction.isSplitMigrating() ? "migrating" :
			migration == Migration::Complete ? "complete" : migration == Migration::Interrupted ? "interrupted" : "none";
		(*bundle) << transaction.version << transaction.boundaryRevision << stateName(transaction.state)
			<< transaction.splitAttempt << status << transaction.splitError << migrationStatus
			<< static_cast<uint16>(transaction.layout.nodes.size());
		for (const auto& node : transaction.layout.nodes)
			(*bundle) << node.axis << node.position << node.cellID << node.cellappID;
		// 保留原查询和切分回执，旧版运维工具仍可读取。
		if (action == 2 || action == 3)
		{
			using Merge = Transaction::Merge;
			const auto& merge = transaction.merge;
			const char* mergeStatus = merge ? (merge->stage == Merge::Preparing ? "preparing" :
				merge->stage == Merge::Publishing ? "publishing" :
				(merge->stage == Merge::Cancelling || merge->stage == Merge::Resuming) ? "cancelling" : "draining") :
				(!transaction.mergeError.empty() ? "failed" : transaction.mergeAttempt ? "complete" : "none");
			(*bundle) << transaction.mergeAttempt << mergeStatus << transaction.mergeError;
		}
	}
	channel->send(bundle);
}

const char* Cellappmgr::requestSpaceSplit(SPACE_ID spaceID, SpaceLayout::Transaction& transaction,
	uint64 version, uint64 boundaryRevision, CELL_ID cell, uint8 axis, float position)
{
	using namespace SpaceLayout;
	if (isShuttingdown() || !g_kbeSrvConfig.getCellAppMgr().cellappmgr_space_partitioning_auto_handoff ||
		transaction.state != ACTIVE || transaction.shutdownRequested)
		return "Space must be active with automatic Cell handoff enabled";
	if (transaction.version != version || transaction.boundaryRevision != boundaryRevision)
		return "Space layout changed; run space show again";
	if (transaction.boundaryConfirmation || transaction.split || transaction.merge)
		return "another layout adjustment is in progress";
	if (boundaryRevision == std::numeric_limits<uint64>::max() ||
		transaction.splitAttempt == std::numeric_limits<uint64>::max())
		return "Space layout has no remaining adjustment numbers";
	const auto participants = transaction.layout.participants();
	if (!std::all_of(participants.begin(), participants.end(), [&](COMPONENT_ID cid) { return componentReady(cid); }))
		return "a participating CellApp is unavailable";
	const auto target = findFreeCellapp(spaceID, &transaction);
	const auto* channel = Components::getSingleton().findComponentChannel(CELLAPP_TYPE, target);
	if (!channel || channel->isDestroyed() || channel->condemn() != 0)
		return "no ready CellApp is available for this Space";

	Packet packet;
	packet.sender = componentID_;
	packet.spaceID = spaceID;
	packet.version = version;
	packet.revision = transaction.revision;
	packet.splitAttempt = transaction.splitAttempt + 1;
	packet.splitTarget = target;
	packet.layout = transaction.layout;
	for (const auto& node : packet.layout.nodes)
		if (node.axis == LEAF && node.cellID == cell)
			packet.splitSource = node.cellappID;
	if (!packet.layout.splitCell(cell, axis, position, target, transaction.lastRetiredCellID))
		return "invalid Cell, cut position, or layout limit";
	if (!collectCellPeerRoutes(packet.layout, packet))
		return "a participating CellApp has no registered address";
	transaction.split = Transaction::Split{packet.layout, packet.splitSource, target, channel->sessionEpoch(), timestamp()};
	transaction.splitAttempt = packet.splitAttempt;
	transaction.splitError.clear();
	transaction.splitMigration = Transaction::SplitMigration::None;
	transaction.splitMigrationSource = 0;
	transaction.splitMigrationTarget = 0;
	cellapps_.at(target).reserveSpaceCreation(spaceID);
	packet.operation = SPLIT_PREPARE;
	packet.resources = transaction.resources;
	bool sent = sendSpaceLayout(target, packet);
	packet.operation = SPLIT_STAGE;
	for (const auto cid : participants)
		sent = sendSpaceLayout(cid, packet) && sent;
	if (!sent)
	{
		stopSpaceSplit(spaceID, transaction, "could not send split preparation to all participants");
		return "could not send split preparation to all participants";
	}
	INFO_MSG(fmt::format("Cellappmgr::spaceLayout: preparing split, space={}, attempt={}, cell={}, target={}.\n",
		spaceID, packet.splitAttempt, cell, target));
	return nullptr;
}

void Cellappmgr::handleSpaceSplitAck(const SpaceLayout::Packet& packet, SpaceLayout::Transaction& transaction,
	Network::Channel* channel)
{
	using namespace SpaceLayout;
	auto& split = transaction.split;
	if (!split || transaction.state != ACTIVE || packet.version != transaction.version ||
		packet.splitAttempt != transaction.splitAttempt || !split->layout.cellForApp(packet.sender) ||
		(packet.sender == split->target && channel->sessionEpoch() != split->targetSession))
		return;
	const auto participants = split->layout.participants();
	bool publishing = false;
	if (packet.operation == SPLIT_READY && !split->published)
	{
		if (!packet.success)
		{
			stopSpaceSplit(packet.spaceID, transaction, packet.error.c_str());
			return;
		}
		if (packet.sender == split->target)
			split->readyResourceRevision = packet.revision;
		split->confirmed.insert(packet.sender);
		if (split->confirmed.size() != participants.size() || split->readyResourceRevision != transaction.revision)
			return;
		// 编号和实体归属协议不变，只递增 boundaryRevision。发布后不能因确认迟到撤回新分区。
		transaction.layout = split->layout;
		++transaction.boundaryRevision;
		transaction.splitMigration = Transaction::SplitMigration::CheckingSource;
		transaction.splitMigrationSource = split->source;
		transaction.splitMigrationTarget = split->target;
		split->published = publishing = true;
		split->started = timestamp();
		split->confirmed.clear();
	}
	else if (packet.operation == SPLIT_APPLIED && split->published)
	{
		if (packet.revision != transaction.boundaryRevision)
			return;
		split->confirmed.insert(packet.sender);
		if (split->confirmed.size() != participants.size())
			return;
	}
	else
		return;

	Packet update;
	update.sender = componentID_;
	update.operation = publishing ? SPLIT_APPLY : SPLIT_COMPLETE;
	update.spaceID = packet.spaceID;
	update.version = transaction.version;
	update.revision = transaction.boundaryRevision;
	update.splitAttempt = transaction.splitAttempt;
	// 先通知新 Cell；其他节点在收到 COMPLETE 后才开始向它复制和迁移。
	bool sent = sendSpaceLayout(split->target, update);
	for (const auto cid : participants)
		if (cid != split->target)
			sent = sendSpaceLayout(cid, update) && sent;
	if (!sent)
		failSpaceLayout(packet.spaceID, transaction, "split participant connection is unavailable", true);
	else if (!publishing)
	{
		INFO_MSG(fmt::format("Cellappmgr::spaceLayout: split active, space={}, attempt={}, cells={}.\n",
			packet.spaceID, transaction.splitAttempt, participants.size()));
		split.reset();
	}
}

void Cellappmgr::stopSpaceSplit(SPACE_ID spaceID, SpaceLayout::Transaction& transaction, const char* reason)
{
	using namespace SpaceLayout;
	if (!transaction.split)
		return;
	const auto& split = *transaction.split;
	if (!split.published)
	{
		Packet cancel;
		cancel.sender = componentID_;
		cancel.operation = SPLIT_CANCEL;
		cancel.spaceID = spaceID;
		cancel.version = transaction.version;
		cancel.splitAttempt = transaction.splitAttempt;
		cancel.splitTarget = split.target;
		// 临时 Space 删除前，不能再次把同一个 Space 分配给这个 CellApp。
		cancelledSplits_[split.target].insert_or_assign(spaceID,
			CancelledSplit{split.targetSession, cancel.version, cancel.splitAttempt});
		for (const auto cid : split.layout.participants())
			sendSpaceLayout(cid, cancel);
	}
	transaction.splitError = reason;
	transaction.split.reset();
	WARNING_MSG(fmt::format("Cellappmgr::spaceLayout: split stopped, space={}, attempt={}: {}.\n",
		spaceID, transaction.splitAttempt, reason));
}

void Cellappmgr::sendSplitResource(const SpaceLayout::Packet& packet, SpaceLayout::Transaction& transaction)
{
	if (transaction.split && !transaction.split->published && !sendSpaceLayout(transaction.split->target, packet))
		stopSpaceSplit(packet.spaceID, transaction, "split target could not receive a resource update");
}

const char* Cellappmgr::requestSpaceMerge(SPACE_ID spaceID, SpaceLayout::Transaction& transaction,
	uint64 version, uint64 revision, CELL_ID retiring, CELL_ID keeping)
{
	using namespace SpaceLayout;
	if (isShuttingdown() || !g_kbeSrvConfig.getCellAppMgr().cellappmgr_space_partitioning_auto_handoff ||
		transaction.state != ACTIVE || transaction.shutdownRequested)
		return "Space must be active with automatic Cell handoff enabled";
	if (transaction.version != version || transaction.boundaryRevision != revision)
		return "Space layout changed; run space show again";
	if (transaction.boundaryConfirmation || transaction.split || transaction.isSplitMigrating() || transaction.merge)
		return "another layout adjustment is in progress";
	if (revision == std::numeric_limits<uint64>::max() ||
		transaction.mergeAttempt == std::numeric_limits<uint64>::max())
		return "Space layout has no remaining adjustment numbers";
	auto next = transaction.layout;
	COMPONENT_ID source = 0;
	if (!next.mergeCells(retiring, keeping, source))
		return "merge requires sibling leaf Cells and must keep the Space owner";
	// BaseApp 可能仍在向已接管的 Cell 补发恢复数据，当前没有恢复任务全部完成的确认。
	if (transaction.committedReplacement && transaction.committedReplacement->target == source)
		return "a Cell that took over a failed partition cannot be retired";
	const auto participants = transaction.layout.participants();
	if (!std::all_of(participants.begin(), participants.end(), [&](COMPONENT_ID cid) { return componentReady(cid); }))
		return "a participating CellApp is unavailable";

	using Merge = Transaction::Merge;
	transaction.merge = Merge{std::move(next), source, timestamp()};
	transaction.mergeError.clear();
	Packet packet;
	packet.sender = componentID_;
	packet.operation = MERGE_PREPARE;
	packet.spaceID = spaceID;
	packet.version = version;
	packet.revision = revision;
	packet.mergeAttempt = ++transaction.mergeAttempt;
	packet.mergeRetiring = retiring;
	packet.mergeKeeping = keeping;
	bool sent = true;
	for (auto cid : participants)
		sent = sendSpaceLayout(cid, packet) && sent;
	if (!sent)
	{
		stopSpaceMerge(spaceID, transaction, "could not send merge preparation to all participants");
		return "could not send merge preparation to all participants";
	}
	INFO_MSG(fmt::format("Cellappmgr::spaceLayout: preparing merge, space={}, attempt={}, retiring={}, keeping={}.\n",
		spaceID, packet.mergeAttempt, retiring, keeping));
	return nullptr;
}

void Cellappmgr::handleSpaceMergeAck(SpaceLayout::Packet& packet, SpaceLayout::Transaction& transaction)
{
	using namespace SpaceLayout;
	using Merge = Transaction::Merge;
	auto& merge = transaction.merge;
	if (!merge || transaction.state != ACTIVE || transaction.version != packet.version ||
		transaction.boundaryRevision != packet.revision || transaction.mergeAttempt != packet.mergeAttempt)
		return;
	const auto sender = packet.sender;
	const auto participants = transaction.layout.participants();
	const bool member = participants.count(sender) != 0;
	packet.sender = componentID_;
	if (packet.operation == MERGE_BLOCKED)
	{
		if (sender != merge->source)
			return;
		if (merge->stage == Merge::Preparing)
			stopSpaceMerge(packet.spaceID, transaction, packet.error.c_str());
		else if (merge->stage == Merge::Draining)
		{
			transaction.mergeError = packet.error;
			merge->stage = Merge::Cancelling;
			merge->started = timestamp();
			merge->confirmed.clear();
			packet.operation = MERGE_CANCEL;
			WARNING_MSG(fmt::format("Cellappmgr::spaceLayout: cancelling merge, space={}, attempt={}: {}.\n",
				packet.spaceID, packet.mergeAttempt, packet.error));
			if (!sendSpaceLayout(merge->source, packet))
				failSpaceLayout(packet.spaceID, transaction, "merge source connection is unavailable", true);
		}
		return;
	}
	switch (merge->stage)
	{
	case Merge::Preparing:
		if (packet.operation != MERGE_READY || !member)
			return;
		if (!packet.success)
		{
			stopSpaceMerge(packet.spaceID, transaction, "a CellApp rejected merge preparation");
			return;
		}
		merge->confirmed.insert(sender);
		DEBUG_MSG(fmt::format("Cellappmgr::spaceLayout: merge ready, space={}, attempt={}, cellapp={}.\n",
			packet.spaceID, packet.mergeAttempt, sender));
		if (merge->confirmed.size() != participants.size())
			return;
		merge->stage = Merge::Draining;
		merge->started = timestamp();
		merge->confirmed.clear();
		packet.operation = MERGE_DRAIN;
		for (auto cid : participants)
			if (cid != merge->source && !sendSpaceLayout(cid, packet))
			{
				failSpaceLayout(packet.spaceID, transaction, "merge participant connection is unavailable", true);
				return;
			}
		if (!sendSpaceLayout(merge->source, packet))
			failSpaceLayout(packet.spaceID, transaction, "merge source connection is unavailable", true);
		break;
	case Merge::Draining:
		if (packet.operation != MERGE_DRAINED || sender != merge->source)
			return;
		merge->stage = Merge::Checking;
		merge->started = timestamp();
		packet.operation = MERGE_CHECK;
		for (auto cid : participants)
			if (cid != sender && !sendSpaceLayout(cid, packet))
			{
				failSpaceLayout(packet.spaceID, transaction, "merge participant connection is unavailable", true);
				return;
			}
		break;
	case Merge::Checking:
		if (packet.operation != MERGE_DRAINED || !member || sender == merge->source)
			return;
		merge->confirmed.insert(sender);
		if (merge->confirmed.size() + 1 != participants.size())
			return;
		// 迁出和目标收尾均已确认；在此之前故障恢复始终使用原 Layout。
		transaction.lastRetiredCellID = std::max(transaction.lastRetiredCellID,
			transaction.layout.cellForApp(merge->source)->cellID);
		transaction.layout = std::move(merge->nextLayout);
		merge->stage = Merge::Publishing;
		merge->started = timestamp();
		merge->confirmed.clear();
		packet.operation = MERGE_APPLY;
		packet.revision = ++transaction.boundaryRevision;
		{
			bool sent = true;
			for (auto cid : participants)
			{
				const bool delivered = sendSpaceLayout(cid, packet);
				// 退役 Cell 已无实体；它退出只影响回收确认，不影响存活 Cell 的归属。
				if (cid != merge->source)
					sent = delivered && sent;
			}
			if (!sent)
				failSpaceLayout(packet.spaceID, transaction, "could not publish the merged layout to all CellApps", true);
		}
		break;
	case Merge::Publishing:
		if (packet.operation == MERGE_APPLIED && member)
			merge->confirmed.insert(sender);
		else if (packet.operation == MERGE_REMOVED && sender == merge->source)
			merge->sourceRemoved = true;
		else
			return;
		finishSpaceMerge(packet.spaceID, transaction);
		break;
	case Merge::Cancelling:
		if (packet.operation != MERGE_CANCELLED || !member)
			return;
		if (sender == merge->source)
		{
			if (!merge->confirmed.insert(sender).second)
				return;
			// 退役 Cell 已停止发送快照，其他 Cell 仍按本次合并路由处理在途交接。
			packet.operation = MERGE_CANCEL;
			for (auto cid : participants)
				if (cid != sender && !sendSpaceLayout(cid, packet))
				{
					failSpaceLayout(packet.spaceID, transaction, "merge participant connection is unavailable", true);
					return;
				}
		}
		else if (merge->confirmed.count(merge->source))
			merge->confirmed.insert(sender);
		if (merge->confirmed.size() != participants.size())
			return;
		merge->stage = Merge::Resuming;
		packet.operation = MERGE_RESUME;
		if (!sendSpaceLayout(merge->source, packet))
			failSpaceLayout(packet.spaceID, transaction, "merge source connection is unavailable", true);
		break;
	case Merge::Resuming:
		if (packet.operation == MERGE_CANCELLED && sender == merge->source)
			transaction.merge.reset();
		break;
	}
}

void Cellappmgr::stopSpaceMerge(SPACE_ID spaceID, SpaceLayout::Transaction& transaction, const char* reason)
{
	using namespace SpaceLayout;
	if (!transaction.merge)
		return;
	// Space 故障或关闭时由 SUSPEND 或 REMOVE 清理排空状态。
	if (transaction.merge->stage == Transaction::Merge::Preparing)
	{
		Packet packet;
		packet.sender = componentID_;
		packet.operation = MERGE_CANCEL;
		packet.spaceID = spaceID;
		packet.version = transaction.version;
		packet.revision = transaction.boundaryRevision;
		packet.mergeAttempt = transaction.mergeAttempt;
		for (auto cid : transaction.layout.participants())
			sendSpaceLayout(cid, packet);
	}
	transaction.merge.reset();
	transaction.mergeError = reason;
	WARNING_MSG(fmt::format("Cellappmgr::spaceLayout: merge stopped, space={}, attempt={}: {}.\n",
		spaceID, transaction.mergeAttempt, reason));
}

void Cellappmgr::finishSpaceMerge(SPACE_ID spaceID, SpaceLayout::Transaction& transaction)
{
	if (!transaction.merge->sourceRemoved ||
		transaction.merge->confirmed.size() != transaction.layout.participants().size())
		return;
	transaction.merge.reset();
	INFO_MSG(fmt::format("Cellappmgr::spaceLayout: merge complete, space={}, attempt={}.\n",
		spaceID, transaction.mergeAttempt));
}

void Cellappmgr::onSpaceLayoutRequest(Network::Channel* channel, MemoryStream& stream)
{
	using namespace SpaceLayout;
	if (!spacePartitioningEnabled() || channel == NULL || channel->isExternal() ||
		channel->isDestroyed() || channel->condemn() != 0)
	{
		stream.done();
		return;
	}
	Components& components = Components::getSingleton();
	Components::ComponentInfos* source = components.findComponent(channel);
	if (source == NULL && channel->componentID() != 0)
		source = components.findComponent(CELLAPP_TYPE, channel->componentID());
	if (source == NULL || source->componentType != CELLAPP_TYPE ||
		!Security::isBoundBidirectionalComponentSource(source->cid, source, channel, channel->componentID()))
	{
		WARNING_MSG(fmt::format("Cellappmgr::onSpaceLayoutRequest: rejected unbound channel({}).\n", channel->c_str()));
		stream.done();
		return;
	}
	const COMPONENT_ID sender = source->cid;
	auto sourceApp = cellapps_.find(sender);
	if (sourceApp == cellapps_.end() || !sourceApp->second.spacePartitioningConfirmed())
	{
		stream.done();
		return;
	}
	Packet packet;
	const bool parsed = packet.read(stream);
	stream.done();
	if (!parsed || packet.sender != sender)
		return;

	auto found = spaceLayouts_.find(packet.spaceID);
	if (packet.operation == REPLACEMENT_CHECKED)
	{
		if (found == spaceLayouts_.end() || source->pChannel != channel)
			return;
		auto& transaction = found->second;
		auto& replacement = transaction.replacement;
		if (replacement && replacement->stage == Replacement::Checking &&
			packet.version == replacement->version && packet.revision == transaction.revision &&
			packet.replacedLayoutVersion == transaction.version && packet.replacedCellApp == replacement->lost &&
			packet.replacementCellApp == replacement->target && sender != replacement->lost &&
			transaction.layout.cellForApp(sender))
			replacement->checked.insert(sender);
		return;
	}
	if (found != spaceLayouts_.end() && found->second.replacement &&
		found->second.replacement->matches(sender, channel->sessionEpoch(), packet.version) &&
		source->pChannel == channel && packet.revision == found->second.revision)
	{
		auto& transaction = found->second;
		auto& replacement = *transaction.replacement;
		if (packet.operation == REMOVED)
		{
			// 只在临时 Space 已实际删除后释放分配记录。
			sourceApp->second.confirmSpaceCreation(packet.spaceID);
			INFO_MSG(fmt::format("Cellappmgr::cellRecovery: replacement removed, space={}, version={}, target={}.\n",
				packet.spaceID, replacement.version, sender));
			transaction.replacement.reset();
			transaction.replacementRetryAt = timestamp() + stampsPerSecond();
		}
		else if (packet.operation == REPLACEMENT_READY && replacement.stage == Replacement::Preparing &&
			transaction.state == FAILED && transaction.suspended && !transaction.shutdownRequested)
		{
			if (packet.success)
			{
				replacement.stage = Replacement::Ready;
				replacement.process = packet.replacementProcess;
				INFO_MSG(fmt::format("Cellappmgr::cellRecovery: replacement ready, space={}, oldVersion={}, "
					"version={}, lost={}, target={}, revision={}.\n", packet.spaceID, transaction.version,
					replacement.version, replacement.lost, replacement.target, transaction.revision));
			}
			else
			{
				transaction.rejectedReplacements[sender] = channel->sessionEpoch();
				cancelCellReplacement(packet.spaceID, transaction, packet.error.c_str());
			}
		}
		return;
	}
	if (packet.operation == REQUEST)
	{
		std::string error;
		Space* original = sourceApp->second.spaces().getSpace(packet.spaceID);
		if (packet.layout.owner != packet.sender || original == NULL ||
			original->getScriptModuleName() != packet.resources.scriptModule)
			error = "request must originate from the existing Space owner";
		else if (found != spaceLayouts_.end() &&
			(found->second.state != FAILED || found->second.suspended ||
			 packet.version <= found->second.version))
			error = "Space already has a layout or this request is outdated";
		else
		{
			for (COMPONENT_ID cid : packet.layout.participants())
			{
				auto app = cellapps_.find(cid);
				if (!componentReady(cid) || app == cellapps_.end() ||
					app->second.initProgress() <= 1.f ||
					(cid != packet.sender && app->second.hasAssignedSpace(packet.spaceID)))
				{
					error = "target CellApp is not ready or already holds this Space";
					break;
				}
			}
		}
		if (!error.empty())
		{
			Packet rejected;
			rejected.sender = componentID_;
			rejected.operation = ABORT;
			rejected.spaceID = packet.spaceID;
			rejected.version = packet.version;
			rejected.success = false;
			rejected.error = error;
			sendSpaceLayout(packet.sender, rejected);
			return;
		}
		Transaction transaction;
		transaction.layout = std::move(packet.layout);
		transaction.resources = std::move(packet.resources);
		transaction.version = packet.version;
		transaction.started = timestamp();
		spaceLayouts_[packet.spaceID] = std::move(transaction);
		broadcastSpaceLayout(packet.spaceID, spaceLayouts_[packet.spaceID], PREPARE);
		return;
	}

	if (packet.operation == BOUNDARY_REQUEST)
	{
		requestBoundaryMove(packet, found == spaceLayouts_.end() ? nullptr : &found->second);
		return;
	}
	if (packet.operation == SPLIT_READY || packet.operation == SPLIT_APPLIED)
	{
		if (found != spaceLayouts_.end() && source->pChannel == channel)
			handleSpaceSplitAck(packet, found->second, channel);
		return;
	}
	if (packet.operation == SPLIT_DISCARDED)
	{
		auto app = cancelledSplits_.find(sender);
		if (app == cancelledSplits_.end() || source->pChannel != channel)
			return;
		auto pending = app->second.find(packet.spaceID);
		if (pending != app->second.end() && pending->second.session == channel->sessionEpoch() &&
			pending->second.version == packet.version && pending->second.attempt == packet.splitAttempt)
		{
			sourceApp->second.confirmSpaceCreation(packet.spaceID);
			app->second.erase(pending);
			if (app->second.empty())
				cancelledSplits_.erase(app);
		}
		return;
	}
	if (packet.operation == SPLIT_DRAINED)
	{
		if (found == spaceLayouts_.end() || source->pChannel != channel)
			return;
		auto& transaction = found->second;
		if (!transaction.canConfirmSplitMigration(packet))
			return;
		using Migration = Transaction::SplitMigration;
		if (transaction.splitMigration == Migration::CheckingSource)
		{
			// 此时早于源端确认的交接均已安装或回退，目标只需等待已安装的交接收尾。
			packet.sender = componentID_;
			packet.operation = SPLIT_CHECK_TARGET;
			transaction.splitMigration = sendSpaceLayout(transaction.splitMigrationTarget, packet) ?
				Migration::CheckingTarget : Migration::Interrupted;
		}
		else
			transaction.splitMigration = Migration::Complete;
		return;
	}
	if (packet.operation == MERGE_READY || packet.operation == MERGE_DRAINED ||
		packet.operation == MERGE_APPLIED || packet.operation == MERGE_REMOVED ||
		packet.operation == MERGE_BLOCKED || packet.operation == MERGE_CANCELLED)
	{
		if (found != spaceLayouts_.end() && source->pChannel == channel)
			handleSpaceMergeAck(packet, found->second);
		return;
	}
	if (found == spaceLayouts_.end() || found->second.layout.cellForApp(packet.sender) == NULL)
		return;
	Transaction& transaction = found->second;
	if (transaction.version != packet.version &&
		(packet.operation != SHUTDOWN || !transaction.committedReplacement ||
		 transaction.committedReplacement->originalVersion != packet.version || transaction.layout.owner != sender))
		return;
	// 新请求超限不影响已提交的资源，只通知提交方，不增加资源版本。
	const auto rejectUpdate = [&](const char* error)
	{
		Packet rejected;
		rejected.sender = componentID_;
		rejected.operation = UPDATE_REJECTED;
		rejected.rejectedOperation = packet.operation;
		rejected.spaceID = packet.spaceID;
		rejected.version = transaction.version;
		rejected.revision = transaction.revision;
		rejected.key = packet.key;
		rejected.error = error;
		if (!sendSpaceLayout(sender, rejected))
			WARNING_MSG(fmt::format("Cellappmgr::spaceLayout: could not report rejected resource update "
				"to CellApp({}), space={}, version={}: {}.\n", sender, packet.spaceID, packet.version, error));
	};
	switch (packet.operation)
	{
	case BOUNDARY_APPLIED:
		if (source->pChannel == channel && transaction.confirmBoundary(sender, packet.version, packet.revision))
			finishBoundaryMove(packet.spaceID, transaction, nullptr);
		break;
	case READY: case COMMITTED:
	{
		const State expected = packet.operation == READY ? PREPARING : COMMITTING;
		if (transaction.state != expected || packet.revision != transaction.revision)
			return;
		if (!packet.success)
		{
			failSpaceLayout(packet.spaceID, transaction, packet.error, expected != PREPARING);
			return;
		}
		if (transaction.confirm(packet.sender, packet.version, expected))
		{
			transaction.advance(expected == PREPARING ? COMMITTING : ACTIVE, timestamp());
			broadcastSpaceLayout(packet.spaceID, transaction, expected == PREPARING ? COMMIT : COMPLETE);
			if (transaction.state == ACTIVE)
			{
				transaction.activated = true;
				INFO_MSG(fmt::format("Cellappmgr::spaceLayout: active space={}, version={}, cells={}.\n",
					packet.spaceID, transaction.version, transaction.layout.participants().size()));
			}
		}
		break;
	}
	case SHUTDOWN:
		if (packet.sender != transaction.layout.owner || transaction.state == RELEASING)
			return;
		transaction.shutdownRequested = true;
		stopSpaceSplit(packet.spaceID, transaction, "Space is closing");
		stopSpaceMerge(packet.spaceID, transaction, "Space is closing");
		transaction.interruptSplitMigration();
		if (transaction.committedReplacement)
			closedSpacesWithCellLoss_.insert(packet.spaceID);
		finishBoundaryMove(packet.spaceID, transaction, "Space closed before all boundary confirmations");
		cancelCellReplacement(packet.spaceID, transaction, "Space is closing");
		transaction.advance(RELEASING, timestamp());
		for (const auto cid : transaction.layout.participants())
		{
			const auto lost = lostPartitionCells_.find(cid);
			if (lost == lostPartitionCells_.end())
				continue;
			const auto affected = lost->second.layouts.find(packet.spaceID);
			if (affected == lost->second.layouts.end() || affected->second != transaction.version)
				continue;
			closedSpacesWithCellLoss_.insert(packet.spaceID);
			if (lost->second.exited)
				transaction.acknowledged.insert(cid);
		}
		broadcastSpaceLayout(packet.spaceID, transaction, REMOVE);
		break;
	case REMOVED:
	{
		const auto removal = transaction.recordRemoval(packet.sender, packet.version);
		if (removal == Transaction::Removal::Erase)
			spaceLayouts_.erase(found);
		else if (removal == Transaction::Removal::Fail)
			failSpaceLayout(packet.spaceID, transaction, "a participating Space was removed", true);
		break;
	}
	case SET_DATA: case DELETE_DATA:
	{
		if (transaction.state != ACTIVE)
			return;
		Resources next = transaction.resources;
		if (packet.operation == SET_DATA)
			next.data[packet.key] = packet.value;
		else
			next.data.erase(packet.key);
		// 先校验合并后的总大小，再接受修改，避免长期更新让完整资源超过初次创建的限制。
		if (!next.validate())
		{
			rejectUpdate("SpaceData exceeds the resource limits");
			return;
		}
		transaction.resources = std::move(next);
		Packet response = std::move(packet);
		response.sender = componentID_;
		response.revision = ++transaction.revision;
		sendSplitResource(response, transaction);
		for (COMPONENT_ID cid : transaction.layout.participants())
			if (!sendSpaceLayout(cid, response))
			{
				failSpaceLayout(response.spaceID, transaction, "SpaceData participant is unavailable", true);
				break;
			}
		break;
	}
	case RESOURCES:
	{
		if (transaction.state != ACTIVE || packet.sender != transaction.layout.owner ||
			packet.resources.scriptModule != transaction.resources.scriptModule)
			return;
		// 几何更新只替换映射信息，不能覆盖并发提交且已被管理器排序的其他 SpaceData。
		Resources next = transaction.resources;
		next.setGeometry(packet.resources.geometryPath, packet.resources.loadGeometry, packet.resources.geometryParams);
		if (!next.validate())
		{
			rejectUpdate("geometry update exceeds the resource limits");
			return;
		}
		transaction.resources = std::move(next);
		++transaction.revision;
		broadcastSpaceLayout(packet.spaceID, transaction, RESOURCES);
		break;
	}
	default:
		break;
	}
}

void Cellappmgr::updateSpaceLayouts()
{
	if (spaceLayouts_.empty() && lostPartitionCells_.empty() && disconnectedCellProcesses_.empty())
		return;
	const uint64 now = timestamp();
	if (now < nextSpaceLayoutCheck_)
		return;
	nextSpaceLayoutCheck_ = now + stampsPerSecond();
	checkLostPartitionCells();
	for (auto& entry : spaceLayouts_)
	{
		SpaceLayout::Transaction& transaction = entry.second;
		if ((transaction.state == SpaceLayout::PREPARING || transaction.state == SpaceLayout::COMMITTING) &&
			now - transaction.started > stampsPerSecond() * 30)
			failSpaceLayout(entry.first, transaction, "Cell preparation or confirmation timed out",
				transaction.state == SpaceLayout::COMMITTING);
		if (auto& move = transaction.boundaryConfirmation; move &&
			move->started && now - move->started > 30 * stampsPerSecond())
		{
			WARNING_MSG(fmt::format("Cellappmgr::spaceLayout: waiting for boundary confirmations, "
				"space={}, version={}, revision={}, confirmed={}.\n", entry.first, transaction.version,
				transaction.boundaryRevision, move->acknowledged.size()));
			move->started = 0;
		}
		if (transaction.replacement)
			updateCellReplacement(entry.first, transaction, now);
		if (auto& split = transaction.split; split && split->started &&
			now - split->started > 30 * stampsPerSecond())
		{
			if (!split->published)
				stopSpaceSplit(entry.first, transaction, "split preparation timed out");
			else
			{
				WARNING_MSG(fmt::format("Cellappmgr::spaceLayout: waiting for split confirmations, "
					"space={}, attempt={}, confirmed={}.\n", entry.first, transaction.splitAttempt, split->confirmed.size()));
				split->started = 0;
			}
		}
		if (auto& merge = transaction.merge; merge && merge->started &&
			now - merge->started > 30 * stampsPerSecond())
		{
			if (merge->stage == SpaceLayout::Transaction::Merge::Preparing)
				stopSpaceMerge(entry.first, transaction, "merge preparation timed out");
			else
			{
				WARNING_MSG(fmt::format("Cellappmgr::spaceLayout: waiting for merge confirmations, "
					"space={}, attempt={}, stage={}, confirmed={}.\n", entry.first, transaction.mergeAttempt,
					static_cast<int>(merge->stage), merge->confirmed.size()));
				merge->started = 0;
			}
		}
	}
	if (lostPartitionCells_.empty())
		return;

	// 限制每轮查找候选和传输完整资源的次数；没有候选的 Space 也不能占住后面的 Space。
	size_t attempts = 0;
	auto entry = spaceLayouts_.upper_bound(lastReplacementSpaceChecked_);
	for (size_t checked = 0; checked < spaceLayouts_.size() && attempts < 4; ++checked, ++entry)
	{
		if (entry == spaceLayouts_.end())
			entry = spaceLayouts_.begin();
		lastReplacementSpaceChecked_ = entry->first;
		if (!entry->second.replacement && updateCellReplacement(entry->first, entry->second, now))
			++attempts;
	}
}

bool Cellappmgr::updateCellReplacement(SPACE_ID spaceID, SpaceLayout::Transaction& transaction,
	uint64 now)
{
	using namespace SpaceLayout;
	if (!transaction.replacement && (!transaction.activated || transaction.committedReplacement ||
		transaction.state != FAILED || !transaction.suspended || transaction.shutdownRequested ||
		isShuttingdown() || now < transaction.replacementRetryAt))
		return false;

	auto& components = Components::getSingleton();
	if (transaction.replacement)
	{
		const auto& replacement = *transaction.replacement;
		const auto* channel = components.findComponentChannel(CELLAPP_TYPE, replacement.target);
		if (!channel || channel->isDestroyed() || channel->condemn() != 0 ||
			channel->sessionEpoch() != replacement.targetSession)
		{
			cancelCellReplacement(spaceID, transaction, "replacement connection was lost");
			if (auto app = cellapps_.find(replacement.target); app != cellapps_.end())
				app->second.confirmSpaceCreation(spaceID);
			transaction.replacement.reset();
			transaction.replacementRetryAt = now + stampsPerSecond();
			return false;
		}
	}
	COMPONENT_ID failed = 0;
	for (const auto id : transaction.layout.participants())
	{
		if (componentReady(id))
			continue;
		const auto lost = lostPartitionCells_.find(id);
		if (failed || lost == lostPartitionCells_.end() || !lost->second.exited || cellapps_.count(id))
		{
			cancelCellReplacement(spaceID, transaction, "another layout participant is unavailable");
			return !transaction.replacement;
		}
		const auto affected = lost->second.layouts.find(spaceID);
		if (affected == lost->second.layouts.end() || affected->second != transaction.version)
		{
			cancelCellReplacement(spaceID, transaction, "confirmed exit does not match the layout version");
			return !transaction.replacement;
		}
		failed = id;
	}

	if (transaction.replacement)
	{
		auto& replacement = *transaction.replacement;
		if (replacement.lost != failed || transaction.state != FAILED || transaction.shutdownRequested ||
			isShuttingdown())
			cancelCellReplacement(spaceID, transaction, "Space no longer needs this replacement");
		else if (replacement.stage == Replacement::Preparing && now - replacement.started > 30 * stampsPerSecond())
		{
			transaction.rejectedReplacements[replacement.target] = replacement.targetSession;
			cancelCellReplacement(spaceID, transaction, "replacement preparation timed out");
		}
		else if (replacement.stage == Replacement::Ready || replacement.stage == Replacement::Checking)
			checkCellReplacement(spaceID, transaction, now);
		return false;
	}
	if (!failed)
		return true;

	const uint64 previousVersion = std::max(transaction.version, transaction.lastReplacementVersion);
	if (previousVersion == std::numeric_limits<uint64>::max())
		return true;
	const auto target = findFreeCellapp(spaceID, &transaction);
	auto* channel = components.findComponentChannel(CELLAPP_TYPE, target);
	if (!channel || channel->isDestroyed() || channel->condemn() != 0)
		return true;

	Packet packet;
	packet.sender = componentID_;
	packet.operation = REPLACEMENT_PREPARE;
	packet.spaceID = spaceID;
	packet.version = previousVersion + 1;
	packet.revision = transaction.revision;
	packet.replacedCellApp = failed;
	packet.replacedLayoutVersion = transaction.version;
	packet.layout = transaction.layout;
	if (!packet.layout.replaceCellApp(failed, target))
		return true;
	packet.resources = transaction.resources;
	if (!sendSpaceLayout(target, packet))
		return true;
	transaction.lastReplacementVersion = packet.version;
	transaction.replacement = Replacement{failed, target, channel->sessionEpoch(), packet.version, now};
	cellapps_.at(target).reserveSpaceCreation(spaceID);
	INFO_MSG(fmt::format("Cellappmgr::cellRecovery: preparing replacement, space={}, oldVersion={}, "
		"version={}, lost={}, target={}, cell={}, revision={}.\n", spaceID, transaction.version,
		packet.version, failed, target, packet.layout.cellForApp(target)->cellID, packet.revision));
	return true;
}

void Cellappmgr::checkCellReplacement(SPACE_ID spaceID, SpaceLayout::Transaction& transaction, uint64 now)
{
	using namespace SpaceLayout;
	auto& replacement = *transaction.replacement;
	if (replacement.stage == Replacement::Ready)
	{
		replacement.stage = Replacement::Checking;
		replacement.started = now;
	}
	const auto previous = transaction.layout.participants();
	Packet packet;
	packet.sender = componentID_;
	packet.spaceID = spaceID;
	packet.version = replacement.version;
	packet.revision = transaction.revision;
	packet.replacedCellApp = replacement.lost;
	packet.replacedLayoutVersion = transaction.version;
	packet.replacementCellApp = replacement.target;
	if (replacement.checked.size() + 1 != previous.size())
	{
		if (now - replacement.started > 30 * stampsPerSecond())
		{
			cancelCellReplacement(spaceID, transaction, "participants did not finish pending transfers");
			return;
		}
		packet.operation = REPLACEMENT_CHECK;
		for (const auto cid : previous)
			if (cid != replacement.lost && !replacement.checked.count(cid))
				sendSpaceLayout(cid, packet);
		return;
	}

	auto layout = transaction.layout;
	if (!layout.replaceCellApp(replacement.lost, replacement.target))
		return;
	// 从这里起新版本是唯一布局。之后的失败只能暂停，不能取消成临时 Space 或退回旧版本。
	transaction.committedReplacement = Transaction::CommittedReplacement{replacement.lost,
		transaction.version, replacement.target, replacement.targetSession, replacement.process};
	transaction.layout = std::move(layout);
	transaction.version = replacement.version;
	transaction.boundaryRevision = 0;
	transaction.replacement.reset();
	transaction.advance(COMMITTING, now);
	transaction.suspended = false;
	transaction.error.clear();
	packet.operation = REPLACEMENT_COMMIT;
	bool sent = true;
	for (const auto cid : transaction.layout.participants())
		sent = sendSpaceLayout(cid, packet) && sent;
	if (!sent)
		failSpaceLayout(spaceID, transaction, "replacement commit connection is unavailable", true);
	INFO_MSG(fmt::format("Cellappmgr::cellRecovery: replacement committed, space={}, oldVersion={}, version={}, lost={}, target={}.\n",
		spaceID, packet.replacedLayoutVersion, packet.version, packet.replacedCellApp, packet.replacementCellApp));
}

void Cellappmgr::cancelCellReplacement(SPACE_ID spaceID, SpaceLayout::Transaction& transaction, const char* reason)
{
	using namespace SpaceLayout;
	if (!transaction.replacement || transaction.replacement->stage == Replacement::Cancelling)
		return;
	auto& replacement = *transaction.replacement;
	auto* channel = Components::getSingleton().findComponentChannel(CELLAPP_TYPE, replacement.target);
	Packet packet;
	packet.sender = componentID_;
	packet.operation = REPLACEMENT_CANCEL;
	packet.spaceID = spaceID;
	packet.version = replacement.version;
	packet.revision = transaction.revision;
	if (replacement.stage == Replacement::Checking)
	{
		for (const auto cid : transaction.layout.participants())
			if (cid != replacement.lost)
				sendSpaceLayout(cid, packet);
	}
	if (channel && channel->sessionEpoch() == replacement.targetSession)
		sendSpaceLayout(replacement.target, packet);
	replacement.stage = Replacement::Cancelling;
	INFO_MSG(fmt::format("Cellappmgr::cellRecovery: cancelling replacement, space={}, version={}, target={}: {}.\n",
		spaceID, replacement.version, replacement.target, reason));
}

uint64 Cellappmgr::preparedCellReplacements() const
{
	return std::count_if(spaceLayouts_.begin(), spaceLayouts_.end(), [](const auto& entry)
	{
		const auto& replacement = entry.second.replacement;
		return replacement && (replacement->stage == SpaceLayout::Replacement::Ready ||
			replacement->stage == SpaceLayout::Replacement::Checking);
	});
}

void Cellappmgr::watchPartitionCell(Network::Channel& channel, const Components::ComponentInfos& source,
	const CellRecovery::ProcessIdentity& identity, uint64 process)
{
	if (source.pChannel != &channel || channel.protocoltype() != Network::PROTOCOL_TCP ||
		partitionCellProcesses_.count(source.cid) || !channel.pEndPoint() || !source.pIntAddr ||
		!process || !identity.pid || (source.pid && source.pid != identity.pid))
		return;
	u_int32_t localIP = 0;
	if (channel.pEndPoint()->getlocaladdress(nullptr, &localIP) != 0 || !localIP ||
		localIP != channel.addr().ip || localIP != source.pIntAddr->ip)
		return;
	auto watch = CellRecovery::ProcessWatch::open(identity);
	if (!watch)
		return;
	disconnectedCellProcesses_.erase({source.cid, process});
	partitionCellProcesses_.emplace(source.cid, PartitionCellProcess{channel.sessionEpoch(), process, std::move(watch)});
	INFO_MSG(fmt::format("Cellappmgr::cellRecovery: monitoring CellApp process, cellapp={}, session={}, pid={}.\n",
		source.cid, channel.sessionEpoch(), identity.pid));
}

void Cellappmgr::onSpaceLayoutCellLost(COMPONENT_ID cid, uint64 session)
{
	LostPartitionCell lost;
	lost.session = session;
	for (auto& entry : spaceLayouts_)
	{
		SpaceLayout::Transaction& transaction = entry.second;
		if (transaction.split && !transaction.split->published && transaction.split->layout.cellForApp(cid))
			stopSpaceSplit(entry.first, transaction, "a split participant disconnected");
		if (!transaction.layout.cellForApp(cid))
		{
			if (transaction.merge && transaction.merge->stage == SpaceLayout::Transaction::Merge::Publishing &&
				transaction.merge->source == cid && !transaction.merge->sourceRemoved && !isShuttingdown())
				lost.layouts.emplace(entry.first, transaction.version);
			continue;
		}
		if (transaction.replacement && transaction.replacement->lost != cid)
			cancelCellReplacement(entry.first, transaction, "another layout participant disconnected");
		const bool releasing = transaction.state == SpaceLayout::RELEASING;
		// 保留存活 Cell 的删除确认。断线的 Cell 必须等进程退出后才能算作已删除。
		if (releasing)
			closedSpacesWithCellLoss_.insert(entry.first);
		if (!releasing && transaction.state != SpaceLayout::FAILED)
			failSpaceLayout(entry.first, transaction, "a participating CellApp disconnected",
				transaction.state != SpaceLayout::PREPARING);
		if ((releasing || transaction.suspended) && !isShuttingdown())
			lost.layouts.emplace(entry.first, transaction.version);
	}
	cancelledSplits_.erase(cid);
	const auto process = partitionCellProcesses_.find(cid);
	if (process != partitionCellProcesses_.end() && process->second.session == session)
	{
		lost.process = process->second.process;
		lost.watch = std::move(process->second.watch);
		partitionCellProcesses_.erase(process);
	}
	if (!lost.layouts.empty())
	{
		INFO_MSG(fmt::format("Cellappmgr::cellRecovery: waiting for process exit, cellapp={}, session={}, spaces={}, monitored={}.\n",
			cid, session, lost.layouts.size(), bool(lost.watch)));
		lostPartitionCells_.try_emplace(cid, std::move(lost));
	}
	else if (lost.watch)
		disconnectedCellProcesses_.try_emplace({cid, lost.process}, std::move(lost.watch));
}

void Cellappmgr::notifyCellProcessExit(COMPONENT_ID cid, uint64 process)
{
	auto notify = [&](COMPONENT_TYPE type, const Network::MessageHandler& message)
	{
		for (const auto& peer : Components::getSingleton().getComponents(type))
		{
			auto* channel = peer.pChannel;
			if (!channel || channel->isDestroyed() || channel->condemn() != 0)
				continue;
			auto* bundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
			bundle->newMessage(message);
			(*bundle) << cid << process;
			channel->send(bundle);
		}
	};
	notify(BASEAPP_TYPE, BaseappInterface::onCellProcessExit);
	notify(CELLAPP_TYPE, CellappInterface::onCellProcessExit);
}

void Cellappmgr::checkLostPartitionCells()
{
	// 传送目标可以只承载旧式 Space，仍要确认该 Cell 进程是否已退出。
	for (auto it = disconnectedCellProcesses_.begin(); it != disconnectedCellProcesses_.end();)
	{
		if (!it->second->exited())
		{
			++it;
			continue;
		}
		notifyCellProcessExit(it->first.first, it->first.second);
		it = disconnectedCellProcesses_.erase(it);
	}
	for (auto it = lostPartitionCells_.begin(); it != lostPartitionCells_.end();)
	{
		auto& lost = it->second;
		for (auto space = lost.layouts.begin(); space != lost.layouts.end();)
		{
			const auto current = spaceLayouts_.find(space->first);
			if (current == spaceLayouts_.end() || current->second.version != space->second ||
				(!current->second.layout.cellForApp(it->first) &&
				 (!current->second.merge || current->second.merge->source != it->first)))
				space = lost.layouts.erase(space);
			else
				++space;
		}
		if (lost.layouts.empty())
		{
			if (lost.watch)
				disconnectedCellProcesses_.try_emplace({it->first, lost.process}, std::move(lost.watch));
			it = lostPartitionCells_.erase(it);
			continue;
		}
		if (lost.watch && lost.watch->exited())
		{
			lost.exited = true;
			lost.watch.reset();
			notifyCellProcessExit(it->first, lost.process);
			INFO_MSG(fmt::format("Cellappmgr::cellRecovery: process exit confirmed, cellapp={}, session={}, spaces={}.\n",
				it->first, lost.session, lost.layouts.size()));
			for (const auto& [space, version] : lost.layouts)
			{
				const auto layout = spaceLayouts_.find(space);
				if (layout != spaceLayouts_.end())
				{
					auto& transaction = layout->second;
					if (transaction.version == version && transaction.merge &&
						transaction.merge->stage == SpaceLayout::Transaction::Merge::Publishing &&
						transaction.merge->source == it->first)
					{
						transaction.merge->sourceRemoved = true;
						finishSpaceMerge(space, transaction);
					}
				}
				if (layout != spaceLayouts_.end() && layout->second.state == SpaceLayout::RELEASING &&
					layout->second.recordRemoval(it->first, version) == SpaceLayout::Transaction::Removal::Erase)
				{
					spaceLayouts_.erase(layout);
					INFO_MSG(fmt::format("Cellappmgr::spaceLayout: removed space={}, version={} after CellApp {} exited.\n",
						space, version, it->first));
				}
			}
		}
		++it;
	}
}

uint64 Cellappmgr::lostPartitionCellsExited() const
{
	return std::count_if(lostPartitionCells_.begin(), lostPartitionCells_.end(),
		[](const auto& entry) { return entry.second.exited; });
}

uint64 Cellappmgr::lostPartitionCellsWaiting() const
{
	return lostPartitionCells_.size() - lostPartitionCellsExited();
}

void Cellappmgr::reqCellRecoveryScope(Network::Channel* channel, MemoryStream& stream)
{
	if (!spacePartitioningEnabled() || !channel || channel->isExternal() ||
		channel->isDestroyed() || channel->condemn() != 0)
	{
		stream.done();
		return;
	}
	auto& components = Components::getSingleton();
	auto* source = components.findComponent(channel);
	if (!source && channel->componentID())
		source = components.findComponent(BASEAPP_TYPE, channel->componentID());
	if (!source || source->componentType != BASEAPP_TYPE ||
		!Security::isBoundBidirectionalComponentSource(source->cid, source, channel, channel->componentID()))
	{
		stream.done();
		return;
	}

	BoundedStreamReader reader(stream);
	CellRecovery::Query query;
	const bool parsed = query.read(reader);
	stream.done();
	if (!parsed)
		return;

	// 失败和暂停的布局仍归管理器所有，不能因为缺少活动 Cell 就按旧式 Space 重建。
	const bool stillRegistered = cellapps_.count(query.lost) != 0;
	auto* bundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
	bundle->newMessage(BaseappInterface::onCellRecoveryScope);
	(*bundle) << query.lost << query.session << static_cast<uint16>(query.spaces.size());
	const auto lost = lostPartitionCells_.find(query.lost);
	for (const auto space : query.spaces)
	{
		const auto layout = spaceLayouts_.find(space);
		CellRecovery::ScopeResult result;
		result.space = space;
		if (layout == spaceLayouts_.end())
		{
			// Base 的查询可能晚于布局删除。保留 Space 编号，避免走旧式恢复。
			if (closedSpacesWithCellLoss_.count(space))
				result.scope = CellRecovery::Scope::REMOVED;
			else
				result.scope = stillRegistered ? CellRecovery::Scope::UNKNOWN : CellRecovery::Scope::LEGACY;
		}
		else
		{
			const auto& transaction = layout->second;
			result.scope = CellRecovery::Scope::MANAGED;
			// 正在关闭的布局不能再恢复实体。
			const auto& committed = transaction.committedReplacement;
			if (transaction.shutdownRequested)
				result.scope = CellRecovery::Scope::RETIRED;
			else if (committed && committed->lost == query.lost && !stillRegistered)
			{
				result.scope = CellRecovery::Scope::CONFIRMED_EXIT;
				result.layoutVersion = committed->originalVersion;
				const auto targetLoss = lostPartitionCells_.find(committed->target);
				if (targetLoss != lostPartitionCells_.end() && targetLoss->second.exited &&
					targetLoss->second.session == committed->targetSession)
				{
					const auto affected = targetLoss->second.layouts.find(space);
					result.replacementExited = affected != targetLoss->second.layouts.end() &&
						affected->second == transaction.version;
				}
				const auto* target = components.findComponentChannel(CELLAPP_TYPE, committed->target);
				if (!isShuttingdown() && transaction.state == SpaceLayout::ACTIVE && target &&
					!target->isDestroyed() && target->condemn() == 0 &&
					target->sessionEpoch() == committed->targetSession)
				{
					result.replacement = committed->target;
					result.replacementVersion = transaction.version;
					result.replacementProcess = committed->process;
					result.committed = true;
				}
			}
			else if (!transaction.layout.cellForApp(query.lost))
				result.scope = CellRecovery::Scope::RETIRED;
			else if (transaction.state == SpaceLayout::FAILED && transaction.suspended &&
				lost != lostPartitionCells_.end() && lost->second.exited && !stillRegistered)
			{
				const auto affected = lost->second.layouts.find(space);
				if (affected != lost->second.layouts.end() && affected->second == transaction.version)
				{
					result.scope = CellRecovery::Scope::CONFIRMED_EXIT;
					result.layoutVersion = affected->second;
					const auto& replacement = transaction.replacement;
					if (!isShuttingdown() && replacement && replacement->lost == query.lost &&
						(replacement->stage == SpaceLayout::Replacement::Ready ||
						 replacement->stage == SpaceLayout::Replacement::Checking))
					{
						const auto* target = components.findComponentChannel(CELLAPP_TYPE, replacement->target);
						if (target && !target->isDestroyed() && target->condemn() == 0 &&
							target->sessionEpoch() == replacement->targetSession)
						{
							result.replacement = replacement->target;
							result.replacementVersion = replacement->version;
						}
					}
				}
			}
		}
		result.write(*bundle);
	}
	channel->send(bundle);
}

}
