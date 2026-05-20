// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

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
PyFileDescriptor::WriteRequest::WriteRequest() :
	bytes(0),
	pyCallback()
{
}

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
	// 新 completion 读侧仍复用 dispatcher 的“读 handler 表”做脚本回调路由。
	// 区别在于底层 poller 不再把这个 fd 当作“可读通知”交给脚本；
	// IOCP/io_uring/kqueue adapter 会先完成 accept/recv，并把结果放进 completion 队列，
	// 然后 triggerRead(fd)。这里的 handleInputNotification 只负责从队列 take 结果。
	if(accept)
		acceptDescriptors_[fd_] = this;
	else
		readDataDescriptors_[fd_] = this;

	Components::getSingleton().pNetworkInterface()->dispatcher().registerReadFileDescriptor(fd_, this);
}

//-------------------------------------------------------------------------------------
PyFileDescriptor::~PyFileDescriptor()
{
	// 析构即注销底层 fd handler。
	// 这里必须先从脚本层索引 map 中移除，再调用 dispatcher deregister，
	// 防止 deregister 过程中触发迟到 completion 或脚本再次查询时拿到已释放对象。
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
	// readiness 读通知在 completion 模型下语义不成立：
	// - listener fd 应该拿到 accept completion，而不是“listener 可读”；
	// - TCP fd 应该拿到 recv 完成后的 data/error，而不是让脚本再 recv。
	// 因此旧接口保留名称但强制报错，避免旧脚本继续在 completion 后端上误用。
	const char* error = "KBEngine::registerReadFileDescriptor: deprecated readiness API, use registerAcceptFileDescriptor(fd, onAccept) for listeners or registerReadDataFileDescriptor(fd, onRead) for connected sockets!";
	ERROR_MSG(fmt::format("{}\n", error));
	PyErr_Format(PyExc_RuntimeError, error);
	PyErr_PrintEx(0);
	return NULL;
}

//-------------------------------------------------------------------------------------
PyObject* PyFileDescriptor::__py_deregisterReadFileDescriptor(PyObject* self, PyObject* args)
{
	// 与 registerReadFileDescriptor 对称：旧 readiness 注销不再承担任何真实清理。
	// 脚本需要根据 fd 类型选择 deregisterAcceptFileDescriptor 或
	// deregisterReadDataFileDescriptor，避免 listener 与普通连接共用一个注销入口导致误删。
	const char* error = "KBEngine::deregisterReadFileDescriptor: deprecated readiness API, use deregisterAcceptFileDescriptor(fd) for listeners or deregisterReadDataFileDescriptor(fd) for connected sockets!";
	ERROR_MSG(fmt::format("{}\n", error));
	PyErr_Format(PyExc_RuntimeError, error);
	PyErr_PrintEx(0);
	return NULL;
}

