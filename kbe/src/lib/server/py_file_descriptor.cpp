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

#include "network/event_dispatcher.h"
#include "network/event_poller.h"
#include "network/network_interface.h"
#include "py_file_descriptor.h"
#include "server/components.h"
#include "helper/debug_helper.h"
#include "pyscript/pyobject_pointer.h"
#if KBE_PLATFORM == PLATFORM_WIN32
#include <winsock2.h>
#else
#include <errno.h>
#endif

namespace KBEngine{

std::map<int, PyFileDescriptor*> PyFileDescriptor::readDataDescriptors_;
std::map<int, PyFileDescriptor*> PyFileDescriptor::acceptDescriptors_;
std::map<int, PyFileDescriptor*> PyFileDescriptor::writeCompletionDescriptors_;

//-------------------------------------------------------------------------------------
PyFileDescriptor::WriteRequest::WriteRequest(int bytesArg, PyObject* pyCallbackArg) :
	bytes(bytesArg),
	pyCallback(pyCallbackArg)
{
}

//-------------------------------------------------------------------------------------
PyFileDescriptor::PyFileDescriptor(int fd, PyObject* pyCallback, bool write) : 
	fd_(fd),
	pyCallback_(pyCallback),
	write_(write),
	mode_(write ? MODE_WRITE_READY : MODE_READ_READY),
	writeRequests_()
{
	if(write)
		Components::getSingleton().pNetworkInterface()->dispatcher().registerWriteFileDescriptor(fd_, this);
	else
		Components::getSingleton().pNetworkInterface()->dispatcher().registerReadFileDescriptor(fd_, this);
}

//-------------------------------------------------------------------------------------
PyFileDescriptor::PyFileDescriptor(int fd, PyObject* pyCallback, bool accept, int reserved) :
	fd_(fd),
	pyCallback_(pyCallback),
	write_(false),
	mode_(accept ? MODE_ACCEPT : MODE_READ_DATA),
	writeRequests_()
{
	(void)reserved;

	// 完成后端先把 accept 或 recv 结果放入队列，再通过读处理器唤醒脚本桥接层。
	// The completion backend queues accept or recv results before waking this script bridge through its read handler.
	if(accept)
		acceptDescriptors_[fd_] = this;
	else
		readDataDescriptors_[fd_] = this;

	Components::getSingleton().pNetworkInterface()->dispatcher().registerReadFileDescriptor(fd_, this);
}

//-------------------------------------------------------------------------------------
PyFileDescriptor::~PyFileDescriptor()
{
	// 先删除脚本层索引再注销 poller，避免迟到事件查到即将释放的包装对象。
	// Remove the script-side index before deregistering the poller so late events cannot find an object being destroyed.
	if(mode_ == MODE_WRITE_COMPLETION)
	{
		std::map<int, PyFileDescriptor*>::iterator iter = writeCompletionDescriptors_.find(fd_);
		if(iter != writeCompletionDescriptors_.end() && iter->second == this)
			writeCompletionDescriptors_.erase(iter);

		Components::getSingleton().pNetworkInterface()->dispatcher().deregisterWriteFileDescriptor(fd_);
	}
	else if(mode_ == MODE_READ_DATA)
	{
		std::map<int, PyFileDescriptor*>::iterator iter = readDataDescriptors_.find(fd_);
		if(iter != readDataDescriptors_.end() && iter->second == this)
			readDataDescriptors_.erase(iter);

		Components::getSingleton().pNetworkInterface()->dispatcher().deregisterReadFileDescriptor(fd_);
	}
	else if(mode_ == MODE_ACCEPT)
	{
		std::map<int, PyFileDescriptor*>::iterator iter = acceptDescriptors_.find(fd_);
		if(iter != acceptDescriptors_.end() && iter->second == this)
			acceptDescriptors_.erase(iter);

		Components::getSingleton().pNetworkInterface()->dispatcher().deregisterReadFileDescriptor(fd_);
	}
	else if(write_)
		Components::getSingleton().pNetworkInterface()->dispatcher().deregisterWriteFileDescriptor(fd_);
	else
		Components::getSingleton().pNetworkInterface()->dispatcher().deregisterReadFileDescriptor(fd_);
}

//-------------------------------------------------------------------------------------
PyObject* PyFileDescriptor::__py_registerReadFileDescriptor(PyObject* self, PyObject* args)
{
	// readiness 只通知“可读”，无法表达 completion 后端已经完成的 accept 或 recv 结果。
	// Readiness only reports "readable" and cannot represent accept or recv results already completed by the backend.
	const char* error = "KBEngine::registerReadFileDescriptor: deprecated readiness API, use registerAcceptFileDescriptor(fd, onAccept) for listeners or registerReadDataFileDescriptor(fd, onRead) for connected sockets!";
	ERROR_MSG(fmt::format("{}\n", error));
	PyErr_SetString(PyExc_RuntimeError, error);
	return NULL;
}

//-------------------------------------------------------------------------------------
PyObject* PyFileDescriptor::__py_deregisterReadFileDescriptor(PyObject* self, PyObject* args)
{
	// 新读 API 按 listener 与连接区分所有权，旧注销入口不能安全判断应删除哪一种对象。
	// New read APIs separate listener and connection ownership, so the legacy deregistration entry cannot safely choose an object type.
	const char* error = "KBEngine::deregisterReadFileDescriptor: deprecated readiness API, use deregisterAcceptFileDescriptor(fd) for listeners or deregisterReadDataFileDescriptor(fd) for connected sockets!";
	ERROR_MSG(fmt::format("{}\n", error));
	PyErr_SetString(PyExc_RuntimeError, error);
	return NULL;
}

//-------------------------------------------------------------------------------------
PyObject* PyFileDescriptor::__py_registerReadDataFileDescriptor(PyObject* self, PyObject* args)
{
	if(PyTuple_Size(args) != 2)
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::registerReadDataFileDescriptor: args != (fileDescriptor, callback)!");
		PyErr_PrintEx(0);
		return NULL;
	}

