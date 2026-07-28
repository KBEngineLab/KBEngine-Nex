// Copyright 2008-2018 KBEngine Foundation. All Rights Reserved.

#ifndef KBE_ASYNCIO_HELPER_H
#define KBE_ASYNCIO_HELPER_H

#include "common/common.h"
#include "pyscript/kbe_python.h"

namespace KBEngine
{

namespace Network
{
class EventDispatcher;
}

class AsyncioHelper
{
public:
	/**
	 * 将脚本返回的 awaitable 提交到组件主线程持有的 asyncio event loop。
	 * Submit an awaitable returned by script code to the asyncio event loop owned by the component main thread.
	 *
	 * 普通同步返回值会被忽略；函数始终返回 NULL，因为异步结果不参与当前 C++ 调用链。
	 * Ordinary synchronous results are ignored; the function always returns NULL because asynchronous results do not re-enter the current C++ call chain.
	 */
	static PyObject* submitCoroutine(PyObject* pyObject);

	/**
	 * 按服务器配置安装 EventDispatcher 定时器，并初始化当前组件唯一的 asyncio loop。
	 * Install an EventDispatcher timer from server configuration and initialize the single asyncio loop for this component.
	 */
	static bool installTimer(Network::EventDispatcher& dispatcher);

	/**
	 * 停止接收新任务，取消并回收未完成任务，然后关闭 asyncio loop。
	 * Stop accepting new work, cancel and release pending tasks, and then close the asyncio loop.
	 */
	static void shutdown();
};

}

#endif
