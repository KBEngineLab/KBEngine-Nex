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

#include "sqlstatement.h"
#include "db_interface_postgresql.h"

namespace KBEngine {
namespace postgresql {

std::string whereID(DBID dbid)
{
	return fmt::format("id={}", dbid);
}

std::string whereParentID(DBInterface* pdbi, DBID parentID)
{
	return fmt::format("{}={}", columnSqlName(pdbi, TABLE_PARENTID_CONST_STR), parentID);
}

std::string orderByID()
{
	return "ORDER BY id";
}

std::string orderByIDLimit(uint32 limit)
{
	return fmt::format("ORDER BY id LIMIT {}", limit);
}

std::string orderByIDLimitOffset(uint32 limit, uint32 offset)
{
	return fmt::format("ORDER BY id LIMIT {} OFFSET {}", limit, offset);
}

SqlStatement::SqlStatement(DBInterface* pdbi, std::string tableName) :
	pdbi_(pdbi),
	tableName_(tableName),
	sqlTableName_(tableSqlName(pdbi, tableName.c_str())),
	sqlColumns_(),
	columns_(),
	sqlstr_()
{
}

SqlStatement::SqlStatement(DBInterface* pdbi, std::string tableName, const std::vector<std::string>& columns) :
	pdbi_(pdbi),
	tableName_(tableName),
	sqlTableName_(tableSqlName(pdbi, tableName.c_str())),
	sqlColumns_(),
	columns_(),
	sqlstr_()
{
	setColumns(columns);
}

SqlStatement::~SqlStatement()
{
}

void SqlStatement::setColumns(const std::vector<std::string>& columns)
{
	columns_ = columns;
	sqlColumns_.clear();

	for (size_t i = 0; i < columns_.size(); ++i)
	{
		if (i > 0)
			sqlColumns_ += ",";

		sqlColumns_ += columnSqlName(pdbi_, columns_[i].c_str());
	}
}

/*
	执行已经拼好的普通 SQL。
	Execute a fully assembled ordinary SQL statement.
	这里保留为基类入口，适合 UPDATE/DELETE 这类不需要读取返回行的语句。
	The base entry point handles UPDATE and DELETE statements that do not need returned rows.
*/
bool SqlStatement::query(DBInterface* pdbi)
{
	if (sqlstr_.empty())
		return true;

	DBInterface* pQueryDBI = queryDBI(pdbi);
	if (!pQueryDBI->query(sqlstr_, false))
	{
		ERROR_MSG(fmt::format("postgresql::SqlStatement::query: {}, sql={}\n",
			pQueryDBI->getstrerror(), sqlstr_));
		return false;
	}

	return true;
}

SqlStatementInsert::SqlStatementInsert(DBInterface* pdbi, std::string tableName, const DBItemValues& values) :
	SqlStatement(pdbi, tableName),
	dbid_(0)
{
	sqlstr_ = fmt::format("INSERT INTO {} ", sqlTableName_);

	std::string columns;
	std::string sqlValues;

	// 非自增模式下显式写入主键 id，由引擎生成 UUID。
	// In non-incrementing mode, explicitly persist an engine-generated UUID as the primary key.
	if (!pg(pdbi)->isAutoIncrementDBID())
	{
		columns = columnSqlName(pdbi, TABLE_ID_CONST_STR);
		sqlValues = fmt::format("{}", (uint64)KBEngine::genUUID64());
	}

	for (size_t i = 0; i < values.size(); ++i)
	{
		if (!columns.empty())
		{
			columns += ",";
			sqlValues += ",";
		}

		columns += columnSqlName(pdbi, values[i].first.c_str());
		sqlValues += values[i].second;
	}

	if (!columns.empty())
	{
		sqlstr_ += fmt::format("({}) VALUES ({}) ", columns, sqlValues);
	}
	else
	{
		sqlstr_ += "DEFAULT VALUES ";
	}

	sqlstr_ += "RETURNING id";
}

/*
	插入主表或子表记录，并读取 PostgreSQL RETURNING id 返回的新 dbid。
	Insert a root or child row and read its new DBID from PostgreSQL RETURNING id.
	PostgreSQL 没有 MySQL last_insert_id() 那套连接状态，这里直接让 INSERT 返回主键，
	PostgreSQL does not expose MySQL-style last_insert_id() connection state, so INSERT returns the key directly.
	这样批量写实体时也不会依赖额外的查询。
	This also avoids an additional query during batched entity writes.
*/
bool SqlStatementInsert::query(DBInterface* pdbi)
{
	DBInterface* pQueryDBI = queryDBI(pdbi);
	PGresult* result = pg(pQueryDBI)->queryResult(sqlstr_, "postgresql::SqlStatementInsert::query");

	if (PQntuples(result) == 0)
	{
		ERROR_MSG(fmt::format("postgresql::SqlStatementInsert::query: RETURNING id returned no rows, sql={}\n", sqlstr_));
		PQclear(result);
		return false;
	}

	StringConv::str2value(dbid_, PQgetvalue(result, 0, 0));
	PQclear(result);
	return dbid_ > 0;
}

/*
	构造按 dbid 更新的 SET 语句。
	Build a SET statement targeting one DBID.
	values 里已经是上层根据字段类型转换后的 SQL 字面量，这里只负责表名、列名和 WHERE 条件。
	Values are already type-aware SQL literals, so this layer owns only quoted names and the WHERE predicate.
*/
SqlStatementUpdate::SqlStatementUpdate(DBInterface* pdbi, std::string tableName, DBID dbid, const DBItemValues& values) :
	SqlStatement(pdbi, tableName)
{
	if (values.empty())
		return;

	sqlstr_ = fmt::format("UPDATE {} SET ", sqlTableName_);
	for (size_t i = 0; i < values.size(); ++i)
	{
		if (i > 0)
			sqlstr_ += ",";

		sqlstr_ += fmt::format("{}={}", columnSqlName(pdbi, values[i].first.c_str()), values[i].second);
	}

	sqlstr_ += " WHERE " + whereID(dbid);
}

SqlStatementQueryIDs::SqlStatementQueryIDs(DBInterface* pdbi, std::string tableName, const std::string& whereSql, const std::string& orderSql) :
	SqlStatement(pdbi, tableName),
	dbids_()
{
	sqlstr_ = fmt::format("SELECT id FROM {}", sqlTableName_);
	if (!whereSql.empty())
		sqlstr_ += " WHERE " + whereSql;

	if (!orderSql.empty())
		sqlstr_ += " " + orderSql;
}

/*
	查询一组 dbid。
	Query a set of DBIDs.
	ARRAY、FIXED_DICT、ENTITY_COMPONENT 这类拆表结构都先取子表 id，
	Split-table structures such as ARRAY, FIXED_DICT, and ENTITY_COMPONENT first collect child IDs,
	再按 id 递归读取或删除子记录，逻辑和 MySQL 后端保持一致。
	then recursively read or remove child rows, matching MySQL behavior.
*/
bool SqlStatementQueryIDs::query(DBInterface* pdbi)
{
	dbids_.clear();

	DBInterface* pQueryDBI = queryDBI(pdbi);
	PGresult* result = pg(pQueryDBI)->queryResult(sqlstr_, "postgresql::SqlStatementQueryIDs::query");

	for (int i = 0; i < PQntuples(result); ++i)
	{
		DBID dbid = 0;
		StringConv::str2value(dbid, PQgetvalue(result, i, 0));
		dbids_.push_back(dbid);
	}

	PQclear(result);
	return true;
}

/*
	构造普通 SELECT 语句。
	Build an ordinary SELECT statement.
	调用方传入要读取的原生列名，字段值由 query() 后保留在 PGresult 中，
	The caller supplies raw column names and query() retains field values in PGresult.
	上层再根据 EntityDef 的类型逐列反序列化。
	Higher layers then deserialize each column according to its EntityDef type.
*/
SqlStatementQuery::SqlStatementQuery(DBInterface* pdbi, std::string tableName, const std::vector<std::string>& columns,
	const std::string& whereSql, const std::string& orderSql) :
	SqlStatement(pdbi, tableName, columns),
	result_(NULL)
{
	std::string cols = sqlColumns_.empty() ? std::string("id") : sqlColumns_;
	sqlstr_ = fmt::format("SELECT {} FROM {}", cols, sqlTableName_);
	if (!whereSql.empty())
		sqlstr_ += " WHERE " + whereSql;

	if (!orderSql.empty())
		sqlstr_ += " " + orderSql;
}

SqlStatementQuery::SqlStatementQuery(DBInterface* pdbi, const SqlStatement& statementTemplate,
	const std::string& whereSql, const std::string& orderSql) :
	SqlStatement(pdbi, statementTemplate.tableName(), statementTemplate.columns()),
	result_(NULL)
{
	std::string cols = sqlColumns_.empty() ? std::string("id") : sqlColumns_;
	sqlstr_ = fmt::format("SELECT {} FROM {}", cols, sqlTableName_);
	if (!whereSql.empty())
		sqlstr_ += " WHERE " + whereSql;

	if (!orderSql.empty())
		sqlstr_ += " " + orderSql;
}

SqlStatementQuery::~SqlStatementQuery()
{
	if (result_)
		PQclear(result_);
}

/*
	执行 SELECT 并持有结果集。
	Execute SELECT and retain its result set.
	结果集生命周期跟随 SqlStatementQuery 对象，读取 simple item 时可以直接拿列值和长度。
	The result lifetime follows SqlStatementQuery so simple items can safely access values and lengths.
*/
bool SqlStatementQuery::query(DBInterface* pdbi)
{
	if (result_)
	{
		PQclear(result_);
		result_ = NULL;
	}

	DBInterface* pQueryDBI = queryDBI(pdbi);
	result_ = pg(pQueryDBI)->queryResult(sqlstr_, "postgresql::SqlStatementQuery::query");

	return true;
}

int SqlStatementQuery::rows() const
{
	return result_ ? PQntuples(result_) : 0;
}

bool SqlStatementQuery::isNull(int row, int column) const
{
	return result_ == NULL || PQgetisnull(result_, row, column) != 0;
}

char* SqlStatementQuery::value(int row, int column) const
{
	return result_ ? PQgetvalue(result_, row, column) : NULL;
}

int SqlStatementQuery::length(int row, int column) const
{
	return result_ ? PQgetlength(result_, row, column) : 0;
}

/*
	构造删除语句。
	Build a DELETE statement.
	子表清理和实体主表删除都走这里，方便以后统一加 RETURNING、审计或 prepared statement。
	Child cleanup and root deletion share this path so RETURNING, auditing, or prepared statements can be added centrally.
*/
SqlStatementDelete::SqlStatementDelete(DBInterface* pdbi, std::string tableName, const std::string& whereSql) :
	SqlStatement(pdbi, tableName)
{
	sqlstr_ = fmt::format("DELETE FROM {}", sqlTableName_);
	if (!whereSql.empty())
		sqlstr_ += " WHERE " + whereSql;
}

}
}
