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

#ifndef KBE_POSTGRESQL_SQL_STATEMENT_H
#define KBE_POSTGRESQL_SQL_STATEMENT_H

#include "common.h"
#include "common/common.h"
#include "db_interface/db_interface.h"
#include "helper/debug_helper.h"

#include <libpq-fe.h>

namespace KBEngine {
namespace postgresql {

typedef std::vector<std::pair<std::string, std::string> > DBItemValues;

std::string whereID(DBID dbid);
std::string whereParentID(DBInterface* pdbi, DBID parentID);
std::string orderByID();
std::string orderByIDLimit(uint32 limit);
std::string orderByIDLimitOffset(uint32 limit, uint32 offset);

class SqlStatement
{
public:
	SqlStatement(DBInterface* pdbi, std::string tableName);
	SqlStatement(DBInterface* pdbi, std::string tableName, const std::vector<std::string>& columns);
	virtual ~SqlStatement();

	const std::string& sql() const { return sqlstr_; }
	const std::string& tableName() const { return tableName_; }
	const std::string& sqlTableName() const { return sqlTableName_; }
	const std::string& sqlColumns() const { return sqlColumns_; }
	const std::vector<std::string>& columns() const { return columns_; }
	virtual bool query(DBInterface* pdbi = NULL);

protected:
	void setColumns(const std::vector<std::string>& columns);
	DBInterface* queryDBI(DBInterface* pdbi) const { return pdbi != NULL ? pdbi : pdbi_; }

	DBInterface* pdbi_;
	std::string tableName_;
	std::string sqlTableName_;
	std::string sqlColumns_;
	std::vector<std::string> columns_;
	std::string sqlstr_;
};

class SqlStatementInsert : public SqlStatement
{
public:
	SqlStatementInsert(DBInterface* pdbi, std::string tableName, const DBItemValues& values);
	virtual bool query(DBInterface* pdbi = NULL);

	DBID dbid() const { return dbid_; }

private:
	DBID dbid_;
};

class SqlStatementUpdate : public SqlStatement
{
public:
	SqlStatementUpdate(DBInterface* pdbi, std::string tableName, DBID dbid, const DBItemValues& values);
};

class SqlStatementQueryIDs : public SqlStatement
{
public:
	SqlStatementQueryIDs(DBInterface* pdbi, std::string tableName, const std::string& whereSql, const std::string& orderSql = "");
	virtual bool query(DBInterface* pdbi = NULL);

	const std::vector<DBID>& dbids() const { return dbids_; }
	bool found() const { return !dbids_.empty(); }

private:
	std::vector<DBID> dbids_;
};

class SqlStatementQuery : public SqlStatement
{
public:
	SqlStatementQuery(DBInterface* pdbi, std::string tableName, const std::vector<std::string>& columns,
		const std::string& whereSql, const std::string& orderSql = "");
	SqlStatementQuery(DBInterface* pdbi, const SqlStatement& statementTemplate,
		const std::string& whereSql, const std::string& orderSql = "");
	virtual ~SqlStatementQuery();
	virtual bool query(DBInterface* pdbi = NULL);

	int rows() const;
	bool isNull(int row, int column) const;
	char* value(int row, int column) const;
	int length(int row, int column) const;

private:
	PGresult* result_;
};

class SqlStatementDelete : public SqlStatement
{
public:
	SqlStatementDelete(DBInterface* pdbi, std::string tableName, const std::string& whereSql);
};

}
}

#endif // KBE_POSTGRESQL_SQL_STATEMENT_H
