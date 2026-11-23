#include "cellappmgr.h"
#include "cellappmgr_interface.h"
#include "../cellapp/cellapp_interface.h"
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

	if (found == spaceLayouts_.end() || found->second.version != packet.version ||
		found->second.layout.cellForApp(packet.sender) == NULL)
		return;
	Transaction& transaction = found->second;
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
				INFO_MSG(fmt::format("Cellappmgr::spaceLayout: active space={}, version={}, cells={}.\n",
					packet.spaceID, transaction.version, transaction.layout.participants().size()));
		}
		break;
	}
	case SHUTDOWN:
		if (packet.sender != transaction.layout.owner || transaction.state == RELEASING)
			return;
		transaction.advance(RELEASING, timestamp());
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
	if (spaceLayouts_.empty())
		return;
	const uint64 now = timestamp();
	if (now < nextSpaceLayoutCheck_)
		return;
	nextSpaceLayoutCheck_ = now + stampsPerSecond();
	for (auto& entry : spaceLayouts_)
	{
		SpaceLayout::Transaction& transaction = entry.second;
		if ((transaction.state == SpaceLayout::PREPARING || transaction.state == SpaceLayout::COMMITTING) &&
			now - transaction.started > stampsPerSecond() * 30)
			failSpaceLayout(entry.first, transaction, "Cell preparation or confirmation timed out",
				transaction.state == SpaceLayout::COMMITTING);
	}
}

void Cellappmgr::onSpaceLayoutCellLost(COMPONENT_ID cid)
{
	for (auto& entry : spaceLayouts_)
	{
		SpaceLayout::Transaction& transaction = entry.second;
		if (transaction.layout.cellForApp(cid) != NULL && transaction.state != SpaceLayout::FAILED)
			failSpaceLayout(entry.first, transaction, "a participating CellApp disconnected",
				transaction.state != SpaceLayout::PREPARING);
	}
}

}
