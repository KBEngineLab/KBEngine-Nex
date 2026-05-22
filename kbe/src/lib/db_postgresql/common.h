// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved.

#ifndef KBE_DB_POSTGRESQL_COMMON_H
#define KBE_DB_POSTGRESQL_COMMON_H

#include "common/common.h"

namespace KBEngine {

class DBInterface;
class DBInterfacePostgresql;

namespace postgresql {

// 统一把通用 DBInterface 转回 PostgreSQL 后端，调用方不用到处 static_cast。
DBInterfacePostgresql* pg(DBInterface* pdbi);

// SQL 字符串字面量转义，只处理字面量内容，外层引号由调用方决定。
std::string esc(DBInterface* pdbi, const std::string& value);

// 实体表名统一加 tbl_ 前缀并按 PostgreSQL identifier 规则引用。
std::string tableSqlName(DBInterface* pdbi, const char* tableName);

// 字段名、索引名等 identifier 统一走 quoteIdentifier。
std::string columnSqlName(DBInterface* pdbi, const char* columnName);

// BYTEA 写入使用 decode(hex, 'hex')，这里负责把原始二进制转成 hex 文本。
std::string hexEncode(const char* data, size_t size);

}

}

#endif // KBE_DB_POSTGRESQL_COMMON_H
