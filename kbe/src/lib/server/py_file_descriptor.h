// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

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
	// 旧 readiness 兼容对象构造函数。
	// 当前脚本层 registerReadFileDescriptor/registerWriteFileDescriptor 已废弃并直接报错，
	// 但 C++ 类仍保留这个构造函数，避免内部旧路径或后续兼容代码需要普通 fd 通知对象时
	// 大面积调整类结构。
	PyFileDescriptor(int fd, PyObject* pyCallback, bool write);

	// 新 completion 读侧构造函数。
	// accept=true 表示 listener fd：底层完成 accept 后回调脚本 onAccept(listenerFD, clientFD, errorCode)。
	// accept=false 表示已连接 TCP fd：底层完成 recv 后回调脚本 onRead(fd, data, errorCode)。
	// reserved 仅用于区分此构造函数和旧 bool write 构造函数，调用方传 0。
	PyFileDescriptor(int fd, PyObject* pyCallback, bool accept, int reserved);
	virtual ~PyFileDescriptor();

	/**
		脚本请求(注册/注销)文件描述符。

		旧接口:
			registerReadFileDescriptor / registerWriteFileDescriptor
			deregisterReadFileDescriptor / deregisterWriteFileDescriptor
		已经不再提供 readiness 语义，只保留名字用于给旧脚本明确报错，引导迁移到 completion API。

		新接口:
			registerAcceptFileDescriptor(listenerFD, onAccept)
			deregisterAcceptFileDescriptor(listenerFD)
			registerReadDataFileDescriptor(fd, onRead)
			deregisterReadDataFileDescriptor(fd)
			writeFileDescriptor(fd, data, onWriteComplete)

		脚本回调签名:
			onAccept(listenerFD, clientFD, errorCode)
			onRead(fd, data, errorCode)
			onWriteComplete(fd, bytesWritten, errorCode)
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
		// 旧 readiness 模式，仅保留给类内部兼容；脚本入口已经废弃。
		MODE_READ_READY = 0,
		MODE_WRITE_READY,
		// listener fd 的 accept completion 模式。
		MODE_ACCEPT,
		// 已连接 TCP fd 的 recv completion/data 模式。
		MODE_READ_DATA,
		// writeFileDescriptor 的发送完成回调模式。
		MODE_WRITE_COMPLETION
	};

	struct WriteRequest
	{
		WriteRequest();
		WriteRequest(int bytesArg, PyObject* pyCallbackArg);

		int bytes;
		// 每次 writeFileDescriptor 都可以带独立回调。
		// 这里保存强引用，避免脚本侧临时 callback 在 send completion 回来前被 GC。
		PyObjectPtr pyCallback;
	};

	int fd_;
	PyObjectPtr pyCallback_;

	bool write_;
	Mode mode_;
	// 脚本层写请求队列。底层 completion poller 负责真实发送，
	// 这里负责在写队列完全 flush 后按请求顺序回调脚本。
	std::deque<WriteRequest> writeRequests_;

	// 下面三个 map 是脚本层 fd wrapper 的所有权索引。
	// PyFileDescriptor 对象由 register* 创建、由 deregister* 或写完成后 delete this 销毁；
	// fd 可能同时属于不同语义，因此 accept/readData/writeCompletion 分开保存，避免误删。
	static std::map<int, PyFileDescriptor*> readDataDescriptors_;
	static std::map<int, PyFileDescriptor*> acceptDescriptors_;
	static std::map<int, PyFileDescriptor*> writeCompletionDescriptors_;
};

}

#endif // KBE_PY_FILE_DESCRIPTOR_H
