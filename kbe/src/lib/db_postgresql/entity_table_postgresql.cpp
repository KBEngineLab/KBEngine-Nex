// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved.

#include "entity_table_postgresql.h"
#include "db_interface/db_interface.h"
#include "entitydef/scriptdef_module.h"

namespace KBEngine {

// 绑定所属 EntityTables，具体表名在 initialize 时设置。
EntityTablePostgresql::EntityTablePostgresql(EntityTables* pEntityTables) :
	EntityTable(pEntityTables)
{
}

// 当前类没有额外持有资源。
EntityTablePostgresql::~EntityTablePostgresql()
{
}

// 初始化实体表对象，保存已经由上层生成好的数据库表名。
bool EntityTablePostgresql::initialize(ScriptDefModule* sm, std::string name)
{
	// 表名由上层按实体名生成，这里只保存最终名称。
	tableName(name);

	WARNING_MSG(fmt::format("EntityTablePostgresql::initialize: postgresql entity fields are not synced, entity={}\n", sm->getName()));
	return true;
}

// 同步实体基础表结构。
bool EntityTablePostgresql::syncToDB(DBInterface* pdbi)
{
	// 先创建基础实体表；属性字段需要 PostgreSQL 类型映射完成后再同步。
	std::string sql = fmt::format(
		"CREATE TABLE IF NOT EXISTS {} ("
		"id BIGSERIAL PRIMARY KEY, "
		"autoLoad SMALLINT DEFAULT 0"
		")",
		tableName());

	return pdbi->query(sql);
}

// 同步实体表索引。
bool EntityTablePostgresql::syncIndexToDB(DBInterface* /*pdbi*/)
{
	// 当前只有主键索引。
	return true;
}

// 创建实体属性字段映射项。
EntityTableItem* EntityTablePostgresql::createItem(std::string /*type*/, std::string /*defaultVal*/)
{
	// PostgreSQL 属性字段映射补齐前，不创建 EntityTableItem。
	return NULL;
}

}

