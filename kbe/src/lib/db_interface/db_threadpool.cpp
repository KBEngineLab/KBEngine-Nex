// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com
#include "db_threadpool.h"
#include "db_tasks.h"
#include "thread/threadtask.h"
#include "db_interface/db_interface.h"
#include "thread/threadpool.h"
#include "thread/threadguard.h"

namespace KBEngine{

class DBThread : public thread::TPThread
{
public:
	DBThread(const std::string& dbinterfaceName, thread::ThreadPool* threadPool, int threadWaitSecond = 0) :
	thread::TPThread(threadPool, threadWaitSecond),
	_pDBInterface(NULL),
	dbinterfaceName_(dbinterfaceName)
	{
	}

	virtual void onStart()
	{
		DBUtil::initThread(dbinterfaceName_);
		_pDBInterface = DBUtil::createInterface(dbinterfaceName_.c_str(), false);
		if(_pDBInterface == NULL)
		{
			ERROR_MSG("DBThread:: can't create dbinterface!\n");
		}

		DEBUG_MSG(fmt::format("DBThread::onStart(): {0:p}!\n", (void*)this));
	}

	virtual void onEnd()
	{
		if(_pDBInterface)
		{
			_pDBInterface->detach();
			SAFE_RELEASE(_pDBInterface);
			DBUtil::finiThread(dbinterfaceName_);
		}

		DEBUG_MSG(fmt::format("DBThread::onEnd(): {0:p}!\n", (void*)this));
	}

	~DBThread()
	{
	}
	
	virtual thread::TPTask* tryGetTask(void)
	{
		if (task())
		{
			DBTaskBase* pDBTask = static_cast<DBTaskBase*>(task())->tryGetNextTask();
			if (pDBTask != NULL)
			{
				return pDBTask;
			}
		}

		return thread::TPThread::tryGetTask();
	}

	virtual void onProcessTaskStart(thread::TPTask* pTask)
	{
		static_cast<DBTaskBase*>(pTask)->pdbi(_pDBInterface);
	}

	virtual void processTask(thread::TPTask* pTask)
	{
		DBTaskBase* pDBTask = static_cast<DBTaskBase*>(pTask);
		if (!_pDBInterface)
		{
			const std::string error = "database interface is null";
			ERROR_MSG(fmt::format("DBThread::processTask: {}, task={}.\n", error, pDBTask->name()));
			pDBTask->onDatabaseFailure(error);
			return;
		}

		enum ProcessPhase
		{
			PROCESS_PHASE_START,
			PROCESS_PHASE_EXECUTE,
			PROCESS_PHASE_COMMIT
		};

		const uint32 maxRetryCount = 3;
		const uint32 retryDelays[] = { 10, 30, 100 };

		for (uint32 retryCount = 0; ; ++retryCount)
		{
			if (retryCount > 0)
				KBEngine::sleep(retryDelays[std::min<uint32>(retryCount - 1, 2)]);

			ProcessPhase phase = PROCESS_PHASE_START;
			bool transactionStarted = false;

			try
			{
				if (!_pDBInterface->lock())
				{
					const std::string error = _pDBInterface->getstrerror();
					ERROR_MSG(fmt::format("DBThread::processTask: start transaction failed, task={}, db={}, error={}.\n",
						pDBTask->name(), dbinterfaceName_, error));
					pDBTask->onDatabaseFailure(error);
					return;
				}

				transactionStarted = true;
				phase = PROCESS_PHASE_EXECUTE;

				// 事务开始后再恢复任务，避免 BEGIN 失败时丢失上一轮的回调信息。
				if (retryCount > 0)
					pDBTask->resetForRetry();

				thread::TPThread::processTask(pTask);

				phase = PROCESS_PHASE_COMMIT;
				if (!_pDBInterface->unlock())
				{
					const std::string commitError = _pDBInterface->getstrerror();
					_pDBInterface->rollback();
					ERROR_MSG(fmt::format("DBThread::processTask: commit failed, result is unknown, task={}, db={}, error={}.\n",
						pDBTask->name(), dbinterfaceName_, commitError));
					pDBTask->onDatabaseFailure(commitError);
					return;
				}

				return;
			}
			catch (std::exception& e)
			{
				if (transactionStarted)
					_pDBInterface->rollback();

				bool retry = false;
				try
				{
					retry = _pDBInterface->processException(e);
				}
				catch (std::exception& processException)
				{
					ERROR_MSG(fmt::format("DBThread::processTask: process exception failed, task={}, error={}.\n",
						pDBTask->name(), processException.what()));
				}
				catch (...)
				{
					ERROR_MSG(fmt::format("DBThread::processTask: process exception failed with unknown error, task={}.\n",
						pDBTask->name()));
				}

				// 提交阶段失败时无法确定服务端是否已经提交，不能自动重放写任务。
				if (phase == PROCESS_PHASE_COMMIT)
				{
					ERROR_MSG(fmt::format("DBThread::processTask: commit exception, result is unknown, task={}, db={}, error={}.\n",
						pDBTask->name(), dbinterfaceName_, e.what()));
					pDBTask->onDatabaseFailure(e.what());
					return;
				}

				if (!retry || retryCount >= maxRetryCount)
				{
					ERROR_MSG(fmt::format("DBThread::processTask: task failed, task={}, db={}, retries={}, error={}.\n",
						pDBTask->name(), dbinterfaceName_, retryCount, e.what()));
					pDBTask->onDatabaseFailure(e.what());
					return;
				}

				WARNING_MSG(fmt::format("DBThread::processTask: retry task, task={}, db={}, retry={}.\n",
					pDBTask->name(), dbinterfaceName_, retryCount + 1));
			}
			catch (...)
			{
				if (transactionStarted)
					_pDBInterface->rollback();

				ERROR_MSG(fmt::format("DBThread::processTask: task failed with unknown exception, task={}, db={}.\n",
					pDBTask->name(), dbinterfaceName_));
				pDBTask->onDatabaseFailure("unknown database task exception");
				return;
			}
		}
	}

	virtual void onProcessTaskEnd(thread::TPTask* pTask)
	{
	}

private:
	DBInterface* _pDBInterface;
	std::string dbinterfaceName_;
};

//-------------------------------------------------------------------------------------
DBThreadPool::DBThreadPool(const std::string& dbinterfaceName) :
thread::ThreadPool(),
dbinterfaceName_(dbinterfaceName)
{
}

//-------------------------------------------------------------------------------------
DBThreadPool::~DBThreadPool()
{
}

//-------------------------------------------------------------------------------------
thread::TPThread* DBThreadPool::createThread(int threadWaitSecond, bool threadStartsImmediately)
{
	DBThread* tptd = new DBThread(dbinterfaceName_, this, threadWaitSecond);

	if (threadStartsImmediately)
		tptd->createThread();

	return tptd;
}	

//-------------------------------------------------------------------------------------
}
