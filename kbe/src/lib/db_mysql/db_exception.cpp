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

#include "db_exception.h"
#include "db_interface_mysql.h"
#include "db_interface/db_interface.h"
#include <mysql/mysqld_error.h>
#include <mysql/errmsg.h>

namespace KBEngine { 

//-------------------------------------------------------------------------------------
DBException::DBException(DBInterface* pdbi) :
	errStr_(pdbi ? mysql_error(static_cast<DBInterfaceMysql*>(pdbi)->mysql()) : ""),
	errNum_(pdbi ? mysql_errno(static_cast<DBInterfaceMysql*>(pdbi)->mysql()) : 0)
{
}

//-------------------------------------------------------------------------------------
DBException::~DBException() throw()
{
}

//-------------------------------------------------------------------------------------
bool DBException::shouldRetry() const
{
	return (errNum_== ER_LOCK_DEADLOCK) ||
			(errNum_ == ER_LOCK_WAIT_TIMEOUT);
}

//-------------------------------------------------------------------------------------
bool DBException::isLostConnection() const
{
	// Connector/C 在已有连接失效后可能从传输层或 TLS 重协商返回不同 CR_* 错误；这些错误都需要重建连接。
	// Connector/C can report transport or TLS CR_* errors after an established session dies; all require rebuilding the connection.
	switch (errNum_)
	{
	case CR_CONNECTION_ERROR:
	case CR_CONN_HOST_ERROR:
	case CR_IPSOCK_ERROR:
	case CR_SERVER_GONE_ERROR:
	case CR_TCP_CONNECTION:
	case CR_SERVER_LOST:
	case CR_SSL_CONNECTION_ERROR:
	case CR_SERVER_LOST_EXTENDED:
		return true;
	default:
		return false;
	}
}

//-------------------------------------------------------------------------------------
}

// db_exception.cpp
