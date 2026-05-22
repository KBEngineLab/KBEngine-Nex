// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved.

#ifndef KBE_POSTGRESQL_EXCEPTION_H
#define KBE_POSTGRESQL_EXCEPTION_H

#include <string>

namespace KBEngine {

class DBInterfacePostgresql;

/*
	PostgreSQL异常对象
	db_mysql 的 DBException 保持不动，PostgreSQL 单独维护 SQLSTATE 判断。
*/
class DBExceptionPostgresql : public std::exception
{
public:
	DBExceptionPostgresql(DBInterfacePostgresql* pdbi, const std::string& errStr, const std::string& sqlState);
	~DBExceptionPostgresql() throw();

	virtual const char* what() const throw() { return errStr_.c_str(); }

	// 40P01=deadlock_detected，40001=serialization_failure。
	bool shouldRetry() const;

	// SQLSTATE 08 开头表示 connection exception。
	bool isLostConnection() const;

private:
	DBInterfacePostgresql* pdbi_;
	std::string errStr_;
	std::string sqlState_;
};

}

#endif // KBE_POSTGRESQL_EXCEPTION_H
