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

#include "db_tasks.h"
#include "db_interface.h"
#include "entity_table.h"
#include "thread/threadpool.h"
#include "common/memorystream.h"
#include <atomic>

namespace KBEngine{

namespace
{
std::atomic<uint64> g_lastDbQueueWarning(0);
std::atomic<uint64> g_suppressedDbQueueWarnings(0);
std::atomic<uint64> g_lastDbExecutionWarning(0);
std::atomic<uint64> g_suppressedDbExecutionWarnings(0);

bool claimSlowTaskWarning(std::atomic<uint64>& lastWarning, std::atomic<uint64>& suppressed,
	uint64 now, uint64& suppressedCount)
{
	uint64 previous = lastWarning.load(std::memory_order_relaxed);
	while (previous == 0 || now - previous >= stampsPerSecond())
	{
		if (lastWarning.compare_exchange_weak(previous, now, std::memory_order_relaxed))
		{
			suppressedCount = suppressed.exchange(0, std::memory_order_relaxed);
			return true;
		}
	}

	suppressed.fetch_add(1, std::memory_order_relaxed);
	return false;
}
}

//-------------------------------------------------------------------------------------
bool DBTaskBase::process()
{
	uint64 startTime = timestamp();
	
	bool ret = db_thread_process();

	uint64 duration = startTime - initTime_;
	if(duration > stampsPerSecond())
	{
		uint64 suppressedCount = 0;
		if (claimSlowTaskWarning(g_lastDbQueueWarning, g_suppressedDbQueueWarnings,
			startTime, suppressedCount))
		{
			// 完整 SQL 可能包含账号凭据且会让唯一语句绕过日志聚合；只保留定位容量问题所需的时延、长度和抑制数。
			// Full SQL may contain credentials and defeats duplicate-log aggregation; retain only latency, size, and suppression count.
			WARNING_MSG(fmt::format("DBTask::process(): queue delay {:.2f}s, queryBytes={}, suppressed={}, check database connection capacity.\n",
				(double(duration) / stampsPerSecondD()), pdbi_->lastquery().size(), suppressedCount));
		}
	}

	duration = timestamp() - startTime;
	if (duration > stampsPerSecond() * 0.2f)
	{
		uint64 suppressedCount = 0;
		const uint64 now = timestamp();
		if (claimSlowTaskWarning(g_lastDbExecutionWarning, g_suppressedDbExecutionWarnings,
			now, suppressedCount))
		{
			WARNING_MSG(fmt::format("DBTask::process(): execution took {:.2f}s, queryBytes={}, suppressed={}.\n",
				(double(duration) / stampsPerSecondD()), pdbi_->lastquery().size(), suppressedCount));
		}
	}

	return ret;
}

//-------------------------------------------------------------------------------------
thread::TPTask::TPTaskState DBTaskBase::presentMainThread()
{
	if (!transactionCommitted())
	{
		ERROR_MSG(fmt::format("DBTaskBase::presentMainThread: transaction did not commit, result={}, task={:p}.\n",
			dbTransactionResultName(transactionResult_), (void*)this));
	}

	return thread::TPTask::TPTASK_STATE_COMPLETED; 
}

//-------------------------------------------------------------------------------------
DBTaskSyncTable::DBTaskSyncTable(EntityTables* pEntityTables, KBEShared_ptr<EntityTable> pEntityTable) :
pEntityTable_(pEntityTable),
success_(false),
pEntityTables_(pEntityTables)
{
}

//-------------------------------------------------------------------------------------
DBTaskSyncTable::~DBTaskSyncTable()
{
}

//-------------------------------------------------------------------------------------
bool DBTaskSyncTable::db_thread_process()
{
	success_ = !pEntityTable_->syncToDB(pdbi_);
	return false;
}

//-------------------------------------------------------------------------------------
thread::TPTask::TPTaskState DBTaskSyncTable::presentMainThread()
{
	pEntityTables_->onTableSyncSuccessfully(pEntityTable_, success_ && transactionCommitted());
	return thread::TPTask::TPTASK_STATE_COMPLETED; 
}

//-------------------------------------------------------------------------------------
}
