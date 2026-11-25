#include "cellappmgr.h"
#include "cellappmgr_interface.h"
#include "../cellapp/cellapp_interface.h"
#include "../baseapp/baseapp_interface.h"
#include "server/cell_recovery.h"
#include "server/component_routing_guard.h"

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
	Network::Bundle* bundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
	bundle->newMessage(CellappInterface::onSpaceLayout);
	bundle->append(&stream);
	target->pChannel->send(bundle);
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
	if (operation == SpaceLayout::COMPLETE)
	{
		// CellApp 间的连接可能早于布局断开，激活时以管理器现有的注册地址为准。
		auto& components = Components::getSingleton();
		for (COMPONENT_ID cid : participants)
		{
			const auto* component = components.findComponent(CELLAPP_TYPE, cid);
			if (!component || !component->pIntAddr || !component->pExtAddr ||
				component->pIntAddr->ip == 0 || component->pIntAddr->port == 0)
			{
				failSpaceLayout(spaceID, transaction, "participant endpoint is unavailable", true);
				return;
			}
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
	}
	bool sent = true;
	for (COMPONENT_ID cid : participants)
		sent = sendSpaceLayout(cid, packet) && sent;
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
	transaction.advance(SpaceLayout::FAILED, timestamp());
	transaction.suspended = suspend;
	transaction.error = error;
	broadcastSpaceLayout(spaceID, transaction, suspend ? SpaceLayout::SUSPEND : SpaceLayout::ABORT);
	WARNING_MSG(fmt::format("Cellappmgr::spaceLayout: space={}, version={}, failed={}.\n",
		spaceID, transaction.version, error));
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
		if (transaction.committedReplacement)
			closedSpacesWithCellLoss_.insert(packet.spaceID);
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
		if (transaction.replacement)
			updateCellReplacement(entry.first, transaction, now);
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
		if (!transaction.layout.cellForApp(cid))
			continue;
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
			if (current == spaceLayouts_.end() || current->second.version != space->second)
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
