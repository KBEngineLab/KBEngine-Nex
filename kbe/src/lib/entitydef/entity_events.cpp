/*
This source file is part of KBEngine.
*/

#include "entity_events.h"

#include "common/memorystream.h"

namespace KBEngine
{
namespace
{
const uint32 MAX_MIGRATED_SUBSCRIPTIONS = 65535;

struct MigratedCallback
{
	std::string ownerName;
	std::string methodName;
};

typedef std::vector<MigratedCallback> MigratedCallbacks;
struct MigratedEvent
{
	std::string eventName;
	MigratedCallbacks callbacks;
};
typedef std::vector<MigratedEvent> MigratedEvents;

bool stringAttribute(PyObject* object, const char* name, std::string& value)
{
	PyObject* attribute = PyObject_GetAttrString(object, name);
	if (attribute == NULL)
	{
		PyErr_Clear();
		return false;
	}

	const char* text = PyUnicode_AsUTF8(attribute);
	if (text != NULL)
		value = text;
	else
		PyErr_Clear();
	Py_DECREF(attribute);
	return text != NULL;
}

bool describeMigratedCallback(PyObject* entity, PyObject* callback,
	MigratedCallback& description)
{
	if (!PyMethod_Check(callback) || !stringAttribute(callback, "__name__", description.methodName))
		return false;

	PyObject* owner = PyMethod_GET_SELF(callback);
	if (owner == entity)
		return true;

	if (owner == NULL)
		return false;

	// 组件脚本类型由 BaseApp/CellApp 各自注册，公共 entitydef 库不能依赖其中任一静态 PyTypeObject。
	// BaseApp and CellApp register component script types independently, so entitydef validates their shared Python owner/name contract instead of a process-specific PyTypeObject.
	PyObject* componentOwner = PyObject_GetAttrString(owner, "owner");
	if (componentOwner == NULL)
	{
		PyErr_Clear();
		return false;
	}

	const bool belongsToEntity = componentOwner == entity;
	Py_DECREF(componentOwner);
	if (!belongsToEntity || !stringAttribute(owner, "name", description.ownerName))
		return false;

	return !description.ownerName.empty();
}

void collectMigratedEvents(PyObject* entity, MigratedEvents& output)
{
	PyObject* dict = PyObject_GetAttrString(entity, "__dict__");
	if (dict == NULL)
	{
		PyErr_Clear();
		return;
	}

	PyObject* registry = PyDict_GetItemString(dict, "__kbe_entity_events__");
	if (registry == NULL)
	{
		Py_DECREF(dict);
		return;
	}
	Py_INCREF(registry);
	Py_DECREF(dict);

	Py_ssize_t position = 0;
	PyObject* eventName = NULL;
	PyObject* callbacks = NULL;
	uint32 migratedCallbackCount = 0;
	while (PyDict_Next(registry, &position, &eventName, &callbacks))
	{
		const char* eventNameText = PyUnicode_Check(eventName) ? PyUnicode_AsUTF8(eventName) : NULL;
		if (eventNameText == NULL || !PyList_Check(callbacks))
		{
			PyErr_Clear();
			continue;
		}

		MigratedEvent event;
		event.eventName = eventNameText;
		const Py_ssize_t callbackCount = PyList_GET_SIZE(callbacks);
		for (Py_ssize_t callbackIndex = 0; callbackIndex < callbackCount; ++callbackIndex)
		{
			if (migratedCallbackCount >= MAX_MIGRATED_SUBSCRIPTIONS)
			{
				WARNING_MSG(fmt::format(
					"EntityEvents::addToStream: reached the migration subscription limit {}; remaining callbacks are skipped.\n",
					MAX_MIGRATED_SUBSCRIPTIONS));
				break;
			}

			MigratedCallback callback;
			if (describeMigratedCallback(entity, PyList_GET_ITEM(callbacks, callbackIndex), callback))
			{
				event.callbacks.push_back(callback);
				++migratedCallbackCount;
			}
			else
			{
				WARNING_MSG(fmt::format(
					"EntityEvents::addToStream: event '{}' has a callback that cannot be rebound after Cell migration; skipped.\n",
					event.eventName));
			}
		}

		if (!event.callbacks.empty())
			output.push_back(event);

		if (migratedCallbackCount >= MAX_MIGRATED_SUBSCRIPTIONS)
			break;
	}

	Py_DECREF(registry);
}
}

//-------------------------------------------------------------------------------------
void EntityEvents::addToStream(PyObject* entity, MemoryStream& stream)
{
	MigratedEvents events;
	collectMigratedEvents(entity, events);

	// 独立子流隔离事件格式，损坏订阅不会错位后续 callback manager 数据。
	// A length-delimited substream isolates event corruption from the following callback-manager payload.
	MemoryStream eventStream;
	eventStream << static_cast<uint32>(events.size());
	for (MigratedEvents::const_iterator eventIter = events.begin();
		eventIter != events.end(); ++eventIter)
	{
		eventStream << eventIter->eventName << static_cast<uint32>(eventIter->callbacks.size());
		for (MigratedCallbacks::const_iterator callbackIter = eventIter->callbacks.begin();
			callbackIter != eventIter->callbacks.end(); ++callbackIter)
		{
			eventStream << callbackIter->ownerName << callbackIter->methodName;
		}
	}

	stream.appendBlob(&eventStream);
}

//-------------------------------------------------------------------------------------
bool EntityEvents::createFromStream(PyObject* entity, MemoryStream& stream)
{
	std::string eventPayload;
	if (stream.readBlob(eventPayload) == 0)
	{
		ERROR_MSG("EntityEvents::createFromStream: missing event subscription payload.\n");
		return false;
	}

	MemoryStream eventStream(eventPayload.size());
	eventStream.append(eventPayload.data(), eventPayload.size());

	MigratedEvents events;
	try
	{
		uint32 eventCount = 0;
		eventStream >> eventCount;
		if (eventCount > MAX_MIGRATED_SUBSCRIPTIONS)
		{
			ERROR_MSG(fmt::format(
				"EntityEvents::createFromStream: rejected event count {}.\n", eventCount));
			return false;
		}

		uint32 totalCallbacks = 0;
		for (uint32 eventIndex = 0; eventIndex < eventCount; ++eventIndex)
		{
			MigratedEvent event;
			uint32 callbackCount = 0;
			eventStream >> event.eventName >> callbackCount;
			if (callbackCount > MAX_MIGRATED_SUBSCRIPTIONS - totalCallbacks)
			{
				ERROR_MSG(fmt::format(
					"EntityEvents::createFromStream: rejected callback count {} for event '{}'.\n",
					callbackCount, event.eventName));
				return false;
			}

			totalCallbacks += callbackCount;
			event.callbacks.reserve(callbackCount);
			for (uint32 callbackIndex = 0; callbackIndex < callbackCount; ++callbackIndex)
			{
				MigratedCallback callback;
				eventStream >> callback.ownerName >> callback.methodName;
				event.callbacks.push_back(callback);
			}
			events.push_back(event);
		}

		if (eventStream.length() != 0)
		{
			ERROR_MSG(fmt::format(
				"EntityEvents::createFromStream: event payload has {} trailing bytes.\n",
				eventStream.length()));
			return false;
		}
	}
	catch (MemoryStreamException&)
	{
		ERROR_MSG("EntityEvents::createFromStream: malformed event subscription payload.\n");
		return false;
	}

	// 先完整验证子流再替换注册表，损坏数据不会留下部分恢复的事件集合。
	// Validate the complete substream before replacing the registry so malformed data cannot leave a partially restored event set.
	clear(entity);
	for (MigratedEvents::const_iterator eventIter = events.begin();
		eventIter != events.end(); ++eventIter)
	{
		for (MigratedCallbacks::const_iterator callbackIter = eventIter->callbacks.begin();
			callbackIter != eventIter->callbacks.end(); ++callbackIter)
		{
			PyObject* callbackOwner = entity;
			if (!callbackIter->ownerName.empty())
			{
				callbackOwner = PyObject_GetAttrString(entity, callbackIter->ownerName.c_str());
				if (callbackOwner == NULL)
				{
					PyErr_Clear();
					WARNING_MSG(fmt::format(
						"EntityEvents::createFromStream: component '{}' is unavailable for event '{}'.\n",
						callbackIter->ownerName, eventIter->eventName));
					continue;
				}
			}
			else
			{
				Py_INCREF(callbackOwner);
			}

			PyObject* callback = PyObject_GetAttrString(callbackOwner, callbackIter->methodName.c_str());
			Py_DECREF(callbackOwner);
			if (callback == NULL || !PyCallable_Check(callback))
			{
				Py_XDECREF(callback);
				PyErr_Clear();
				WARNING_MSG(fmt::format(
					"EntityEvents::createFromStream: callback '{}.{}' is unavailable for event '{}'.\n",
					callbackIter->ownerName, callbackIter->methodName, eventIter->eventName));
				continue;
			}

			if (registerCallback(entity, eventIter->eventName.c_str(), callback) < 0)
				PyErr_PrintEx(0);
			Py_DECREF(callback);
		}
	}

	return true;
}
}
