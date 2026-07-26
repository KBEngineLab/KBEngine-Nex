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

#ifndef KBE_POSTGRESQL_EXCEPTION_H
#define KBE_POSTGRESQL_EXCEPTION_H

#include <string>

namespace KBEngine {

class DBInterfacePostgresql;

/*
	PostgreSQL异常对象
	PostgreSQL exception object.
	db_mysql 的 DBException 保持不动，PostgreSQL 单独维护 SQLSTATE 判断。
	Keep the MySQL DBException unchanged while PostgreSQL owns its SQLSTATE classification.
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
	// SQLSTATE class 08 denotes a connection exception.
	bool isLostConnection() const;

private:
	DBInterfacePostgresql* pdbi_;
	std::string errStr_;
	std::string sqlState_;
	// SQLSTATE 可能在物理断线时缺失，因此构造异常时同时快照 libpq 连接状态。
	// SQLSTATE can be absent on a physical disconnect, so snapshot the libpq connection state when constructing the exception.
	bool connectionLost_;
};

}

#endif // KBE_POSTGRESQL_EXCEPTION_H
