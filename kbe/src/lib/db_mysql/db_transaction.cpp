// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

#include "db_interface_mysql.h"
#include "db_transaction.h"
#include "db_exception.h"
#include "db_interface/db_interface.h"
#include "helper/debug_helper.h"
#include "common/timestamp.h"
#include <mysql/mysqld_error.h>
#include <mysql/errmsg.h>

namespace KBEngine { 
namespace mysql {
 
static std::string SQL_START_TRANSACTION = "START TRANSACTION";
static std::string SQL_ROLLBACK = "ROLLBACK";
static std::string SQL_COMMIT = "COMMIT";

//-------------------------------------------------------------------------------------
DBTransaction::DBTransaction(DBInterface* pdbi, bool autostart):
	pdbi_(pdbi),
	active_(false),
	committed_(false),
	autostart_(autostart)
{
	if(autostart)
		start();
}

//-------------------------------------------------------------------------------------
DBTransaction::~DBTransaction()
{
	if(autostart_)
		end();
}

//-------------------------------------------------------------------------------------
bool DBTransaction::start()
{
	if (active_)
		return true;

	committed_ = false;

	if (!pdbi_->query(SQL_START_TRANSACTION, false))
		return false;

	static_cast<DBInterfaceMysql*>(pdbi_)->inTransaction(true);
	active_ = true;
	return true;
}

//-------------------------------------------------------------------------------------
void DBTransaction::end()
{
	if (active_ && !committed_)
		rollback();
}

//-------------------------------------------------------------------------------------
bool DBTransaction::shouldRetry() const
{
	return (pdbi_->getlasterror() == ER_LOCK_DEADLOCK);
}

//-------------------------------------------------------------------------------------
bool DBTransaction::commit()
{
	KBE_ASSERT(active_ && !committed_);

	uint64 startTime = timestamp();

	if (!pdbi_->query(SQL_COMMIT, false))
		return false;

	uint64 duration = timestamp() - startTime;
	if(duration > stampsPerSecond() * 0.2f)
	{
		WARNING_MSG(fmt::format("DBTransaction::commit(): took {:.2f} seconds\n", 
			(double(duration)/stampsPerSecondD())));
	}

	committed_ = true;
	active_ = false;
	static_cast<DBInterfaceMysql*>(pdbi_)->inTransaction(false);
	return true;
}

//-------------------------------------------------------------------------------------
bool DBTransaction::rollback()
{
	if (!active_)
		return true;

	DBInterfaceMysql* pMysql = static_cast<DBInterfaceMysql*>(pdbi_);
	bool success = true;

	// 连接已经断开时事务由服务端释放，本地只需要清理事务状态。
	if (!pMysql->hasLostConnection())
	{
		try
		{
			success = pdbi_->query(SQL_ROLLBACK, false);
		}
		catch (DBException& e)
		{
			if (e.isLostConnection())
				pMysql->hasLostConnection(true);

			success = false;
		}
		catch (...)
		{
			success = false;
		}
	}

	active_ = false;
	committed_ = false;
	pMysql->inTransaction(false);
	return success;
}

}
}
