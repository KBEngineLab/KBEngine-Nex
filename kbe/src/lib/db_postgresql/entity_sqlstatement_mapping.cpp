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
