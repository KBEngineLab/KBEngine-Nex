// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

#ifndef KBE_ASYNCIO_HELPER_HANDLER_H
#define KBE_ASYNCIO_HELPER_HANDLER_H

#include "common/common.h"
#include "pyscript/kbe_python.h"

namespace KBEngine {

	namespace Network
	{
		class EventDispatcher;
	}

	class AsyncioHelper
	{
	public:

		/**
		 * 提交一个协程到C++侧持有的主线程asyncio loop中执行。
		 * 检查传入对象是否实现__await__。
		 * 用loop.create_task把awaitable转换为Task。
		 * 保存Task强引用，等待timer驱动和完成回收。
		 *
		 * @param pyObject: 要提交的Python对象，必须是一个可等待的协程。
		 * @return 返回NULL，因为submitCoroutine函数是异步的，暂时不处理返回结果。
		 */
		static PyObject* submitCoroutine(PyObject* pyObject);

		/**
		 * 安装一个主线程timer，用来非阻塞推进asyncio事件循环。
		 * 按asyncioRepeatOffset配置决定是否启用。
		 * 初始化asyncio event loop。
		 * 注册底层EventDispatcher timer，避免被gameUpdateHertz量化。
		 */
		static bool installTimer(Network::EventDispatcher& dispatcher);

		/**
		 * 关闭asyncio调度器，取消未完成的任务。
		 * 拒绝新任务。
		 * 取消未完成Task。
		 * pump一次让取消生效。
		 * 关闭并释放event loop。
		 */
		static void shutdown();
	};

}

#endif