//-------------------------------------------------------------------------------------
PyObject* PyFileDescriptor::__py_registerReadDataFileDescriptor(PyObject* self, PyObject* args)
{
	// 注册已连接 TCP fd 的“读完成/data”回调。
	// onRead(fd, data, errorCode) 中的 data 来自 poller 的 completion handoff 队列；
	// 脚本不应该再对 fd 调用 recv，否则会绕过底层 completion 生命周期和队列背压。
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
		// 这些脚本 API 依赖 takeTcpReceivedData/takeAcceptedSocket/queueTcpSend。
		// 如果当前平台或配置不是 completion poller，直接拒绝注册，避免脚本拿到
		// 一个永远不会触发 data completion 的 fd。
		PyErr_Format(PyExc_RuntimeError, "KBEngine::registerReadDataFileDescriptor: current poller does not support completion IO!");
		PyErr_PrintEx(0);
		return NULL;
	}

	if(pPoller->findForRead(fd) != NULL)
	{
		// 一个 fd 的读侧只能有一个语义：accept 或 read data。
		// 同时注册多个读 handler 会让 triggerRead 无法判断应该消费哪个 completion 队列。
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
	// 注销 TCP data completion。
	// delete PyFileDescriptor 会进入析构函数，析构函数负责从 dispatcher 注销读侧 fd，
	// 并让底层 completion poller 清掉还未被脚本消费的 recv handoff 队列。
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
	// 注册 listener fd 的 accept completion。
	// 底层 IOCP/uring/kqueue adapter 完成 accept 后，把 accepted socket 放入队列，
	// 再触发这里的读 handler。脚本回调收到的是 listenerFD/clientFD/errorCode，
	// 不再需要、也不应该在脚本里对 listener 调用 accept()。
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
		// listener 的 accept completion 和普通连接的 read data completion 都占用读侧 handler。
		// 禁止重复注册可以避免 listener fd 被错误地当成 TCP data fd 消费。
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
	// 注销 listener accept completion。
	// 底层 completion poller 会在 deregisterForRead 后丢弃/关闭尚未交给脚本的 accepted socket，
	// 避免停止监听时有 accepted fd 泄漏。
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
	// readiness 写通知在 completion 模型下也不成立。
	// 脚本不再注册“fd 可写”回调，而是每次写入都提交一个 writeFileDescriptor 请求，
	// 底层完成发送后按请求回调 onWriteComplete(fd, bytesWritten, errorCode)。
	const char* error = "KBEngine::registerWriteFileDescriptor: deprecated readiness API, use writeFileDescriptor(fd, data, onWriteComplete) completion API instead!";
	ERROR_MSG(fmt::format("{}\n", error));
	PyErr_Format(PyExc_RuntimeError, error);
	PyErr_PrintEx(0);
	return NULL;
}

//-------------------------------------------------------------------------------------
PyObject* PyFileDescriptor::__py_writeFileDescriptor(PyObject* self, PyObject* args)
{
	// 提交一次 TCP 发送请求。
	// data 必须是 bytes，底层 poller 会复制到自己的发送队列中，因此函数返回后
	// 脚本侧 bytes 生命周期不影响真实异步发送。
	// 每次调用都可以带不同 callback，完成时按照入队顺序回调。
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
		// 第一次对某 fd 写入时，临时注册一个写侧 handler。
		// completion poller 在所有 pending send 清空后 triggerWrite(fd)，
		// 这里再统一消费 writeRequests_ 并回调脚本。
		if(pPoller->findForWrite(fd) != NULL)
		{
			PyErr_Format(PyExc_RuntimeError, "KBEngine::writeFileDescriptor: fd already registered for write!");
			PyErr_PrintEx(0);
			return NULL;
		}

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
		// 零字节写或入队立即失败会同步回调并且不会留下 pending 请求。
		// 此时不需要继续持有写侧 handler，立即释放 wrapper。
		delete pPyFileDescriptor;

	S_Return;
}

//-------------------------------------------------------------------------------------
PyObject* PyFileDescriptor::__py_deregisterWriteFileDescriptor(PyObject* self, PyObject* args)
{
	// 旧 readiness 写注销同样只报错。
	// 新写路径没有单独的显式注销入口：writeFileDescriptor 创建的写 wrapper
	// 会在全部写请求完成并回调脚本后自动 delete this。
	const char* error = "KBEngine::deregisterWriteFileDescriptor: deprecated readiness API, use writeFileDescriptor(fd, data, onWriteComplete) completion API instead!";
	ERROR_MSG(fmt::format("{}\n", error));
	PyErr_Format(PyExc_RuntimeError, error);
	PyErr_PrintEx(0);
	return NULL;
}

