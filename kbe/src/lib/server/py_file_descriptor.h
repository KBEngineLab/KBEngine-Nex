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

#ifndef KBE_PY_FILE_DESCRIPTOR_H
#define KBE_PY_FILE_DESCRIPTOR_H

#include "common/common.h"
#include "pyscript/scriptobject.h"
#include "common/smartpointer.h"
#include <deque>
#include <map>

namespace KBEngine{
typedef SmartPointer<PyObject> PyObjectPtr;

class PyFileDescriptor : public Network::InputNotificationHandler, public Network::OutputNotificationHandler
{
public:
	PyFileDescriptor(int fd, PyObject* pyCallback, bool write);
	// 完成式读路径使用独立构造函数，避免改变旧就绪通知对象的行为。
	// Completion-based read paths use a separate constructor so legacy readiness objects retain their behavior.
	PyFileDescriptor(int fd, PyObject* pyCallback, bool accept, int reserved);
	virtual ~PyFileDescriptor();
	
	/**
		旧 readiness 名称保留用于给迁移中的脚本返回明确错误；实际 IO 必须使用 completion API。
		Legacy readiness names remain only to return explicit migration errors; actual IO must use completion APIs.
	*/
	static PyObject* __py_registerReadFileDescriptor(PyObject* self, PyObject* args);
	static PyObject* __py_registerWriteFileDescriptor(PyObject* self, PyObject* args);
	static PyObject* __py_deregisterReadFileDescriptor(PyObject* self, PyObject* args);
	static PyObject* __py_deregisterWriteFileDescriptor(PyObject* self, PyObject* args);
	static PyObject* __py_registerReadDataFileDescriptor(PyObject* self, PyObject* args);
	static PyObject* __py_deregisterReadDataFileDescriptor(PyObject* self, PyObject* args);
	static PyObject* __py_registerAcceptFileDescriptor(PyObject* self, PyObject* args);
	static PyObject* __py_deregisterAcceptFileDescriptor(PyObject* self, PyObject* args);
	static PyObject* __py_writeFileDescriptor(PyObject* self, PyObject* args);
protected:

	virtual int handleInputNotification( int fd );
	virtual int handleOutputNotification( int fd );

	void callback();
	void callbackAccept();
	void callbackReadData();
	void callbackWriteComplete(int bytesWritten, int errorCode);
	bool enqueueWrite(PyObject* pyData, PyObject* pyCallback);
	static int lastErrorCode();

	enum Mode
	{
		MODE_READ_READY = 0,
		MODE_WRITE_READY,
		MODE_ACCEPT,
		MODE_READ_DATA,
		MODE_WRITE_COMPLETION
	};

	struct WriteRequest
	{
		WriteRequest(int bytesArg, PyObject* pyCallbackArg);

		int bytes;
		// 写请求持有回调的强引用，确保异步发送结束前临时 Python 对象不会被回收。
		// Each write request owns a strong callback reference so temporary Python objects survive until completion.
		PyObjectPtr pyCallback;
	};

	int fd_;
	PyObjectPtr pyCallback_;

	bool write_;
	Mode mode_;
	std::deque<WriteRequest> writeRequests_;

	// 不同完成语义分别维护所有权，允许同一连接同时注册读数据并提交写请求。
	// Separate ownership maps allow one connection to receive data while independently submitting writes.
	static std::map<int, PyFileDescriptor*> readDataDescriptors_;
	static std::map<int, PyFileDescriptor*> acceptDescriptors_;
	static std::map<int, PyFileDescriptor*> writeCompletionDescriptors_;
};

}

#endif // KBE_PY_FILE_DESCRIPTOR_H
