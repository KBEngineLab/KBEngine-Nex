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

#ifndef KBE_POSTGRESQL_ENTITY_SQL_STATEMENT_MAPPING_H
#define KBE_POSTGRESQL_ENTITY_SQL_STATEMENT_MAPPING_H

#include "common/common.h"
#include "common/singleton.h"

namespace KBEngine {
namespace postgresql {

class SqlStatement;

/*
	实体 SQL 模板映射。
	Entity SQL statement template mapping.
	当前主要用于让 PostgreSQL 后端和 MySQL 一样有统一的 statement 管理入口，
	It currently gives PostgreSQL the same centralized statement management entry point as MySQL.
	后续如果要把字段列表缓存下来，可以直接挂在这里。
	Future column-list caching can be attached here without rescanning entity items.
*/
class EntitySqlStatementMapping : public Singleton<EntitySqlStatementMapping>
{
public:
	EntitySqlStatementMapping();
	virtual ~EntitySqlStatementMapping();

	void addQuerySqlStatement(const std::string& tableName, SqlStatement* pSqlStatement);
	void addInsertSqlStatement(const std::string& tableName, SqlStatement* pSqlStatement);
	void addUpdateSqlStatement(const std::string& tableName, SqlStatement* pSqlStatement);

	SqlStatement* findQuerySqlStatement(const std::string& tableName);
	SqlStatement* findInsertSqlStatement(const std::string& tableName);
	SqlStatement* findUpdateSqlStatement(const std::string& tableName);

private:
	KBEUnordered_map<std::string, KBEShared_ptr<SqlStatement> > querySqlStatements_;
	KBEUnordered_map<std::string, KBEShared_ptr<SqlStatement> > insertSqlStatements_;
	KBEUnordered_map<std::string, KBEShared_ptr<SqlStatement> > updateSqlStatements_;
};

}
}

#endif // KBE_POSTGRESQL_ENTITY_SQL_STATEMENT_MAPPING_H
