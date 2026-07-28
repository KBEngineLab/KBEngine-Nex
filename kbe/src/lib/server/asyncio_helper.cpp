#include "asyncio_helper.h"

#include "network/event_dispatcher.h"
#include "pyscript/script.h"
#include "serverconfig.h"

#include <vector>

namespace KBEngine
{

namespace
{
// 模块、event loop 和 Task 都由组件主线程独占，避免引入额外线程和 GIL 竞争。
// The module, event loop, and tasks are owned exclusively by the component main thread to avoid extra threads and GIL contention.
PyObject* g_asyncioModule = NULL;
PyObject* g_loop = NULL;
std::vector<PyObject*> g_tasks;

// 关闭阶段拒绝新任务，防止脚本析构回调重新填充正在关闭的 loop。
// New tasks are rejected during shutdown so script finalizers cannot refill a loop that is being closed.
bool g_shuttingDown = false;

// 直接使用底层 dispatcher timer，避免调度精度被 gameUpdateHertz 限制。
// A dispatcher timer is used directly so scheduling precision is not quantized by gameUpdateHertz.
TimerHandle g_timerHandle;

// 单次 tick 同时受迭代次数和时间预算限制，避免大量 ready callback 长时间阻塞组件主循环。
// Each tick is bounded by both iteration count and elapsed time so a large ready queue cannot monopolize the component loop.
const int ASYNCIO_MAX_PUMP_ITERATIONS = 64;
const int ASYNCIO_MIN_PUMP_ITERATIONS = 8;
const uint64 ASYNCIO_MAX_PUMP_MILLISECONDS = 2;

void closeAwaitable(PyObject* pyObject)
{
	// coroutine 在未调度时必须显式 close，否则 Python 会在回收时产生未等待警告。
	// A coroutine rejected before scheduling must be closed explicitly or Python will report that it was never awaited.
	PyObject* pyRet = PyObject_CallMethod(pyObject, const_cast<char*>("close"), const_cast<char*>(""));
	if (pyRet == NULL)
		PyErr_Clear();
	else
		Py_DECREF(pyRet);
}

bool ensureLoop()
{
	if (g_loop != NULL)
		return true;

	g_asyncioModule = PyImport_ImportModule("asyncio");
	if (g_asyncioModule == NULL)
	{
		SCRIPT_ERROR_CHECK();
		return false;
	}

	// 使用独立 loop 而不是隐式全局 loop，确保其生命周期严格跟随当前 KBE 组件。
	// Use a dedicated loop rather than an implicit global loop so its lifetime follows the current KBE component exactly.
	g_loop = PyObject_CallMethod(g_asyncioModule, const_cast<char*>("new_event_loop"), const_cast<char*>(""));
	if (g_loop == NULL)
	{
		SCRIPT_ERROR_CHECK();
		Py_CLEAR(g_asyncioModule);
		return false;
	}

	PyObject* pyRet = PyObject_CallMethod(
		g_asyncioModule, const_cast<char*>("set_event_loop"), const_cast<char*>("O"), g_loop);
	if (pyRet == NULL)
	{
		SCRIPT_ERROR_CHECK();
		Py_CLEAR(g_loop);
		Py_CLEAR(g_asyncioModule);
		return false;
	}

	Py_DECREF(pyRet);
	g_shuttingDown = false;
	return true;
}

Py_ssize_t readySize()
{
	// _ready 是 asyncio 的内部队列，只用于决定是否继续当前非阻塞 pump，不参与业务逻辑。
	// _ready is an asyncio internal queue used only to decide whether this non-blocking pump should continue.
	PyObject* pyReady = PyObject_GetAttrString(g_loop, "_ready");
	if (pyReady == NULL)
	{
		PyErr_Clear();
		return 0;
	}

	Py_ssize_t size = PyObject_Length(pyReady);
	Py_DECREF(pyReady);
	if (size < 0)
	{
		PyErr_Clear();
		return 0;
	}

	return size;
}

void collectDoneTasks()
{
	std::vector<PyObject*>::iterator iter = g_tasks.begin();
	while (iter != g_tasks.end())
	{
		PyObject* task = *iter;
		PyObject* pyDone = PyObject_CallMethod(task, const_cast<char*>("done"), const_cast<char*>(""));
		if (pyDone == NULL)
		{
			SCRIPT_ERROR_CHECK();
			Py_DECREF(task);
			iter = g_tasks.erase(iter);
			continue;
		}

		const int doneState = PyObject_IsTrue(pyDone);
		Py_DECREF(pyDone);
		if (doneState < 0)
		{
			SCRIPT_ERROR_CHECK();
			Py_DECREF(task);
			iter = g_tasks.erase(iter);
			continue;
		}

		if (doneState == 0)
		{
			++iter;
			continue;
		}

		PyObject* pyCancelled = PyObject_CallMethod(task, const_cast<char*>("cancelled"), const_cast<char*>(""));
		if (pyCancelled == NULL)
		{
			SCRIPT_ERROR_CHECK();
		}
		else
		{
			const int cancelledState = PyObject_IsTrue(pyCancelled);
			Py_DECREF(pyCancelled);
			if (cancelledState < 0)
			{
				SCRIPT_ERROR_CHECK();
			}
			else if (cancelledState == 0)
			{
				// 主动读取 result，使协程异常进入 KBE 日志，而不是延迟到 Task 析构时才报警。
				// Read result eagerly so coroutine exceptions reach KBE logs instead of surfacing later during Task destruction.
				PyObject* pyRet = PyObject_CallMethod(task, const_cast<char*>("result"), const_cast<char*>(""));
				if (pyRet == NULL)
				{
					SCRIPT_ERROR_CHECK();
				}
				else
				{
					Py_DECREF(pyRet);
				}
			}
		}

		Py_DECREF(task);
		iter = g_tasks.erase(iter);
	}
}

bool pumpLoopOnce()
{
	// 先安排 stop 再运行 loop，可只处理当前 ready 队列而不阻塞 KBE 主线程等待 IO。
	// Scheduling stop before running the loop processes the current ready queue without blocking the KBE main thread for IO.
	PyObject* stopFunc = PyObject_GetAttrString(g_loop, "stop");
	if (stopFunc == NULL)
	{
		SCRIPT_ERROR_CHECK();
		return false;
	}

	PyObject* pyRet = PyObject_CallMethod(
		g_loop, const_cast<char*>("call_soon"), const_cast<char*>("O"), stopFunc);
	Py_DECREF(stopFunc);
	if (pyRet == NULL)
	{
		SCRIPT_ERROR_CHECK();
		return false;
	}

	Py_DECREF(pyRet);
	pyRet = PyObject_CallMethod(g_loop, const_cast<char*>("run_forever"), const_cast<char*>(""));
	if (pyRet == NULL)
	{
		SCRIPT_ERROR_CHECK();
		return false;
	}

	Py_DECREF(pyRet);
	return true;
}

void pumpLoop()
{
	if (!ensureLoop())
		return;

	uint64 maxPumpStamps = stampsPerSecond() * ASYNCIO_MAX_PUMP_MILLISECONDS / 1000;
	if (maxPumpStamps == 0)
		maxPumpStamps = 1;

	const uint64 beginStamps = timestamp();
	for (int i = 0; i < ASYNCIO_MAX_PUMP_ITERATIONS; ++i)
	{
		if (!pumpLoopOnce())
			return;

		collectDoneTasks();
		if (i + 1 < ASYNCIO_MIN_PUMP_ITERATIONS)
			continue;

		if (readySize() <= 0)
			break;

		if (timestamp() - beginStamps >= maxPumpStamps)
			break;
	}

	collectDoneTasks();
}

class AsyncioTimerHandler : public TimerHandler
{
private:
	virtual void handleTimeout(TimerHandle /*handle*/, void* /*pUser*/)
	{
		pumpLoop();
	}