	PyObject* pycallback = NULL;
	int fd = 0;
	if(!PyArg_ParseTuple(args, "iO", &fd, &pycallback))
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::registerReadDataFileDescriptor: args error!");
		PyErr_PrintEx(0);
		return NULL;
	}

	if(fd <= 0)
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::registerReadDataFileDescriptor: fd <= 0!");
		PyErr_PrintEx(0);
		return NULL;
	}

	if(!PyCallable_Check(pycallback))
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::registerReadDataFileDescriptor: invalid pycallback!");
		PyErr_PrintEx(0);
		return NULL;
	}

	Network::EventPoller* pPoller = Components::getSingleton().pNetworkInterface()->dispatcher().pPoller();
	if(pPoller == NULL || !pPoller->supportsCompletion())
	{
		// 数据回调必须消费 completion handoff 队列，就绪后端无法提供相同语义。
		// Data callbacks must consume the completion handoff queue; readiness backends cannot provide equivalent semantics.
		PyErr_Format(PyExc_RuntimeError, "KBEngine::registerReadDataFileDescriptor: current poller does not support completion IO!");
		PyErr_PrintEx(0);
		return NULL;
	}

	if(pPoller->findForRead(fd) != NULL)
	{
		// 每个 fd 只能有一个读侧消费者，否则 accept 与 recv 队列的归属会产生歧义。
		// Each fd may have only one read-side consumer, otherwise ownership of accept and recv queues becomes ambiguous.
		PyErr_Format(PyExc_RuntimeError, "KBEngine::registerReadDataFileDescriptor: fd already registered for read!");
		PyErr_PrintEx(0);
		return NULL;
	}

	new PyFileDescriptor(fd, pycallback, false, 0);
	S_Return;
}

//-------------------------------------------------------------------------------------
PyObject* PyFileDescriptor::__py_deregisterReadDataFileDescriptor(PyObject* self, PyObject* args)
{
	if(PyTuple_Size(args) != 1)
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::deregisterReadDataFileDescriptor: args != (fileDescriptor)!");
		PyErr_PrintEx(0);
		return NULL;
	}

	int fd = 0;
	if(!PyArg_ParseTuple(args, "i", &fd))
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::deregisterReadDataFileDescriptor: args error!");
		PyErr_PrintEx(0);
		return NULL;
	}

	if(fd <= 0)
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::deregisterReadDataFileDescriptor: fd <= 0!");
		PyErr_PrintEx(0);
		return NULL;
	}

	std::map<int, PyFileDescriptor*>::iterator iter = readDataDescriptors_.find(fd);
	if(iter != readDataDescriptors_.end())
		delete iter->second;

	S_Return;
}

