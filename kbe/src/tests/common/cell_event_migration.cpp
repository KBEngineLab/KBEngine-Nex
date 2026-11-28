#include "entitydef/entity_events.h"
#include "common/memorystream.h"
#include "helper/debug_helper.h"
#include "server/cellapp/boundary_ghosts.h"
#include "server/cellapp/cell_handoff_snapshot.h"

#include <cstdlib>
#include <iostream>

namespace
{
bool require(bool condition, const char* message)
{
	if (condition)
		return true;

	std::cerr << message << std::endl;
	if (PyErr_Occurred())
		PyErr_PrintEx(0);
	return false;
}

bool checkCopyReservations()
{
	using namespace KBEngine;
	using namespace CellHandoff;
	CopyBudget budget;
	const CopyPeers both{{8001, 1}, {8002, 2}};
	size_t slowBytes = CopyBudget::ORDINARY_BYTES;
	auto queued = [&](COMPONENT_ID app) { return app == 8002 ? slowBytes : 0; };
	std::vector<std::unique_ptr<CopyReservation>> admitted;
	for (size_t i = 0; i < CopyBudget::SLOTS; ++i)
	{
		auto copy = budget.tryReserve(both, queued);
		if (!require(copy && copy->peers == both, "handoff lost reserved first-snapshot capacity"))
			return false;
		admitted.emplace_back(std::move(copy));
	}
	if (!require(!budget.tryReserve(both, queued), "concurrent handoffs exceeded reserved queue limit"))
		return false;
	admitted.clear();
	slowBytes += (CopyBudget::SLOTS - 1) * CopyBudget::SLOT;
	auto last = budget.tryReserve(both, queued);
	if (!require(last != nullptr, "exact queue limit should admit one reserved handoff"))
		return false;
	++slowBytes;
	last.reset();
	if (!require(!budget.tryReserve(both, queued), "congested peer admitted beyond hard limit"))
		return false;
	for (size_t i = 0; i < CopyBudget::SLOTS; ++i)
		admitted.emplace_back(budget.tryReserve({both.front()}, queued));
	return require(admitted.back() != nullptr, "rejection on slow peer leaked capacity on ready peer");
}

bool checkHandoffAbsenceOrdering()
{
	using KBEngine::HandoffCopyAbsence;
	const HandoffCopyAbsence before{8001, 1, 101, 7}, after{8002, 2, 102, 8};
	HandoffCopyAbsence pending;
	if (!require(!pending.receive(after, before, false) && pending.matches(after),
		"an early ABSENT must wait for the matching MOVED"))
		return false;
	for (const HandoffCopyAbsence other : {HandoffCopyAbsence{8003, 2, 102, 8}, {8002, 3, 102, 8},
		{8002, 2, 103, 8}, {8002, 2, 102, 9}})
		if (!require(!pending.matches(other), "stale ABSENT matched a different handoff"))
			return false;
	pending = {};
	return require(pending.receive(after, after, true), "MOVED then ABSENT must withdraw the placeholder") &&
		require(!pending.receive(after, after, false) && !pending.epoch, "late ABSENT removed a formal CREATE") &&
		require(!pending.receive(before, after, true) && !pending.epoch, "late ABSENT superseded newer ownership");
}

bool checkHandoffBody(KBEngine::COMPONENT_ID base)
{
	using namespace KBEngine;
	const CellHandoff::Route route{8001, 8002, 2, 101, base ? 102u : 0u, 1, 2};
	const CellHandoff::PeerProcesses processes{11, 22};
	const Position3D pos(1.f, 2.f, 3.f);
	const Direction3D dir;
	const CellHandoff::CopyPeers peers{{8003, 3}, {8004, 4}};
	const auto write = [&](MemoryStream& body, const auto& snapshot) {
		return CellHandoff::writeSnapshotBody(body, 1, 1, pos, dir, base, 3, route, processes, peers, snapshot);
	};
	MemoryStream prefix;
	const auto offset = write(prefix, [](MemoryStream&) {});
	std::vector<uint8> data(MemoryStream::MAX_SIZE - offset, 7);
	MemoryStream valid;
	const auto start = write(valid, [&](MemoryStream& stream) { stream.append(data.data(), data.size() - 1); });
	valid.rpos(start);
	if (!require(valid.wpos() == MemoryStream::MAX_SIZE - 1 && valid.length() == data.size() - 1 &&
		valid.data()[valid.rpos()] == 7, "handoff rollback must start at the snapshot"))
		return false;
	MemoryStream rejected;
	try
	{
		write(rejected, [&](MemoryStream& stream) { stream.append(data.data(), data.size()); });
		return require(false, "handoff body at the stream limit was accepted");
	}
	catch (const MemoryStreamWriteOverflow&)
	{
		return require(rejected.wpos() == offset, "partial handoff snapshot written");
	}
}
}

