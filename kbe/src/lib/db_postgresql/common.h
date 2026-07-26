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

#ifndef KBE_DB_POSTGRESQL_COMMON_H
#define KBE_DB_POSTGRESQL_COMMON_H

#include "common/common.h"

namespace KBEngine {

class DBInterface;
class DBInterfacePostgresql;

namespace postgresql {

// 统一把通用 DBInterface 转回 PostgreSQL 后端，调用方不用到处 static_cast。
// Centralize conversion from DBInterface to the PostgreSQL backend so callers do not repeat static_cast.
DBInterfacePostgresql* pg(DBInterface* pdbi);

// SQL 字符串字面量转义，只处理字面量内容，外层引号由调用方决定。
// Escape SQL literal contents only; the caller remains responsible for surrounding quotes.
std::string esc(DBInterface* pdbi, const std::string& value);

// 实体表名统一加 tbl_ 前缀并按 PostgreSQL identifier 规则引用。
// Apply the tbl_ prefix and PostgreSQL identifier quoting consistently to entity tables.
std::string tableSqlName(DBInterface* pdbi, const char* tableName);

// 字段名、索引名等 identifier 统一走 quoteIdentifier。
// Route column, index, and other identifiers through quoteIdentifier consistently.
std::string columnSqlName(DBInterface* pdbi, const char* columnName);

// BYTEA 写入使用 decode(hex, 'hex')，这里负责把原始二进制转成 hex 文本。
// BYTEA writes use decode(hex, 'hex'); this helper converts raw bytes to hexadecimal text.
std::string hexEncode(const char* data, size_t size);

}

}

#endif // KBE_DB_POSTGRESQL_COMMON_H
