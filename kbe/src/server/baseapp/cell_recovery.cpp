#include "baseapp.h"
#include "cellapp/cellapp_interface.h"
#include "cellappmgr/cellappmgr_interface.h"
#include "client_lib/client_interface.h"
#include "proxy.h"
#include "restore_entity_handler.h"
#include "server/cell_recovery.h"
#include "server/component_routing_guard.h"

namespace KBEngine
{
namespace
{
void sendRecoveryAction(Network::Channel& target, const CellRecovery::StageKey& key,
	uint64 targetProcess, CellRecovery::RestoreAction action, bool flag = false)
{
	auto* bundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
	bundle->newMessage(CellappInterface::reqStageRecoveryCellBackup);
	key.write(*bundle);
	CellRecovery::RecoveryProcesses{CellRecovery::recoveryProcessID(), targetProcess}.write(*bundle);
	(*bundle) << static_cast<uint8>(action) << static_cast<uint8>(flag);
	target.send(bundle);
}
}

void Baseapp::onCellProcessExit(Network::Channel* channel, COMPONENT_ID cid, uint64 process)
{
	if (!channel || channel->isExternal() || channel->isDestroyed() || channel->condemn() != 0 ||
		channel != Components::getSingleton().getCellappmgrChannel() || !cid || !process || isShuttingdown())
		return;

	const auto now = timestamp();
	for (auto it = recentCellProcessExits_.begin(); it != recentCellProcessExits_.end();)
	{
		if (it->second <= now)
			it = recentCellProcessExits_.erase(it);
		else
			++it;
	}
	// 保留到本轮传送登记和下一次重试都能看到退出确认。
	recentCellProcessExits_[{cid, process}] = now + 30 * stampsPerSecond();
}

bool Baseapp::hasConfirmedCellProcessExit(COMPONENT_ID cid, uint64 process) const
{
	const auto exit = recentCellProcessExits_.find({cid, process});
	return exit != recentCellProcessExits_.end() && exit->second > timestamp();
}

void Baseapp::deferCellLoss(Network::Channel& channel)
{
	const COMPONENT_ID cid = channel.componentID();
	if (!cid)
		return;

	if (std::any_of(cellRecoveryStaging_.begin(), cellRecoveryStaging_.end(),
			[cid](const auto& entry) { return entry.second.committed && entry.second.target == cid; }) ||
		std::any_of(recoveryActivations_.begin(), recoveryActivations_.end(),
			[cid](const auto& entry) { return entry.second.target == cid; }))
	{
		const auto* peer = Components::getSingleton().findComponent(&channel);
		if (peer && peer->pIntAddr && peer->pExtAddr)
		{
			auto& saved = recoveryPeers_[cid];
			saved = *peer;
			saved.pChannel = nullptr;
		}
	}

	CellLoss loss;
	loss.session = channel.sessionEpoch();
	auto* manager = Components::getSingleton().getCellappmgrChannel();
	if (manager && !manager->isDestroyed() && manager->condemn() == 0)
		loss.managerSession = manager->sessionEpoch();
	for (const auto& entry : pEntities_->getEntities())
	{
		const auto* entity = static_cast<const Entity*>(entry.second.get());
		const auto* cell = entity->cellEntityCall();
		if (!cell || !entity->spaceID())
			continue;
		if (cell->componentID() == cid)
		{
			auto& group = loss.pending[entity->spaceID()];
			group.routed.push_back(entity->id());
			group.managed = group.managed || recoveryActivations_.count(entity->id()) != 0;
		}
		// 路由可能已切到存活目标，也可能仍指向存活源，但两种情况都需要核对交接。
		if (entity->hasCellHandoffWith(cid))
			loss.pending[entity->spaceID()].handoffs.push_back(entity->id());
	}
	for (const auto& [id, activation] : recoveryActivations_)
	{
		if (activation.target != cid)
			continue;
		const auto* entity = findEntity(id);
		if (!entity || !entity->cellEntityCall() || entity->cellEntityCall()->componentID() != cid ||
			entity->spaceID() != activation.key.space)
		{
			auto& group = loss.pending[activation.key.space];
			group.managed = true;
			group.routed.push_back(id);
		}
	}
	if (loss.pending.empty())
		return;

	// Base 的断线通知可能先于管理器，也可能发生在路由切换途中。先保留原引用。
	INFO_MSG(fmt::format("Baseapp::cellRecovery: waiting for scope, cellapp={}, session={}, spaces={}.\n",
		cid, loss.session, loss.pending.size()));
	cellLosses_[cid] = std::move(loss);
}

void Baseapp::queryCellRecoveryScopes()
{
	nextCellLossQuery_ = timestamp() + stampsPerSecond();
	auto& components = Components::getSingleton();
	auto* manager = components.getCellappmgrChannel();
	if (!manager || manager->isDestroyed() || manager->condemn() != 0)
		return;

	auto it = cellLosses_.upper_bound(lastQueriedCellLoss_);
	if (it == cellLosses_.end())
		it = cellLosses_.begin();
	lastQueriedCellLoss_ = it->first;
	auto& loss = it->second;
	if (!loss.managerSession || loss.managerSession != manager->sessionEpoch())
	{
		// 管理器重连后不能确认原布局仍在，不能把原来的分区当成旧式 Space。
		return;
	}

	auto group = loss.pending.upper_bound(loss.lastQueried);
	if (group == loss.pending.end())
		group = loss.pending.begin();
	std::vector<SPACE_ID> spaces;
	for (; group != loss.pending.end() && spaces.size() < CellRecovery::MAX_SPACES_PER_QUERY; ++group)
		spaces.push_back(group->first);
	loss.lastQueried = spaces.back();

	auto* bundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
	bundle->newMessage(CellappmgrInterface::reqCellRecoveryScope);
	(*bundle) << it->first << loss.session << static_cast<uint16>(spaces.size());
	for (const auto space : spaces)
		(*bundle) << space;
	manager->send(bundle);
}

void Baseapp::onCellRecoveryScope(Network::Channel* channel, MemoryStream& stream)
{
	auto& components = Components::getSingleton();
	if (!channel || channel->isExternal() || channel->isDestroyed() || channel->condemn() != 0 ||
		channel != components.getCellappmgrChannel())
	{
		stream.done();
		return;
	}

	BoundedStreamReader reader(stream);
	CellRecovery::Reply reply;
	const bool parsed = reply.read(reader);
	stream.done();
	if (!parsed || isShuttingdown())
		return;

	using CellRecovery::Scope;
	for (const auto& result : reply.scopes)
	{
		const auto entry = cellRecoveryStaging_.find({reply.lost, result.space});
		if (entry == cellRecoveryStaging_.end())
			continue;
		auto& staging = entry->second;
		if (staging.loss.cellSession != reply.session || staging.loss.managerSession != channel->sessionEpoch())
			continue;
		// 原管理器已删除布局，目标 Space 已删除或进程已退出，无需继续等待清理回执。
		if (result.scope == Scope::REMOVED)
		{
			cellRecoveryStaging_.erase(entry);
			continue;
		}
		staging.mayInstall = result.committed;
		if (result.replacementExited && staging.committed && staging.loss.layoutVersion == result.layoutVersion)
		{
			forgetRecoveryPeer(staging.target);
			cellRecoveryStaging_.erase(entry);
			continue;
		}
		if (result.scope != Scope::CONFIRMED_EXIT || staging.loss.layoutVersion != result.layoutVersion)
		{
			if (staging.committed && !staging.awaiting.empty())
				staging.retired = true;
			else
				cellRecoveryStaging_.erase(entry);
			continue;
		}
		if (result.replacement == reply.lost)
			continue;
		if (staging.committed && !result.committed)
			continue;
		if (staging.target != result.replacement || staging.version != result.replacementVersion ||
			staging.committed != result.committed)
		{
			staging.target = result.replacement;
			staging.version = result.replacementVersion;
			staging.committed = result.committed;
			staging.targetProcess = result.replacementProcess;
			staging.resetProgress(0);
		}
	}

	auto waiting = cellLosses_.find(reply.lost);
	if (waiting == cellLosses_.end())
		return;
	auto& loss = waiting->second;
	if (loss.session != reply.session || loss.managerSession != channel->sessionEpoch())
		return;

	for (const auto& result : reply.scopes)
	{
		const auto space = result.space;
		const auto scope = result.scope;
		const auto group = loss.pending.find(space);
		if (group == loss.pending.end() || scope == Scope::UNKNOWN)
			continue;
		auto& candidates = group->second;
		const auto hasPendingActivation = [&]()
		{
			return std::any_of(candidates.routed.begin(), candidates.routed.end(), [&](ENTITY_ID id)
			{
				const auto pending = recoveryActivations_.find(id);
				return pending != recoveryActivations_.end() && pending->second.target == reply.lost &&
					pending->second.key.space == space && pending->second.managerSession == loss.managerSession;
			});
		};
		if (scope == Scope::MANAGED || scope == Scope::RETIRED || scope == Scope::CONFIRMED_EXIT)
		{
			if (!candidates.managed)
			{
				candidates.managed = true;
				++heldCellRecoveryScopes_;
				INFO_MSG(fmt::format("Baseapp::cellRecovery: managed Space held, space={}, cellapp={}, entities={}.\n",
					space, reply.lost, candidates.routed.size()));
			}
			if (scope == Scope::CONFIRMED_EXIT)
			{
				if (!candidates.handoffs.empty() || !candidates.routed.empty())
					confirmedCellLosses_.push_back({reply.lost, loss.session, loss.managerSession, space, result.layoutVersion,
						std::move(candidates.handoffs), std::move(candidates.routed)});
			}
			else if (scope == Scope::MANAGED && !components.findComponentChannel(CELLAPP_TYPE, reply.lost))
				continue;
			else if (scope == Scope::RETIRED && hasPendingActivation() &&
				!components.findComponentChannel(CELLAPP_TYPE, reply.lost))
				continue;
			// 旧 Cell 已重新连接时丢弃本轮断线记录；下一次断线会重新检查路由。
		}
		else if (scope == Scope::REMOVED)
		{
			// 删完的布局已没有版本可查，只清理已登记的激活等待，不再恢复实体。
			INFO_MSG(fmt::format("Baseapp::cellRecovery: Space already removed, space={}, cellapp={}.\n",
				space, reply.lost));
			if (hasPendingActivation())
				confirmedCellLosses_.push_back({reply.lost, loss.session, loss.managerSession, space, 0,
					{}, std::move(candidates.routed)});
		}
		else if (scope == Scope::LEGACY && !candidates.managed && candidates.handoffs.empty())
		{
			for (const auto id : candidates.routed)
				loss.legacy.emplace_back(space, id);
		}
		loss.pending.erase(group);
	}
	// 已确定属于分区的交接仍可等待退出确认，但不应阻塞同一进程上的旧式 Space。
	if (!loss.pending.empty() && (loss.legacy.empty() ||
		std::any_of(loss.pending.begin(), loss.pending.end(), [](const auto& entry) { return !entry.second.managed; })))
		return;

	std::vector<ENTITY_ID> legacy;
	for (const auto& [space, id] : loss.legacy)
	{
		const auto* entity = findEntity(id);
		if (entity && entity->spaceID() == space)
			legacy.push_back(id);
	}
	loss.legacy.clear();
	if (loss.pending.empty())
		cellLosses_.erase(waiting);
	if (!components.findComponentChannel(CELLAPP_TYPE, reply.lost))
		restoreLegacyCellEntities(reply.lost, legacy, true);
}

void Baseapp::processConfirmedCellLosses()
{
	auto& components = Components::getSingleton();
	auto* manager = components.getCellappmgrChannel();
	if (!manager || manager->isDestroyed() || manager->condemn() != 0)
	{
		confirmedCellLosses_.clear();
		return;
	}

	// 每 Tick 只核对有限数量，不遍历存活 Cell 上的实体。
	constexpr size_t maxPerTick = 64;
	for (size_t i = 0; i < maxPerTick && !confirmedCellLosses_.empty(); ++i)
	{
		auto& batch = confirmedCellLosses_.front();
		const auto* cell = components.findComponentChannel(CELLAPP_TYPE, batch.lost);
		if (batch.managerSession != manager->sessionEpoch() ||
			(cell && !cell->isDestroyed() && cell->condemn() == 0))
		{
			confirmedCellLosses_.pop_front();
			continue;
		}
		const bool isHandoff = !batch.handoffs.empty();
		auto& ids = isHandoff ? batch.handoffs : batch.routed;
		const CellRecovery::LostReal loss{batch.lost, batch.session, batch.space, batch.version, batch.managerSession};
		SmartPointer<Entity> entity(findEntity(ids.back()));
		const auto activation = recoveryActivations_.find(ids.back());
		if (activation != recoveryActivations_.end() && activation->second.target == batch.lost &&
			activation->second.key.space == batch.space &&
			(!batch.version || activation->second.key.replacementVersion == batch.version) &&
			activation->second.managerSession == batch.managerSession)
		{
			if (entity)
				entity->cancelPartitionRestore();
			recoveryActivations_.erase(activation);
		}
		if (entity && batch.version)
		{
			if (isHandoff)
				entity->onPartitionCellExit(loss);
			if (recoverPartitionReal(*entity, loss))
				++batch.lostReals;
		}
		ids.pop_back();
		if (batch.handoffs.empty() && batch.routed.empty())
		{
			INFO_MSG(fmt::format("Baseapp::cellRecovery: checked lost Reals, cellapp={}, space={}, version={}, lost={}.\n",
				batch.lost, batch.space, batch.version, batch.lostReals));
			confirmedCellLosses_.pop_front();
		}
	}
}

bool Baseapp::recoverPartitionReal(Entity& entity, const CellRecovery::LostReal& loss)
{
	if (!entity.markPartitionRealLost(loss))
		return false;

	++confirmedRealLosses_;
	if (entity.partitionRecoveryCellData(loss))
	{
		++confirmedLossesWithBackup_;
		auto& staging = cellRecoveryStaging_[{loss.cellapp, loss.space}];
		staging.loss = loss;
		staging.entities.push_back(entity.id());
	}
	else
	{
		INFO_MSG(fmt::format("Baseapp::cellRecovery: no usable backup, entity={}, space={}, cellapp={}.\n",
			entity.id(), loss.space, loss.cellapp));
		entity.loseCell();
	}
	return true;
}

void Baseapp::processCellRecoveryStaging()
{
	auto entry = cellRecoveryStaging_.upper_bound(lastStagingGroup_);
	if (entry == cellRecoveryStaging_.end())
		entry = cellRecoveryStaging_.begin();
	lastStagingGroup_ = entry->first;
	auto& staging = entry->second;
	const auto& loss = staging.loss;
	auto& components = Components::getSingleton();
	auto* manager = components.getCellappmgrChannel();
	const bool managerMatches = manager && !manager->isDestroyed() && manager->condemn() == 0 &&
		manager->sessionEpoch() == loss.managerSession;
	if ((manager && !managerMatches) || isShuttingdown())
		staging.retired = true;

	const auto now = timestamp();
	if (staging.retired && !staging.committed)
	{
		cellRecoveryStaging_.erase(entry);
		return;
	}
	if (staging.retired && staging.awaiting.empty())
	{
		cellRecoveryStaging_.erase(entry);
		return;
	}
	if (managerMatches && now >= staging.nextQuery)
	{
		// READY 可以晚于丢失确认，也可能因替代节点变化而撤销。只向原管理器查询。
		staging.nextQuery = now + stampsPerSecond();
		auto* bundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
		bundle->newMessage(CellappmgrInterface::reqCellRecoveryScope);
		(*bundle) << loss.cellapp << loss.cellSession << uint16(1) << loss.space;
		manager->send(bundle);
	}

	auto* target = staging.target ? components.findComponentChannel(CELLAPP_TYPE, staging.target) : nullptr;
	if (!target || target->isDestroyed() || target->condemn() != 0)
		return;
	if (staging.targetSession != target->sessionEpoch())
	{
		// 同一恢复目标的新连接只续发未确认项，不重新安装已完成的实体。
		staging.targetSession = target->sessionEpoch();
		for (auto& pending : staging.awaiting)
			pending.second.retryAt = 0;
	}

	using CellRecovery::RestoreAction;
	// 每 Tick 只处理一组，最多检查八个实体。单份大备份独占本 Tick 的发送量。
	constexpr size_t maxPerTick = 8;
	constexpr size_t maxBytesPerTick = 1024 * 1024;
	size_t remaining = maxPerTick;
	size_t bytes = 0;
	const auto send = [&](Entity& entity, const std::vector<uint8>& backup)
	{
		if (bytes && backup.size() > maxBytesPerTick - bytes)
			return false;
		auto* bundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
		bundle->newMessage(CellappInterface::reqStageRecoveryCellBackup);
		CellRecovery::StageKey{loss.cellapp, loss.space, loss.layoutVersion, staging.version, entity.id()}.write(*bundle);
		CellRecovery::RecoveryProcesses{CellRecovery::recoveryProcessID(), staging.targetProcess}.write(*bundle);
		(*bundle) << static_cast<uint8>(staging.committed ? RestoreAction::Install : RestoreAction::Stage)
			<< static_cast<uint8>(entity.isCreatedSpace()) << entity.pScriptModule()->getUType()
			<< entity.recoveryRoutingEpoch();
		bundle->appendBlob(backup.data(), backup.size());
		target->send(bundle);
		bytes += backup.size();
		return true;
	};

	for (auto pending = staging.awaiting.begin(); pending != staging.awaiting.end() && remaining &&
		bytes < maxBytesPerTick;)
	{
		if (pending->second.retryAt > now)
		{
			++pending;
			continue;
		}
		--remaining;
		auto* entity = findEntity(pending->first);
		const auto* backup = entity ? entity->partitionRecoveryCellData(loss) : nullptr;
		if (pending->second.discarding || staging.retired || !backup)
		{
			if (staging.committed)
			{
				pending->second = {now + 3 * stampsPerSecond(), true};
				sendRecoveryAction(*target, {loss.cellapp, loss.space, loss.layoutVersion, staging.version, pending->first},
					staging.targetProcess, RestoreAction::Discard);
				++pending;
			}
			else
				pending = staging.awaiting.erase(pending);
		}
		else if (managerMatches && (!staging.committed || staging.mayInstall) && send(*entity, *backup))
		{
			pending->second.retryAt = now + 3 * stampsPerSecond();
			++pending;
		}
		else
			break;
	}
	while (managerMatches && !staging.retired && (!staging.committed || staging.mayInstall) &&
		remaining && bytes < maxBytesPerTick && staging.awaiting.size() < maxPerTick &&
		staging.nextEntity < staging.entities.size())
	{
		--remaining;
		const auto id = staging.entities[staging.nextEntity];
		auto* entity = findEntity(id);
		const auto* backup = entity ? entity->partitionRecoveryCellData(loss) : nullptr;
		if (backup)
		{
			if (!send(*entity, *backup))
				break;
			staging.awaiting.emplace(id, CellRecoveryStaging::Pending{now + 3 * stampsPerSecond()});
		}
		++staging.nextEntity;
	}
	if (staging.committed && staging.nextEntity == staging.entities.size() && staging.awaiting.empty())
		cellRecoveryStaging_.erase(entry);
}

void Baseapp::onStageRecoveryCellBackup(Network::Channel* channel, MemoryStream& stream)
{
	using CellRecovery::RestoreAction;
	BoundedStreamReader reader(stream);
	CellRecovery::StageKey key;
	CellRecovery::RecoveryProcesses processes;
	uint8 action = 0;
	uint8 accepted = 0;
	const bool parsed = key.read(reader) && processes.read(reader) && reader.read(action) &&
		action <= static_cast<uint8>(RestoreAction::Discard) && reader.read(accepted) && accepted <= 1 && reader.empty();
	stream.done();
	auto& components = Components::getSingleton();
	if (!parsed || !channel || channel->isExternal() || channel->isDestroyed() ||
		channel->condemn() != 0 || !components.isExpectedComponentChannel(CELLAPP_TYPE, channel) ||
		processes.base != CellRecovery::recoveryProcessID())
		return;
	const auto operation = static_cast<RestoreAction>(action);
	if (operation == RestoreAction::Activate || operation == RestoreAction::Discard)
	{
		const auto pending = recoveryActivations_.find(key.entity);
		if (pending != recoveryActivations_.end() && pending->second.key == key &&
			pending->second.targetProcess == processes.target && pending->second.target == channel->componentID() &&
			pending->second.session == channel->sessionEpoch())
		{
			if (pending->second.discarding != (operation == RestoreAction::Discard))
				return;
			recoveryActivations_.erase(pending);
			if (operation == RestoreAction::Discard)
				return;
			SmartPointer<Entity> entity(findEntity(key.entity));
			if (!entity || entity->spaceID() != key.space || !entity->cellEntityCall() ||
				entity->cellEntityCall()->componentID() != channel->componentID())
				return;
			if (accepted)
			{
				entity->finishPartitionRestore();
				INFO_MSG(fmt::format("Baseapp::cellRecovery: restored entity={}, space={}, version={}, target={}.\n",
					key.entity, key.space, key.replacementVersion, channel->componentID()));
			}
			else
			{
				// 同一个替代进程已明确拒绝激活，不能留下指向未安装实体的 Cell 引用。
				entity->cancelPartitionRestore();
				entity->onLoseCell(channel, stream);
			}
			return;
		}
		if (operation == RestoreAction::Activate)
			return;
	}

	const auto entry = cellRecoveryStaging_.find({key.lost, key.space});
	if (entry == cellRecoveryStaging_.end())
		return;
	auto& staging = entry->second;
	if ((operation != RestoreAction::Stage) != staging.committed || key.originalVersion != staging.loss.layoutVersion ||
		key.replacementVersion != staging.version || channel->componentID() != staging.target ||
		channel->sessionEpoch() != staging.targetSession || processes.target != staging.targetProcess)
		return;
	const auto pending = staging.awaiting.find(key.entity);
	if (pending == staging.awaiting.end() || pending->second.discarding != (operation == RestoreAction::Discard))
		return;
	if (operation == RestoreAction::Discard)
	{
		if (accepted)
		{
			staging.awaiting.erase(pending);
			INFO_MSG(fmt::format("Baseapp::cellRecovery: discarded installation, entity={}, space={}, version={}, target={}.\n",
				key.entity, key.space, key.replacementVersion, staging.target));
		}
		return;
	}

	auto* entity = findEntity(key.entity);
	if (operation == RestoreAction::Install && accepted)
	{
		const auto* manager = components.getCellappmgrChannel();
		if (isShuttingdown() || staging.retired || !staging.mayInstall || !manager ||
			manager->isDestroyed() || manager->condemn() != 0 || manager->sessionEpoch() != staging.loss.managerSession ||
			!entity || !entity->routeRecoveredCell(staging.loss, staging.target))
		{
			pending->second = {timestamp() + 3 * stampsPerSecond(), true};
			sendRecoveryAction(*channel, key, staging.targetProcess, RestoreAction::Discard);
			return;
		}
		staging.awaiting.erase(pending);
		const bool hasClient = entity->pScriptModule()->hasClient() && static_cast<Proxy*>(entity)->hasClient();
		if (hasClient)
		{
			// 旧 Cell 已退出，客户端需要先清掉旧的视野和场景数据。
			auto* bundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
			bundle->newMessage(ClientInterface::onEntityLeaveSpace);
			(*bundle) << key.entity;
			static_cast<Proxy*>(entity)->sendToClient(ClientInterface::onEntityLeaveSpace, bundle);
		}
		recoveryActivations_.emplace(key.entity, RecoveryActivation{key, staging.target, staging.targetProcess,
			staging.targetSession, staging.loss.managerSession, timestamp() + 3 * stampsPerSecond()});
		sendRecoveryAction(*channel, key, staging.targetProcess, RestoreAction::Activate, hasClient);
		return;
	}
	staging.awaiting.erase(pending);
	if (!entity || !entity->partitionRecoveryCellData(staging.loss))
		return;
	if (accepted)
	{
		++stagedCellRecoveryReceipts_;
		INFO_MSG(fmt::format("Baseapp::cellRecovery: backup staged, entity={}, space={}, version={}, target={}.\n",
			key.entity, key.space, key.replacementVersion, staging.target));
	}
	else
	{
		WARNING_MSG(fmt::format("Baseapp::cellRecovery: restore step rejected, action={}, entity={}, space={}, version={}, target={}.\n",
			action, key.entity, key.space, key.replacementVersion, staging.target));
		// 预存失败还可以正式安装。当前替代 Cell 明确拒绝安装时，旧路由已无法使用。
		if (operation == RestoreAction::Install)
			entity->loseCell();
	}
}

void Baseapp::processRecoveryActivations()
{
	auto& components = Components::getSingleton();
	const auto now = timestamp();
	auto pending = recoveryActivations_.upper_bound(lastRecoveryActivation_);
	const auto count = std::min<size_t>(8, recoveryActivations_.size());
	for (size_t i = 0; i < count; ++i)
	{
		if (pending == recoveryActivations_.end())
			pending = recoveryActivations_.begin();
		auto current = pending++;
		lastRecoveryActivation_ = current->first;
		auto& activation = current->second;
		if (now < activation.retryAt)
			continue;
		auto* channel = components.findComponentChannel(CELLAPP_TYPE, activation.target);
		activation.retryAt = now + 3 * stampsPerSecond();
		if (!channel || channel->isDestroyed() || channel->condemn() != 0)
			continue;
		activation.session = channel->sessionEpoch();
		auto* entity = findEntity(current->first);
		if (!entity || entity->spaceID() != activation.key.space ||
			!entity->cellEntityCall() || entity->cellEntityCall()->componentID() != activation.target)
		{
			activation.discarding = true;
			sendRecoveryAction(*channel, activation.key, activation.targetProcess, CellRecovery::RestoreAction::Discard);
			continue;
		}
		const bool hasClient = entity->pScriptModule()->hasClient() && static_cast<Proxy*>(entity)->hasClient();
		sendRecoveryAction(*channel, activation.key, activation.targetProcess, CellRecovery::RestoreAction::Activate, hasClient);
	}
}

void Baseapp::forgetRecoveryPeer(COMPONENT_ID cid)
{
	if (recoveryPeer_ == cid)
		recoveryEndpoint_.reset();
	recoveryPeers_.erase(cid);
}

void Baseapp::reconnectRecoveryPeer()
{
	const auto now = timestamp();
	auto& components = Components::getSingleton();
	if (!recoveryEndpoint_)
	{
		if (now < nextRecoveryConnect_)
			return;
		auto next = recoveryPeers_.upper_bound(recoveryPeer_);
		if (next == recoveryPeers_.end())
			next = recoveryPeers_.begin();
		recoveryPeer_ = next->first;
		const auto needsPeer = [this](const auto& entry) { return entry.second.target == recoveryPeer_; };
		if (components.findComponentChannel(CELLAPP_TYPE, recoveryPeer_) ||
			(!std::any_of(cellRecoveryStaging_.begin(), cellRecoveryStaging_.end(), needsPeer) &&
			 !std::any_of(recoveryActivations_.begin(), recoveryActivations_.end(), needsPeer)))
		{
			forgetRecoveryPeer(recoveryPeer_);
			return;
		}

		// 只由 Base 重连，使用断线前登记的地址，不阻塞游戏 Tick。
		nextRecoveryConnect_ = now + stampsPerSecond();
		recoveryEndpoint_.reset(Network::EndPoint::createPoolObject(OBJECTPOOL_POINT));
		auto& endpoint = *recoveryEndpoint_;
		endpoint.socket(SOCK_STREAM);
		if (!endpoint.good() || endpoint.setnonblocking(true) != 0)
		{
			recoveryEndpoint_.reset();
			return;
		}
		endpoint.addr(*next->second.pIntAddr);
		endpoint.setnodelay(true);
		if (endpoint.connect(false) != 0)
		{
			const int error = kbe_lasterror();
#if KBE_PLATFORM == PLATFORM_WIN32
			const bool connecting = error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
#else
			const bool connecting = error == EINPROGRESS || error == EINTR;
#endif
			if (!connecting)
			{
				recoveryEndpoint_.reset();
				return;
			}
		}
		recoveryConnectDeadline_ = now + 2 * stampsPerSecond();
	}

	auto& endpoint = *recoveryEndpoint_;
	if (endpoint.getremoteaddress(nullptr, nullptr) != 0)
	{
		int error = 0;
		socklen_t length = sizeof(error);
		if (now >= recoveryConnectDeadline_ ||
			getsockopt(endpoint.socket(), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length) != 0 ||
			error != 0)
			recoveryEndpoint_.reset();
		return;
	}

	const auto& peer = recoveryPeers_.at(recoveryPeer_);
	auto* route = components.findComponent(recoveryPeer_);
	if (route && (route->componentType != CELLAPP_TYPE || route->pChannel ||
		!route->pIntAddr || !(*route->pIntAddr == *peer.pIntAddr)))
	{
		forgetRecoveryPeer(recoveryPeer_);
		return;
	}
	const bool added = route == nullptr;
	if (added)
	{
		std::string externalAddress(peer.externalAddressEx);
		components.addComponent(peer.uid, peer.username, CELLAPP_TYPE, peer.cid, peer.globalOrderid,
			peer.groupOrderid, peer.gus, peer.pIntAddr->ip, peer.pIntAddr->port, peer.pExtAddr->ip,
			peer.pExtAddr->port, externalAddress, peer.pid, peer.cpu, peer.mem, peer.usedmem,
			peer.extradata, peer.extradata1, peer.extradata2, peer.extradata3);
		route = components.findComponent(CELLAPP_TYPE, peer.uid, peer.cid);
	}
	if (route && components.registerConnectedComponent(*route, recoveryEndpoint_.release(), recoveryPeer_, false) == 0)
	{
		INFO_MSG(fmt::format("Baseapp::cellRecovery: reconnected CellApp {}.\n", recoveryPeer_));
		forgetRecoveryPeer(recoveryPeer_);
	}
	else
	{
		recoveryEndpoint_.reset();
		if (added && route)
			components.delComponent(peer.uid, CELLAPP_TYPE, peer.cid, false, false);
	}
}

void Baseapp::onRecoveryCellBackup(Network::Channel* channel, MemoryStream& stream)
{
	BoundedStreamReader reader(stream);
	ENTITY_ID id = 0;
	if (channel && channel->isInternal() && !channel->isDestroyed() && channel->condemn() == 0 &&
		reader.read(id) && id > 0)
	{
		SmartPointer<Entity> entity(findEntity(id));
		if (entity)
		{
			stream.read_skip(sizeof(id));
			entity->onRecoveryCellBackup(*channel, stream);
		}
	}
	stream.done();
}

void Baseapp::restoreLegacyCellEntities(COMPONENT_ID cid, const std::vector<ENTITY_ID>& ids, bool excludeTransfers)
{
	RestoreEntityHandler* handler = nullptr;
	for (const auto id : ids)
	{
		auto* entity = findEntity(id);
		if (!entity || !entity->cellEntityCall() || entity->cellEntityCall()->componentID() != cid ||
			(excludeTransfers && entity->hasPendingCellTransfer()))
			continue;

		auto* cell = entity->cellEntityCall();
		S_RELEASE(cell);
		entity->cellEntityCall(nullptr);
		entity->installCellDataAttr(entity->getCellData());
		entity->onCellAppDeath();
		if (!handler)
		{
			handler = new RestoreEntityHandler(cid, networkInterface());
			pRestoreEntityHandlers_.emplace_back(handler);
		}
		handler->pushEntity(id);
	}
}
}
