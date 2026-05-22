// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved.

#ifndef KBE_POSTGRESQL_ENTITY_TABLE_H
#define KBE_POSTGRESQL_ENTITY_TABLE_H

#include "db_interface/entity_table.h"

namespace KBEngine {

/*
	PostgreSQL实体表实现
	实体属性映射不能直接复用 MySQL 方言，PostgreSQL 字段生成在这个类型里维护。
*/
class EntityTablePostgresql : public EntityTable
{
public:
	EntityTablePostgresql(EntityTables* pEntityTables);
	virtual ~EntityTablePostgresql();

	virtual bool initialize(ScriptDefModule* sm, std::string name);
	virtual bool syncToDB(DBInterface* pdbi);
	virtual bool syncIndexToDB(DBInterface* pdbi);
	virtual EntityTableItem* createItem(std::string type, std::string defaultVal);
};

}

#endif // KBE_POSTGRESQL_ENTITY_TABLE_H
