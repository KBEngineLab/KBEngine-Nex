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

#ifndef KBE_DB_TASKS_H
#define KBE_DB_TASKS_H

#include "common/common.h"
#include "common/timer.h"
#include "db_transaction_result.h"
#include "thread/threadtask.h"

namespace KBEngine{ 

class MemoryStream;
class DBInterface;
class EntityTable;
class EntityTables;

/*
	数据库线程任务基础类
*/

class DBTaskBase : public thread::TPTask
{
public:

	DBTaskBase():
	initTime_(timestamp()),
	transactionResult_(DB_TRANSACTION_NOT_COMMITTED)
	{
	}

	virtual ~DBTaskBase(){}
	virtual bool process();
	virtual bool db_thread_process() = 0;
	virtual DBTaskBase* tryGetNextTask(){ return NULL; }
	virtual thread::TPTask::TPTaskState presentMainThread();
	// 结构同步任务需要由后端决定是否启用事务，避免把 MongoDB 索引枚举放进多文档事务。
	// Schema synchronization lets each backend choose transaction usage so MongoDB index enumeration never enters a multi-document transaction.
	virtual bool isSchemaSynchronization() const { return false; }

	virtual void pdbi(DBInterface* ptr){ pdbi_ = ptr; }
	void transactionResult(DBTransactionResult value){ transactionResult_ = value; }
	DBTransactionResult transactionResult() const { return transactionResult_; }
	bool transactionCommitted() const { return transactionResult_ == DB_TRANSACTION_COMMITTED; }

	uint64 initTime() const{ return initTime_; }

protected:
	DBInterface* pdbi_;
	uint64 initTime_;
	// 工作线程在进入主线程回调前写入最终提交状态，回调不得把 UNKNOWN 当作成功或安全重试。
	// The worker stores the final commit state before main-thread presentation; callbacks must not treat UNKNOWN as success or a safe retry.
	DBTransactionResult transactionResult_;
};

/**
	执行一条sql语句
*/
class DBTaskSyncTable : public DBTaskBase
{
public:
	DBTaskSyncTable(EntityTables* pEntityTables, KBEShared_ptr<EntityTable> pEntityTable);
	virtual ~DBTaskSyncTable();
	virtual bool db_thread_process();
	virtual thread::TPTask::TPTaskState presentMainThread();
	// 该标记只描述任务类别，具体事务能力仍由数据库适配层声明。
	// This flag identifies the task category; the database adapter still declares the actual transaction capability.
	virtual bool isSchemaSynchronization() const { return true; }

protected:
	KBEShared_ptr<EntityTable> pEntityTable_;
	bool success_;
	EntityTables* pEntityTables_;
};


}
#endif // KBE_DB_TASKS_H
