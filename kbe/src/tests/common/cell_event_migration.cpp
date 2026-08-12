#include "entitydef/entity_events.h"
#include "common/memorystream.h"

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
}

int main()
{
	Py_Initialize();

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