	virtual void onRelease(TimerHandle /*handle*/, void* /*pUser*/)
	{
		g_timerHandle.clearWithoutCancel();
		delete this;
	}
};
}

PyObject* AsyncioHelper::submitCoroutine(PyObject* pyObject)
{
	if (pyObject == NULL)
		return NULL;

	const int isAwaitable = PyObject_HasAttrString(pyObject, "__await__");
	if (isAwaitable < 0)
	{
		SCRIPT_ERROR_CHECK();
		return NULL;
	}

	if (isAwaitable == 0)
		return NULL;

	if (g_shuttingDown || g_kbeSrvConfig.asyncioRepeatOffset() <= 0.f)
	{
		// 禁用状态不能创建无人推进的 Task；关闭 awaitable 后给出可操作的配置提示。
		// Disabled scheduling must not create an unpumped Task; close the awaitable and emit an actionable configuration message.
		closeAwaitable(pyObject);
		if (!g_shuttingDown)
			ERROR_MSG("AsyncioHelper::submitCoroutine: asyncio is disabled, set asyncioRepeatOffset greater than 0.\n");
		return NULL;
	}

	if (!ensureLoop())
	{
		closeAwaitable(pyObject);
		return NULL;
	}

	PyObject* task = PyObject_CallMethod(
		g_loop, const_cast<char*>("create_task"), const_cast<char*>("O"), pyObject);
	if (task == NULL)
	{
		SCRIPT_ERROR_CHECK();
		closeAwaitable(pyObject);
		return NULL;
	}

	// C++ 持有强引用直到任务完成，避免只由 asyncio 弱引用管理的 Task 被提前回收。
	// C++ keeps a strong reference until completion so a Task managed only by asyncio weak references cannot be collected early.
	g_tasks.push_back(task);
	return NULL;
}

bool AsyncioHelper::installTimer(Network::EventDispatcher& dispatcher)
{
	if (g_kbeSrvConfig.asyncioRepeatOffset() <= 0.f)
		return true;

	if (g_timerHandle.isSet())
		return true;

	// 仅在显式安装新调度器时重新开放任务提交，shutdown 后的析构回调仍会被拒绝。
	// Reopen task submission only when a new dispatcher is explicitly installed; finalizer callbacks after shutdown remain rejected.
	g_shuttingDown = false;

	if (!ensureLoop())
		return false;

	AsyncioTimerHandler* handler = new AsyncioTimerHandler();
	const int64 intervalUS = KBE_MAX(
		int64(1000), int64(g_kbeSrvConfig.asyncioRepeatOffset() * 1000000.f));
	g_timerHandle = dispatcher.addTimer(intervalUS, handler);
	if (!g_timerHandle.isSet())
	{
		delete handler;
		ERROR_MSG("AsyncioHelper::installTimer: unable to add asyncio timer.\n");
		return false;
	}

	return true;
}

void AsyncioHelper::shutdown()
{
	if (g_loop == NULL)
		return;

	g_shuttingDown = true;

	// 取消前做一次有预算的非阻塞推进，使刚提交的退出回调能够执行，并让已就绪任务自然完成。
	// Perform one budgeted non-blocking pump before cancellation so newly submitted shutdown callbacks can start and ready tasks can finish naturally.
	pumpLoop();

	if (g_timerHandle.isSet())
		g_timerHandle.cancel();

	for (std::vector<PyObject*>::iterator iter = g_tasks.begin(); iter != g_tasks.end(); ++iter)
	{
		PyObject* pyRet = PyObject_CallMethod(*iter, const_cast<char*>("cancel"), const_cast<char*>(""));
		if (pyRet == NULL)
			PyErr_Clear();
		else
			Py_DECREF(pyRet);
	}

	// 再推进一轮，让 CancelledError 和 finally 块有机会在脚本类型卸载前执行。
	// Pump once more so CancelledError handling and finally blocks can run before script types are unloaded.
	pumpLoop();
	collectDoneTasks();

	for (std::vector<PyObject*>::iterator iter = g_tasks.begin(); iter != g_tasks.end(); ++iter)
		Py_DECREF(*iter);
	g_tasks.clear();

	PyObject* pyRet = PyObject_CallMethod(g_loop, const_cast<char*>("close"), const_cast<char*>(""));
	if (pyRet == NULL)
	{
		SCRIPT_ERROR_CHECK();
	}
	else
	{
		Py_DECREF(pyRet);
	}

	Py_CLEAR(g_loop);
	Py_CLEAR(g_asyncioModule);
}

}
