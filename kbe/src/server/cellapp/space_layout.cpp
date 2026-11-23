#include "cellapp.h"
#include "cellapp_interface.h"
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
	if (packet.operation == PREPARE && packet.version == layoutVersion_)
		layoutPreparedVersion_ = packet.version;
	if (!isGood() || layoutRemovalPending_ || packet.layout.cellForApp(g_componentID) == NULL ||
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
		layoutPrepareReceived_ = packet.operation == PREPARE;
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
	layoutVersion_ = packet.version;
	layoutState_ = PREPARING;
	layoutError_.clear();
	layoutPinned_ = true;
	layoutReplica_ = replica;
	layoutReadySent_ = false;
	layoutPrepareReceived_ = packet.operation == PREPARE;
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

void Space::setLayoutState(SpaceLayout::State state, const std::string& error)
{
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
	if (!hasManagedCell())
		return;
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

bool Cellapp::sendSpaceLayout(const SpaceLayout::Packet& packet)
{
	Network::Channel* channel = Components::getSingleton().getCellappmgrChannel();
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
			if (!isShuttingdown() && space->canAcceptEntities())
			{
				for (const auto& entry : packet.cellPeerRoutes)
				{
					if (entry.first <= componentID_)
						continue;
					const auto* current = components.findComponent(entry.first);
					if (current && (current->componentType != CELLAPP_TYPE || current->pChannel))
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
			Network::Channel* manager = Components::getSingleton().getCellappmgrChannel();
			if (manager != NULL)
			{
				Network::Bundle* presence = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
				presence->newMessage(CellappmgrInterface::updateSpaceData);
				(*presence) << componentID_ << space->id() << space->getScriptModuleName()
					<< false << space->getGeometryPath();
				manager->send(presence);
			}
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
		if (space->layoutState() == ACTIVE)
			space->applyLayoutData(packet.key, packet.value, packet.operation == DELETE_DATA, packet.revision);
		break;
	case RESOURCES:
		if (space->layoutState() == ACTIVE && packet.revision > space->resourceRevision())
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

PyObject* Cellapp::__py_getSpaceLayout(PyObject*, PyObject* args)
{
	SPACE_ID spaceID = 0;
	if (!PyArg_ParseTuple(args, "I:getSpaceLayout", &spaceID))
		return NULL;
	Space* space = Spaces::findSpace(spaceID);
	if (space == NULL)
		Py_RETURN_NONE;
	PyObject* cells = PyList_New(0);
	for (const SpaceLayout::Node& node : space->layout().nodes)
	{
		if (node.axis != SpaceLayout::LEAF)
			continue;
		SpaceLayout::Bounds bounds;
		space->layout().boundsForApp(node.cellappID, bounds);
		PyObject* cell = Py_BuildValue("{s:I,s:K,s:(ffff)}",
			"cellID", node.cellID, "cellappID", static_cast<unsigned long long>(node.cellappID),
			"bounds", bounds.minX, bounds.minZ, bounds.maxX, bounds.maxZ);
		PyList_Append(cells, cell);
		Py_DECREF(cell);
	}
	return Py_BuildValue("{s:K,s:s,s:s,s:K,s:K,s:I,s:O,s:O,s:N}",
		"version", static_cast<unsigned long long>(space->layoutVersion()),
		"state", SpaceLayout::stateName(space->layoutState()), "error", space->layoutError().c_str(),
		"resourceRevision", static_cast<unsigned long long>(space->resourceRevision()),
		"ownerCellAppID", static_cast<unsigned long long>(space->layout().owner),
		"localCellID", space->pCell() == NULL ? 0u : space->pCell()->id(),
		"resourcesReady", space->isGeometryLoading() ? Py_False : Py_True,
		"hasNavigation", space->pNavHandle() == NULL ? Py_False : Py_True,
		"cells", cells);
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