//-------------------------------------------------------------------------------------
PyObject* PyFileDescriptor::__py_registerAcceptFileDescriptor(PyObject* self, PyObject* args)
{
	if(PyTuple_Size(args) != 2)
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::registerAcceptFileDescriptor: args != (fileDescriptor, callback)!");
		PyErr_PrintEx(0);
		return NULL;
	}

	PyObject* pycallback = NULL;
	int fd = 0;
	if(!PyArg_ParseTuple(args, "iO", &fd, &pycallback))
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::registerAcceptFileDescriptor: args error!");
		PyErr_PrintEx(0);
		return NULL;
	}

	if(fd <= 0)
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::registerAcceptFileDescriptor: fd <= 0!");
		PyErr_PrintEx(0);
		return NULL;
	}

	if(!PyCallable_Check(pycallback))
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::registerAcceptFileDescriptor: invalid pycallback!");
		PyErr_PrintEx(0);
		return NULL;
	}

	Network::EventPoller* pPoller = Components::getSingleton().pNetworkInterface()->dispatcher().pPoller();
	if(pPoller == NULL || !pPoller->supportsCompletion())
	{
		PyErr_Format(PyExc_RuntimeError, "KBEngine::registerAcceptFileDescriptor: current poller does not support completion IO!");
		PyErr_PrintEx(0);
		return NULL;
	}

	if(pPoller->findForRead(fd) != NULL)
	{
		// listener 与连接数据都复用 dispatcher 的读槽，重复注册必须在创建包装对象前拒绝。
		// Listeners and connection data share the dispatcher's read slot, so duplicates must be rejected before allocation.
		PyErr_Format(PyExc_RuntimeError, "KBEngine::registerAcceptFileDescriptor: fd already registered for read!");
		PyErr_PrintEx(0);
		return NULL;
	}

	new PyFileDescriptor(fd, pycallback, true, 0);
	S_Return;
}

//-------------------------------------------------------------------------------------
PyObject* PyFileDescriptor::__py_deregisterAcceptFileDescriptor(PyObject* self, PyObject* args)
{
	if(PyTuple_Size(args) != 1)
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::deregisterAcceptFileDescriptor: args != (fileDescriptor)!");
		PyErr_PrintEx(0);
		return NULL;
	}

	int fd = 0;
	if(!PyArg_ParseTuple(args, "i", &fd))
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::deregisterAcceptFileDescriptor: args error!");
		PyErr_PrintEx(0);
		return NULL;
	}

	if(fd <= 0)
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::deregisterAcceptFileDescriptor: fd <= 0!");
		PyErr_PrintEx(0);
		return NULL;
	}

	std::map<int, PyFileDescriptor*>::iterator iter = acceptDescriptors_.find(fd);
	if(iter != acceptDescriptors_.end())
		delete iter->second;

	S_Return;
}

//-------------------------------------------------------------------------------------
PyObject* PyFileDescriptor::__py_registerWriteFileDescriptor(PyObject* self, PyObject* args)
{
	// completion 发送按每次请求返回完成结果，不再暴露“当前可写”的瞬时状态。
	// Completion sends report each request result instead of exposing the transient "currently writable" state.
	const char* error = "KBEngine::registerWriteFileDescriptor: deprecated readiness API, use writeFileDescriptor(fd, data, onWriteComplete) completion API instead!";
	ERROR_MSG(fmt::format("{}\n", error));
	PyErr_SetString(PyExc_RuntimeError, error);
	return NULL;
}

//-------------------------------------------------------------------------------------
PyObject* PyFileDescriptor::__py_deregisterWriteFileDescriptor(PyObject* self, PyObject* args)
{
	// writeFileDescriptor 创建的短生命周期处理器会在请求完成后自行注销，不提供独立注销入口。
	// Short-lived handlers created by writeFileDescriptor deregister themselves after completion and have no separate deregistration entry.
	const char* error = "KBEngine::deregisterWriteFileDescriptor: deprecated readiness API, use writeFileDescriptor(fd, data, onWriteComplete) completion API instead!";
	ERROR_MSG(fmt::format("{}\n", error));
	PyErr_SetString(PyExc_RuntimeError, error);
	return NULL;
}

