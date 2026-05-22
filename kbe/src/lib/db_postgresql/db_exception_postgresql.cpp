// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved.

#include "db_exception_postgresql.h"
#include "db_interface_postgresql.h"

namespace KBEngine {

// 保存 libpq 返回的错误文本和 SQLSTATE，供 dbmgr 判断是否重试。
DBExceptionPostgresql::DBExceptionPostgresql(DBInterfacePostgresql* pdbi,
	const std::string& errStr,
	const std::string& sqlState) :
	pdbi_(pdbi),
	errStr_(errStr),
	sqlState_(sqlState)
{
}

// std::exception 派生类析构保持 throw()，和工程里已有异常类型一致。
DBExceptionPostgresql::~DBExceptionPostgresql() throw()
{
}

// 判断 PostgreSQL 事务级错误是否适合由 DB 线程重新执行当前任务。
bool DBExceptionPostgresql::shouldRetry() const
{
	return sqlState_ == "40P01" || sqlState_ == "40001";
}

// 判断 libpq 错误是否属于连接异常。
bool DBExceptionPostgresql::isLostConnection() const
{
	return sqlState_.size() >= 2 && sqlState_.compare(0, 2, "08") == 0;
}

}
