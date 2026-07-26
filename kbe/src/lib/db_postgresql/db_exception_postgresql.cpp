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

#include "db_exception_postgresql.h"
#include "db_interface_postgresql.h"

namespace KBEngine {

// 保存 libpq 返回的错误文本和 SQLSTATE，供 dbmgr 判断是否重试。
// Preserve libpq error text and SQLSTATE so dbmgr can decide whether to retry.
DBExceptionPostgresql::DBExceptionPostgresql(DBInterfacePostgresql* pdbi,
	const std::string& errStr,
	const std::string& sqlState) :
	pdbi_(pdbi),
	errStr_(errStr),
	sqlState_(sqlState)
{
}

// std::exception 派生类析构保持 throw()，和工程里已有异常类型一致。
// Keep the derived exception destructor throw() specification consistent with existing engine exception types.
DBExceptionPostgresql::~DBExceptionPostgresql() throw()
{
}

// 判断 PostgreSQL 事务级错误是否适合由 DB 线程重新执行当前任务。
// Classify transaction-level PostgreSQL failures that are safe for the DB worker to execute again.
bool DBExceptionPostgresql::shouldRetry() const
{
	return sqlState_ == "40P01" || sqlState_ == "40001";
}

// 判断 libpq 错误是否属于连接异常。
// Classify libpq failures that represent a broken database connection.
bool DBExceptionPostgresql::isLostConnection() const
{
	return sqlState_.size() >= 2 && sqlState_.compare(0, 2, "08") == 0;
}

}
