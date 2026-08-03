/*
This source file is part of KBEngine.
*/

#ifndef KBE_ENTITY_EVENTS_H
#define KBE_ENTITY_EVENTS_H

#include "pyscript/scriptobject.h"

namespace KBEngine
{

class EntityEvents
{
public:
	static PyObject* pyRegister(PyObject* entity, PyObject* args)
	{
		const char* eventName = NULL;
		PyObject* callback = NULL;
		if (!PyArg_ParseTuple(args, "sO:registerEvent", &eventName, &callback))
			return NULL;
		if (!PyCallable_Check(callback))
		{
			PyErr_SetString(PyExc_TypeError, "registerEvent callback must be callable");
			return NULL;
		}

		PyObject* registry = getRegistry(entity, true);
		if (registry == NULL)
			return NULL;
		PyObject* callbacks = PyDict_GetItemString(registry, eventName);
		if (callbacks == NULL)
		{
			callbacks = PyList_New(0);
			if (callbacks == NULL || PyDict_SetItemString(registry, eventName, callbacks) != 0)
			{
				Py_XDECREF(callbacks);
				Py_DECREF(registry);
				return NULL;
			}
			Py_DECREF(callbacks);
			callbacks = PyDict_GetItemString(registry, eventName);
		}

		const int contains = PySequence_Contains(callbacks, callback);
		if (contains < 0)
		{
			Py_DECREF(registry);
			return NULL;
		}
		const bool added = contains == 0 && PyList_Append(callbacks, callback) == 0;
		Py_DECREF(registry);
		if (contains == 0 && !added)
			return NULL;
		return PyBool_FromLong(added);
	}

	static PyObject* pyDeregister(PyObject* entity, PyObject* args)
	{
		const char* eventName = NULL;
		PyObject* callback = NULL;
		if (!PyArg_ParseTuple(args, "sO:deregisterEvent", &eventName, &callback))
			return NULL;

		PyObject* registry = getRegistry(entity, false);
		if (registry == NULL)
		{
			PyErr_Clear();
			Py_RETURN_FALSE;
		}
		PyObject* callbacks = PyDict_GetItemString(registry, eventName);
		if (callbacks == NULL)
		{
			Py_DECREF(registry);
			Py_RETURN_FALSE;
		}

		const Py_ssize_t index = PySequence_Index(callbacks, callback);
		if (index < 0)
		{
			PyErr_Clear();
			Py_DECREF(registry);
			Py_RETURN_FALSE;
		}
		if (PySequence_DelItem(callbacks, index) != 0)
		{
			Py_DECREF(registry);
			return NULL;
		}
		if (PyList_GET_SIZE(callbacks) == 0 && PyDict_DelItemString(registry, eventName) != 0)
			PyErr_Clear();
		Py_DECREF(registry);
		Py_RETURN_TRUE;
	}

	static PyObject* pyFire(PyObject* entity, PyObject* args)
	{
		const Py_ssize_t argCount = PyTuple_Size(args);
		if (argCount < 1)
		{
			PyErr_SetString(PyExc_TypeError, "fireEvent requires an event name");
			return NULL;
		}
		PyObject* pyEventName = PyTuple_GET_ITEM(args, 0);
		if (!PyUnicode_Check(pyEventName))
		{
			PyErr_SetString(PyExc_TypeError, "fireEvent event name must be a string");
			return NULL;
		}
		const char* eventName = PyUnicode_AsUTF8(pyEventName);
		if (eventName == NULL)
			return NULL;

		PyObject* registry = getRegistry(entity, false);
		if (registry == NULL)
		{
			PyErr_Clear();
			Py_RETURN_TRUE;
		}
		PyObject* callbacks = PyDict_GetItemString(registry, eventName);
		if (callbacks == NULL)
		{
			Py_DECREF(registry);
			Py_RETURN_TRUE;
		}

		// 回调允许在触发期间注销自身，快照保证当前轮次的列表迭代稳定。
		// Callbacks may deregister themselves during dispatch; a snapshot keeps this pass stable.
		PyObject* snapshot = PySequence_List(callbacks);
		PyObject* eventArgs = PyTuple_GetSlice(args, 1, argCount);
		Py_DECREF(registry);
		if (snapshot == NULL || eventArgs == NULL)
		{
			Py_XDECREF(snapshot);
			Py_XDECREF(eventArgs);
			return NULL;
		}

		const Py_ssize_t callbackCount = PyList_GET_SIZE(snapshot);
		for (Py_ssize_t index = 0; index < callbackCount; ++index)
		{
			PyObject* result = PyObject_CallObject(PyList_GET_ITEM(snapshot, index), eventArgs);
			if (result != NULL)
				Py_DECREF(result);
			else
				PyErr_PrintEx(0);
		}
		Py_DECREF(eventArgs);
		Py_DECREF(snapshot);
		Py_RETURN_TRUE;
	}

	static void clear(PyObject* entity)
	{
		PyObject* dict = PyObject_GetAttrString(entity, "__dict__");
		if (dict == NULL)
		{
			PyErr_Clear();
			return;
		}
		if (PyDict_DelItemString(dict, registryKey()) != 0)
			PyErr_Clear();
		Py_DECREF(dict);
	}

private:
	static const char* registryKey()
	{
		return "__kbe_entity_events__";
	}

	static PyObject* getRegistry(PyObject* entity, bool create)
	{
		PyObject* dict = PyObject_GetAttrString(entity, "__dict__");
		if (dict == NULL)
			return NULL;
		PyObject* registry = PyDict_GetItemString(dict, registryKey());
		if (registry != NULL)
		{
			Py_INCREF(registry);
			Py_DECREF(dict);
			return registry;
		}
		if (!create)
		{
			Py_DECREF(dict);
			return NULL;
		}

		registry = PyDict_New();
		if (registry == NULL || PyDict_SetItemString(dict, registryKey(), registry) != 0)
		{
			Py_XDECREF(registry);
			Py_DECREF(dict);
			return NULL;
		}
		Py_DECREF(dict);
		return registry;
	}
};

}

#endif // KBE_ENTITY_EVENTS_H
