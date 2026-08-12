/*
This source file is part of KBEngine
For the latest info, see http://www.kbengine.org/

Copyright (c) 2008-2018 KBEngine.

KBEngine is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

KBEngine is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License
along with KBEngine.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "script.h"
#include "pyurl.h"
#include "scriptstdouterr.h"
#include "py_macros.h"
#include "helper/profile.h"
#include <limits>

namespace KBEngine{ namespace script {

bool PyUrl::isInit = false;
std::map<const Network::Http::Request*, PyObjectPtr> PyUrl::pyCallbacks;

namespace
{
bool parseHeaders(PyObject* pyHeaders, std::map<std::string, std::string>& headers)
{
	if (!PyDict_Check(pyHeaders))
	{
		PyErr_SetString(PyExc_TypeError, "KBEngine.urlopen: headers must be a dict[str, str]");
		return false;
	}

	PyObject* key = NULL;
	PyObject* value = NULL;
	Py_ssize_t pos = 0;
	while (PyDict_Next(pyHeaders, &pos, &key, &value))
	{
		if (!PyUnicode_Check(key) || !PyUnicode_Check(value))
		{
			PyErr_SetString(PyExc_TypeError, "KBEngine.urlopen: header names and values must be str");
			return false;
		}

		// UTF-8转换仍可能因内存错误失败，不能把NULL传给std::string构造函数。
		// UTF-8 conversion can still fail on allocation, so NULL must never reach a std::string constructor.
		const char* headerName = PyUnicode_AsUTF8(key);
		const char* headerValue = PyUnicode_AsUTF8(value);
		if (!headerName || !headerValue)
			return false;

		headers[headerName] = headerValue;
	}

	return true;
}
}

//-------------------------------------------------------------------------------------
bool PyUrl::initialize(Script* pScript)
{
	if(isInit)
		return true;
	
	isInit = true;

	// 注册产生uuid方法到py
	APPEND_SCRIPT_MODULE_METHOD(pScript->getModule(),	urlopen,	__py_urlopen,	METH_VARARGS,	0);
	return isInit;
}

//-------------------------------------------------------------------------------------
void PyUrl::finalise(void)
{
	if (!isInit)
		return;

	isInit = false;
	pyCallbacks.clear();
}

//-------------------------------------------------------------------------------------
void PyUrl::onHttpCallback(bool success, const Network::Http::Request& pRequest, const std::string& data)
{
	if (!isInit)
		return;

	std::map<const Network::Http::Request*, PyObjectPtr>::iterator callbackIter =
		pyCallbacks.find(&pRequest);
	if (callbackIter == pyCallbacks.end())
	{
		ERROR_MSG("PyUrl::onHttpCallback: request callback is no longer registered.\n");
		return;
	}

	// 本地强引用覆盖Python回调重入finalise()的情况，注册项可在调用前安全移除。
	// A local strong reference covers reentrant finalise() calls, allowing the registry entry to be removed before invocation.
	PyObjectPtr pyCallback = callbackIter->second;
	pyCallbacks.erase(callbackIter);

	// httpcode, data, headers, opt_success, url 
	PyObject* pyargs = PyTuple_New(5);

	PyTuple_SET_ITEM(pyargs, 0, PyLong_FromLong(pRequest.getHttpCode()));
	PyTuple_SET_ITEM(pyargs, 1, PyUnicode_FromString(pRequest.getReceivedContent()));
	PyTuple_SET_ITEM(pyargs, 2, PyUnicode_FromString(pRequest.getReceivedHeader()));
	PyTuple_SET_ITEM(pyargs, 3, PyBool_FromLong(success));
	PyTuple_SET_ITEM(pyargs, 4, PyUnicode_FromString(pRequest.url()));

	PyObject* pyRet = PyObject_CallObject(pyCallback.get(), pyargs);
	if (pyRet == NULL)
	{
		SCRIPT_ERROR_CHECK();
	}
	else
	{
		Py_DECREF(pyRet);
	}

	Py_DECREF(pyargs);
}

//-------------------------------------------------------------------------------------
PyObject* PyUrl::__py_urlopen(PyObject* self, PyObject* args)
{
	int argCount = (int)PyTuple_Size(args);
	PyObject* pyCallback = NULL;
	char* surl = NULL;
	char* postData = NULL;
	Py_ssize_t postDataLength = 0;
	std::map<std::string, std::string> map_headers;

	if (argCount < 1 || argCount > 4)
	{
		PyErr_SetString(PyExc_TypeError, "KBEngine.urlopen expects 1 to 4 arguments");
		return NULL;
	}

	if (argCount == 1)
	{
		if (!PyArg_ParseTuple(args, "s:urlopen", &surl))
			return NULL;
	}
	else if (argCount == 2)
	{
		if (!PyArg_ParseTuple(args, "sO:urlopen", &surl, &pyCallback))
			return NULL;
	}
	else if (argCount == 3)
	{
		PyObject* pyobj = NULL;
		if (!PyArg_ParseTuple(args, "sOO:urlopen", &surl, &pyCallback, &pyobj))
			return NULL;

		// 检查是headers还是post data
		if (PyDict_Check(pyobj))
		{
			if (!parseHeaders(pyobj, map_headers))
				return NULL;
		}
		else if (PyBytes_Check(pyobj))
		{
			if (PyBytes_AsStringAndSize(pyobj, &postData, &postDataLength) < 0)
			{
				SCRIPT_ERROR_CHECK();
				return NULL;
			}
		}
		else
		{
			PyErr_SetString(PyExc_TypeError, "KBEngine.urlopen: third argument must be post data bytes or a headers dict");
			return NULL;
		}
	}
	else if (argCount == 4)
	{
		PyObject* pypost = NULL;
		PyObject* pyheaders = NULL;
		if (!PyArg_ParseTuple(args, "sOOO:urlopen", &surl, &pyCallback, &pypost, &pyheaders))
			return NULL;

		if (!parseHeaders(pyheaders, map_headers))
			return NULL;

		if (PyBytes_Check(pypost))
		{
			if (PyBytes_AsStringAndSize(pypost, &postData, &postDataLength) < 0)
			{
				SCRIPT_ERROR_CHECK();
				return NULL;
			}
		}
		else
		{
			PyErr_SetString(PyExc_TypeError, "KBEngine.urlopen: POST data must be bytes");
			return NULL;
		}
	}

	if (pyCallback && !PyCallable_Check(pyCallback))
	{
		PyErr_SetString(PyExc_TypeError, "KBEngine.urlopen: callback must be callable");
		return NULL;
	}

	// HTTP层使用32位长度，拒绝无法完整表达的数据，避免静默截断请求体。
	// The HTTP layer uses a 32-bit length, so reject unrepresentable data instead of silently truncating the request body.
	if (static_cast<uint64>(postDataLength) > std::numeric_limits<unsigned int>::max())
	{
		PyErr_SetString(PyExc_OverflowError, "KBEngine.urlopen: POST data exceeds the HTTP request size limit");
		return NULL;
	}

	Network::Http::Request* pRequest = new Network::Http::Request();
	if (pyCallback)
	{
		pyCallbacks[pRequest] = PyObjectPtr(pyCallback);
		pRequest->setCallback(onHttpCallback);
	}

	if (map_headers.size() > 0)
	{
		Network::Http::Request::Status result = pRequest->setHeader(map_headers);
		if (Network::Http::Request::OK != result)
		{
			delete pRequest;
			return PyLong_FromLong(result);
		}
	}

	if (postDataLength > 0 && postData)
	{
		Network::Http::Request::Status result = pRequest->setPostData(
			postData, static_cast<unsigned int>(postDataLength));
		if (Network::Http::Request::OK != result)
		{
			delete pRequest;
			return PyLong_FromLong(result);
		}
	}

	Network::Http::Request::Status result = pRequest->setURL(surl);
	if (Network::Http::Request::OK != result)
	{
		delete pRequest;
		return PyLong_FromLong(result);
	}

	Network::Http::Request::Status status = Network::Http::perform(pRequest);
	return PyLong_FromLong(status);
}

//-------------------------------------------------------------------------------------

}
}
