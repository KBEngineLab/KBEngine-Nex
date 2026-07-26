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
#include "db_interface_mongodb.h"
#include "db_interface/db_interface.h"

namespace KBEngine {
	namespace mongodb {

		//-------------------------------------------------------------------------------------
		DBException::DBException(DBInterface* pdbi) :
			errStr_(static_cast<DBInterfaceMongodb*>(pdbi)->getstrerror()),
			errDomain_(static_cast<DBInterfaceMongodb*>(pdbi)->lastErrorDomain()),
			errCode_(static_cast<DBInterfaceMongodb*>(pdbi)->lastErrorCode())
		{
		}

		//-------------------------------------------------------------------------------------
		DBException::~DBException() throw()
		{
		}

		//-------------------------------------------------------------------------------------
		bool DBException::shouldRetry() const
		{
			// 仅重试服务端明确表示为瞬态的并发或主节点切换错误，避免永久错误造成 DB 工作线程死循环。
			// Retry only transient concurrency and primary-transition failures so permanent errors cannot spin a DB worker forever.
			return errCode_ == 24 || errCode_ == 50 || errCode_ == 91 || errCode_ == 112 ||
				errCode_ == 189 || errCode_ == 10107 || errCode_ == 11600 || errCode_ == 11602 ||
				errCode_ == 13435 || errCode_ == 13436;
		}

		//-------------------------------------------------------------------------------------
		bool DBException::isLostConnection() const
		{
			// 驱动的流、协议和服务器选择错误表示当前连接不可继续使用；部分服务端代码同样代表网络或节点不可达。
			// Driver stream, protocol, and server-selection domains invalidate the current connection; selected server codes also mean the node is unreachable.
			return errDomain_ == MONGOC_ERROR_STREAM || errDomain_ == MONGOC_ERROR_PROTOCOL ||
				errDomain_ == MONGOC_ERROR_SERVER_SELECTION || errCode_ == 6 || errCode_ == 7 ||
				errCode_ == 89 || errCode_ == 9001;
		}

		//-------------------------------------------------------------------------------------
	}
}
