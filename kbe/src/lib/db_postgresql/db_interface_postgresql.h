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

#ifndef KBE_DB_INTERFACE_POSTGRESQL_H
#define KBE_DB_INTERFACE_POSTGRESQL_H

#include "common/common.h"
#include "common/memorystream.h"
#include "common/singleton.h"
#include "db_interface/db_interface.h"
#include "helper/debug_helper.h"

#include <libpq-fe.h>

namespace KBEngine {

/*
	PostgreSQL数据库接口
	PostgreSQL database interface.
	databaseInterfaces/type=postgresql 时由 DBUtil 创建。
	DBUtil creates this backend when databaseInterfaces/type is postgresql.
*/
class DBInterfacePostgresql : public DBInterface
{
public:
	using DBInterface::query;

	DBInterfacePostgresql(const char* name, std::string characterSet, std::string collation);
	virtual ~DBInterfacePostgresql();

	static bool initInterface(DBInterface* pdbi);

	virtual bool checkEnvironment();
	virtual bool checkErrors();

	virtual bool attach(const char* databaseName = NULL);
	virtual bool detach();
	bool reattach();

	virtual bool query(const char* cmd, uint32 size, bool printlog = true, MemoryStream* result = NULL);
	bool write_query_result(PGresult* pgResult, MemoryStream* result);
	PGresult* queryResult(const std::string& sql, const char* caller,
		ExecStatusType expectedStatus = PGRES_TUPLES_OK, bool traceQuery = true);

	virtual bool getTableNames(std::vector<std::string>& tableNames, const char* pattern);
	virtual bool getTableItemNames(const char* tableName, std::vector<std::string>& itemNames);

	virtual EntityTable* createEntityTable(EntityTables* pEntityTables);
	virtual bool dropEntityTableFromDB(const char* tableName);
	virtual bool dropEntityTableItemFromDB(const char* tableName, const char* tableItemName);

	virtual const char* c_str();
	virtual const char* getstrerror();
	virtual int getlasterror();

	virtual bool lock();
	virtual DBTransactionResult unlock();
	virtual bool rollback();
	virtual bool processException(std::exception& e);
	/**
		是否为自动增长id
		Whether database identifiers use automatic incrementing.
	*/
	bool isAutoIncrementDBID() const;

	PGconn* pgconn() { return pConn_; }
	const std::string& lastSqlState() const { return lastSqlState_; }
	bool inTransaction() const { return inTransaction_; }
	void inTransaction(bool value);

	std::string escapeString(const char* data, size_t size);
	std::string quoteIdentifier(const char* identifier);

private:
	bool connectToDatabase(const char* databaseName, bool logFailure = true);
	bool ensureDatabaseExists();
	void updateLastError(PGresult* result);

private:
	PGconn* pConn_;
	std::string characterSet_;
	std::string collation_;
	std::string lastError_;
	std::string lastSqlState_;
	bool inTransaction_;
	char cstr_[MAX_BUF * 2];
};

}

#endif // KBE_DB_INTERFACE_POSTGRESQL_H
