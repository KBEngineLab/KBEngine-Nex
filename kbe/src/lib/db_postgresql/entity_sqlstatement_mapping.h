// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved.

#ifndef KBE_POSTGRESQL_ENTITY_SQL_STATEMENT_MAPPING_H
#define KBE_POSTGRESQL_ENTITY_SQL_STATEMENT_MAPPING_H

#include "common/common.h"
#include "common/singleton.h"

namespace KBEngine {
namespace postgresql {

class SqlStatement;

/*
	实体 SQL 模板映射。
	当前主要用于让 PostgreSQL 后端和 MySQL 一样有统一的 statement 管理入口，
	后续如果要把字段列表缓存下来，可以直接挂在这里。
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