int main()
{
	// 损坏数据校验会记录日志，先建立测试日志实例，避免异常路径触发单例断言。
	KBEngine::DebugHelper debugHelper;
	Py_Initialize();
	if (!checkCopyReservations() || !checkHandoffAbsenceOrdering() || !checkHandoffBody(7001) || !checkHandoffBody(0))
	{
		Py_Finalize();
		return EXIT_FAILURE;
	}

	PyObject* mainModule = PyImport_AddModule("__main__");
	PyObject* globals = mainModule ? PyModule_GetDict(mainModule) : NULL;
	const char* script =
		"class MigratingEntity:\n"
		"    def __init__(self):\n"
		"        self.total = 0\n"
		"    def on_ping(self, value):\n"
		"        self.total += value\n"
		"source = MigratingEntity()\n"
		"target = MigratingEntity()\n";

	PyObject* scriptResult = globals ?
		PyRun_String(script, Py_file_input, globals, globals) : NULL;
	const bool fixturesCreated = require(scriptResult != NULL,
		"failed to create Python migration fixtures");
	Py_XDECREF(scriptResult);
	if (!fixturesCreated)
	{
		Py_Finalize();
		return EXIT_FAILURE;
	}

	PyObject* source = PyDict_GetItemString(globals, "source");
	PyObject* target = PyDict_GetItemString(globals, "target");
	PyObject* callback = source ? PyObject_GetAttrString(source, "on_ping") : NULL;
	PyObject* registerArgs = callback ? Py_BuildValue("(sO)", "ping", callback) : NULL;
	PyObject* registered = registerArgs ?
		KBEngine::EntityEvents::pyRegister(source, registerArgs) : NULL;
	const bool registrationOk = require(registered == Py_True,
		"failed to register source Entity event callback");
	Py_XDECREF(registered);
	Py_XDECREF(registerArgs);
	Py_XDECREF(callback);
	if (!registrationOk)
	{
		Py_Finalize();
		return EXIT_FAILURE;
	}

	KBEngine::MemoryStream migrationStream;
	KBEngine::EntityEvents::addToStream(source, migrationStream);
	if (!require(KBEngine::EntityEvents::createFromStream(target, migrationStream),
		"failed to restore Entity event subscriptions"))
	{
		Py_Finalize();
		return EXIT_FAILURE;
	}

	PyObject* fireArgs = Py_BuildValue("(si)", "ping", 7);
	PyObject* fired = fireArgs ? KBEngine::EntityEvents::pyFire(target, fireArgs) : NULL;
	Py_XDECREF(fired);
	Py_XDECREF(fireArgs);
	PyObject* total = PyObject_GetAttrString(target, "total");
	const bool firedOnTarget = require(total != NULL && PyLong_AsLong(total) == 7,
		"restored Entity event callback did not execute on the target object");
	Py_XDECREF(total);
	if (!firedOnTarget)
	{
		Py_Finalize();
		return EXIT_FAILURE;
	}

	KBEngine::MemoryStream malformedEventPayload;
	malformedEventPayload << static_cast<KBEngine::uint32>(1);
	KBEngine::MemoryStream malformedMigrationStream;
	malformedMigrationStream.appendBlob(&malformedEventPayload);
	const KBEngine::uint32 sentinel = 0x51A7B10B;
	malformedMigrationStream << sentinel;
	if (!require(!KBEngine::EntityEvents::createFromStream(target, malformedMigrationStream),
		"malformed Entity event payload was accepted"))
	{
		Py_Finalize();
		return EXIT_FAILURE;
	}

	KBEngine::uint32 restoredSentinel = 0;
	malformedMigrationStream >> restoredSentinel;
	if (!require(restoredSentinel == sentinel,
		"malformed Entity event payload consumed the following migration section"))
	{
		Py_Finalize();
		return EXIT_FAILURE;
	}

	Py_Finalize();
	std::cout << "CELL_EVENT_MIGRATION_TEST_PASS" << std::endl;
	return EXIT_SUCCESS;
}