//-------------------------------------------------------------------------------------
PyObject* PyFileDescriptor::__py_writeFileDescriptor(PyObject* self, PyObject* args)
{
	if(PyTuple_Size(args) != 3)
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::writeFileDescriptor: args != (fileDescriptor, data, callback)!");
		PyErr_PrintEx(0);
		return NULL;
	}

	int fd = 0;
	PyObject* pydata = NULL;
	PyObject* pycallback = NULL;
	if(!PyArg_ParseTuple(args, "iOO", &fd, &pydata, &pycallback))
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::writeFileDescriptor: args error!");
		PyErr_PrintEx(0);
		return NULL;
	}

	if(fd <= 0)
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::writeFileDescriptor: fd <= 0!");
		PyErr_PrintEx(0);
		return NULL;
	}

	if(!PyBytes_Check(pydata))
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::writeFileDescriptor: data must be bytes!");
		PyErr_PrintEx(0);
		return NULL;
	}

	if(!PyCallable_Check(pycallback))
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::writeFileDescriptor: invalid pycallback!");
		PyErr_PrintEx(0);
		return NULL;
	}

	Network::EventPoller* pPoller = Components::getSingleton().pNetworkInterface()->dispatcher().pPoller();
	if(pPoller == NULL || !pPoller->supportsCompletion())
	{
		PyErr_Format(PyExc_RuntimeError, "KBEngine::writeFileDescriptor: current poller does not support completion IO!");
		PyErr_PrintEx(0);
		return NULL;
	}

	PyFileDescriptor* pPyFileDescriptor = NULL;
	std::map<int, PyFileDescriptor*>::iterator iter = writeCompletionDescriptors_.find(fd);
	if(iter != writeCompletionDescriptors_.end())
	{
		pPyFileDescriptor = iter->second;
	}
	else
	{
		if(pPoller->findForWrite(fd) != NULL)
		{
			PyErr_Format(PyExc_RuntimeError, "KBEngine::writeFileDescriptor: fd already registered for write!");
			PyErr_PrintEx(0);
			return NULL;
		}

		// 写包装对象按 fd 延迟创建，并在全部排队请求完成后自动释放。
		// The write wrapper is created lazily per fd and releases itself after all queued requests complete.
		pPyFileDescriptor = new PyFileDescriptor(fd, NULL, true);
		pPyFileDescriptor->mode_ = MODE_WRITE_COMPLETION;
		writeCompletionDescriptors_[fd] = pPyFileDescriptor;
	}

	if(!pPyFileDescriptor->enqueueWrite(pydata, pycallback))
	{
		if(pPyFileDescriptor->writeRequests_.empty())
			delete pPyFileDescriptor;

		return NULL;
	}

	if(pPyFileDescriptor->writeRequests_.empty())
	{
		// 零长度或同步失败的请求已经完成回调，无需留下空闲写处理器。
		// Zero-length or synchronously failed requests have already called back, so no idle write handler is retained.
		delete pPyFileDescriptor;
	}

	S_Return;
}

//-------------------------------------------------------------------------------------
int PyFileDescriptor::handleInputNotification(int fd)
{
	//INFO_MSG(fmt::format("PyFileDescriptor:handleInputNotification: fd = {}\n",
	//			fd));

	if(mode_ == MODE_ACCEPT)
		callbackAccept();
	else if(mode_ == MODE_READ_DATA)
		callbackReadData();
	else
		callback();

	return 0;
}

//-------------------------------------------------------------------------------------
int PyFileDescriptor::handleOutputNotification( int fd )
{
	//INFO_MSG(fmt::format("PyFileDescriptor:handleOutputNotification: fd = {}\n",
	//			fd));

	if(mode_ == MODE_WRITE_COMPLETION)
	{
		// 处理固定快照允许完成回调重入 writeFileDescriptor，新请求留到下一次底层完成再通知。
		// Processing a fixed snapshot allows callbacks to re-enter writeFileDescriptor while new requests wait for the next completion.
		size_t completedRequests = writeRequests_.size();
		while(completedRequests-- > 0 && !writeRequests_.empty())
		{
			WriteRequest request = writeRequests_.front();
			writeRequests_.pop_front();
			pyCallback_ = request.pyCallback;
			callbackWriteComplete(request.bytes, 0);
			pyCallback_ = NULL;
		}

		if(writeRequests_.empty())
			delete this;

		return 0;
	}

	callback();
	return 0;
}

//-------------------------------------------------------------------------------------
void PyFileDescriptor::callback()
{
	if(pyCallback_ != NULL)
	{
		PyObject* pyResult = PyObject_CallFunction(pyCallback_.get(), 
											const_cast<char*>("i"), 
											fd_);

		if(pyResult != NULL)
			Py_DECREF(pyResult);
		else
			SCRIPT_ERROR_CHECK();
	}
	else
	{
		ERROR_MSG(fmt::format("PyFileDescriptor::callback: not found callback:{}.\n", fd_));
	}
}

//-------------------------------------------------------------------------------------
void PyFileDescriptor::callbackAccept()
{
	Network::EventPoller* pPoller = Components::getSingleton().pNetworkInterface()->dispatcher().pPoller();
	if(pPoller == NULL)
		return;

	// 一次唤醒可能覆盖多个已完成连接，必须排空队列以免连接滞留到下一次事件。
	// One wakeup may cover several completed connections, so drain the queue to avoid deferring sockets until another event.
	KBESOCKET acceptedSocket = 0;
	while(pPoller->takeAcceptedSocket(fd_, acceptedSocket))
	{
		if(pyCallback_ != NULL)
		{
			PyObject* pyResult = PyObject_CallFunction(pyCallback_.get(),
											const_cast<char*>("iii"),
											fd_,
											static_cast<int>(acceptedSocket),
											0);

			if(pyResult != NULL)
				Py_DECREF(pyResult);
			else
				SCRIPT_ERROR_CHECK();
		}
		else
		{
			ERROR_MSG(fmt::format("PyFileDescriptor::callbackAccept: not found callback:{}.\n", fd_));
		}
	}
}