//-------------------------------------------------------------------------------------
int PyFileDescriptor::handleInputNotification(int fd)
{
	//INFO_MSG(fmt::format("PyFileDescriptor:handleInputNotification: fd = {}\n",
	//			fd));

	// triggerRead(fd) 在 completion 后端中只表示“某类读侧完成结果已经入队”。
	// 根据注册模式不同，分别从 accepted socket 队列或 TCP received data 队列取结果。
	// 旧 MODE_READ_READY 分支只保留给 C++ 内部兼容对象。
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
		// completion poller 只有在该 fd 当前所有 pending send 都清空后才 triggerWrite。
		// 因此这里可以把本轮已排队的 writeRequests_ 视为已完成请求并逐个回调。
		// 使用 completedRequests 快照是为了允许脚本在 onWriteComplete 内再次 writeFileDescriptor；
		// 新增请求会留在队列中，等待下一次底层 send completion 后再回调，避免递归/重入。
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
			// 写 wrapper 是按需创建的短生命周期对象。
			// 队列为空说明脚本没有在回调中追加新的写请求，可以自动释放并注销写 handler。
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
	// accept completion 消费路径。
	// takeAcceptedSocket 只取底层已经完成的 accept 结果，不做阻塞系统调用。
	// 一个 triggerRead 可能对应多个已经排队的 accepted socket，因此循环 drain。
	Network::EventPoller* pPoller = Components::getSingleton().pNetworkInterface()->dispatcher().pPoller();
	if(pPoller == NULL)
		return;

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
			// 当前脚本 fd API 仍以 int 形式传递 fd；底层 Windows x64 的 SOCKET 是 UINT_PTR，
			// 引擎现有 dispatcher/脚本接口也会把 socket 当 int 使用，因此这里保持一致。
			// 后续若要完整支持大于 int 的 SOCKET，需要整体升级 fd 参数解析与脚本回调格式。

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
	// TCP recv completion 消费路径。
	// completion poller 已经把 recv 到的数据复制到 tcpReceived_ 队列；
	// 这里仅把队列中的 data/error 转为 Python bytes 并回调脚本。
	// disconnected/errorCode 表示读侧生命周期结束，通知一次后停止本轮 drain。
	Network::EventPoller* pPoller = Components::getSingleton().pNetworkInterface()->dispatcher().pPoller();
	if(pPoller == NULL)
		return;

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

		if(disconnected || errorCode != 0)
			break;
	}
}

//-------------------------------------------------------------------------------------
void PyFileDescriptor::callbackWriteComplete(int bytesWritten, int errorCode)
{
	// write completion 回调包装。
	// bytesWritten 表示脚本本次 writeFileDescriptor 提交的数据长度；
	// 当前底层只在 fd 发送队列整体 flush 后触发写回调，所以这里按请求粒度返回提交长度。
	// 如果 queueTcpSend 入队失败，会同步以 bytesWritten=0/errorCode!=0 回调。
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
	// 把脚本 bytes 转交给 completion poller 的 TCP send 队列。
	// queueTcpSend 会复制数据并尝试立即投递底层异步 send；
	// PyFileDescriptor 只保存 callback 和长度，用于完成后按脚本 API 回调。
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
		// 零字节写不需要进入底层发送队列，直接按成功完成回调。
		pyCallback_ = pyCallback;
		callbackWriteComplete(0, 0);
		pyCallback_ = NULL;
		return true;
	}

	Network::EventPoller* pPoller = Components::getSingleton().pNetworkInterface()->dispatcher().pPoller();
	if(pPoller == NULL || !pPoller->queueTcpSend(fd_, buffer, static_cast<int>(length)))
	{
		// 入队失败表示底层发送队列满、fd 无效或 poller 不可用。
		// 这里不抛出 Python 异常中断脚本流程，而是通过 completion 风格的
		// onWriteComplete(fd, 0, errorCode) 把错误交给业务层统一处理。
		int errorCode = lastErrorCode();
		pyCallback_ = pyCallback;
		callbackWriteComplete(0, errorCode);
		pyCallback_ = NULL;
		return true;
	}

	writeRequests_.push_back(WriteRequest(static_cast<int>(length), pyCallback));
	return true;
}

//-------------------------------------------------------------------------------------
int PyFileDescriptor::lastErrorCode()
{
	// 不同平台的 socket 错误来源不同，封装成统一 errorCode 交给脚本回调。
#if KBE_PLATFORM == PLATFORM_WIN32
	return WSAGetLastError();
#else
	return errno;
#endif
}

//-------------------------------------------------------------------------------------
}
