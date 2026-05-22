// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved.

#include "entity_sqlstatement_mapping.h"
#include "sqlstatement.h"

namespace KBEngine {

template <> postgresql::EntitySqlStatementMapping* Singleton<postgresql::EntitySqlStatementMapping>::singleton_ = 0;

namespace postgresql {

EntitySqlStatementMapping g_entitySqlStatementMapping;

EntitySqlStatementMapping::EntitySqlStatementMapping()
{
}

EntitySqlStatementMapping::~EntitySqlStatementMapping()
{
}

void EntitySqlStatementMapping::addQuerySqlStatement(const std::string& tableName, SqlStatement* pSqlStatement)
{
	querySqlStatements_[tableName].reset(pSqlStatement);
}

void EntitySqlStatementMapping::addInsertSqlStatement(const std::string& tableName, SqlStatement* pSqlStatement)
{
	insertSqlStatements_[tableName].reset(pSqlStatement);
}

void EntitySqlStatementMapping::addUpdateSqlStatement(const std::string& tableName, SqlStatement* pSqlStatement)
{
	updateSqlStatements_[tableName].reset(pSqlStatement);
}

SqlStatement* EntitySqlStatementMapping::findQuerySqlStatement(const std::string& tableName)
{
	KBEUnordered_map<std::string, KBEShared_ptr<SqlStatement> >::iterator iter = querySqlStatements_.find(tableName);
	return iter == querySqlStatements_.end() ? NULL : iter->second.get();
}

SqlStatement* EntitySqlStatementMapping::findInsertSqlStatement(const std::string& tableName)
{
	KBEUnordered_map<std::string, KBEShared_ptr<SqlStatement> >::iterator iter = insertSqlStatements_.find(tableName);
	return iter == insertSqlStatements_.end() ? NULL : iter->second.get();
}

SqlStatement* EntitySqlStatementMapping::findUpdateSqlStatement(const std::string& tableName)
{
	KBEUnordered_map<std::string, KBEShared_ptr<SqlStatement> >::iterator iter = updateSqlStatements_.find(tableName);
	return iter == updateSqlStatements_.end() ? NULL : iter->second.get();
}

}
}