//-------------------------------------------------------------------------------------
void PyFileDescriptor::callbackReadData()
{
	Network::EventPoller* pPoller = Components::getSingleton().pNetworkInterface()->dispatcher().pPoller();
	if(pPoller == NULL)
		return;

	// completion 队列保留数据顺序和终止状态；脚本收到 bytes 后不能再次直接 recv 同一 fd。
	// The completion queue preserves data order and terminal state; scripts must not recv the same fd again after receiving bytes.
	std::vector<char> data;
	bool disconnected = false;
	int errorCode = 0;
	while(pPoller->takeTcpReceivedData(fd_, data, disconnected, errorCode))
	{
		if(pyCallback_ != NULL)
		{
			PyObject* pyData = PyBytes_FromStringAndSize(data.empty() ? "" : data.data(), data.size());
			PyObject* pyResult = PyObject_CallFunction(pyCallback_.get(),
											const_cast<char*>("iOi"),
											fd_,
											pyData,
											errorCode);
			Py_XDECREF(pyData);

			if(pyResult != NULL)
				Py_DECREF(pyResult);
			else
				SCRIPT_ERROR_CHECK();
		}
		else
		{
			ERROR_MSG(fmt::format("PyFileDescriptor::callbackReadData: not found callback:{}.\n", fd_));
		}

		// EOF 或错误结束读生命周期，本轮不能继续分发后续缓存数据。
		// EOF or an error terminates the read lifecycle, so no later buffered data may be dispatched in this pass.
		if(disconnected || errorCode != 0)
			break;
	}
}

//-------------------------------------------------------------------------------------
void PyFileDescriptor::callbackWriteComplete(int bytesWritten, int errorCode)
{
	if(pyCallback_ != NULL)
	{
		PyObject* pyResult = PyObject_CallFunction(pyCallback_.get(),
										const_cast<char*>("iii"),
										fd_,
										bytesWritten,
										errorCode);

		if(pyResult != NULL)
			Py_DECREF(pyResult);
		else
			SCRIPT_ERROR_CHECK();
	}
	else
	{
		ERROR_MSG(fmt::format("PyFileDescriptor::callbackWriteComplete: not found callback:{}.\n", fd_));
	}
}

//-------------------------------------------------------------------------------------
bool PyFileDescriptor::enqueueWrite(PyObject* pyData, PyObject* pyCallback)
{
	char* buffer = NULL;
	Py_ssize_t length = 0;
	if(PyBytes_AsStringAndSize(pyData, &buffer, &length) < 0)
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::writeFileDescriptor: data error!");
		PyErr_PrintEx(0);
		return false;
	}

	if(length == 0)
	{
		// 空写没有底层操作，仍以成功完成回调保持 API 的每次提交一次通知约定。
		// An empty write has no backend operation but still reports success to preserve one completion per submission.
		pyCallback_ = pyCallback;
		callbackWriteComplete(0, 0);
		pyCallback_ = NULL;
		return true;
	}

	Network::EventPoller* pPoller = Components::getSingleton().pNetworkInterface()->dispatcher().pPoller();
	if(pPoller == NULL || !pPoller->queueTcpSend(fd_, buffer, static_cast<int>(length)))
	{
		// 入队失败通过完成回调返回，业务脚本可以用统一的异步错误路径清理连接。
		// Queue failures are returned through the completion callback so scripts can use one asynchronous cleanup path.
		int errorCode = lastErrorCode();
		pyCallback_ = pyCallback;
		callbackWriteComplete(0, errorCode);
		pyCallback_ = NULL;
		return true;
	}

	// poller 已复制字节数据，这里只需保存长度与回调，不延长 Python bytes 生命周期。
	// The poller has copied the bytes, so only the length and callback are retained without extending the Python bytes lifetime.
	writeRequests_.push_back(WriteRequest(static_cast<int>(length), pyCallback));
	return true;
}

//-------------------------------------------------------------------------------------
int PyFileDescriptor::lastErrorCode()
{
#if KBE_PLATFORM == PLATFORM_WIN32
	return WSAGetLastError();
#else
	return errno;
#endif
}

//-------------------------------------------------------------------------------------
}
