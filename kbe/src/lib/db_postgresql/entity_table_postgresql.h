// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved.

#ifndef KBE_POSTGRESQL_ENTITY_TABLE_H
#define KBE_POSTGRESQL_ENTITY_TABLE_H

#include "db_interface/entity_table.h"

typedef struct pg_result PGresult;

namespace KBEngine {

namespace postgresql {
class SqlStatementQuery;
}

struct PostgreSQLColumnInfo
{
	PostgreSQLColumnInfo() :
		type(),
		defaultValue(),
		notNull(false),
		hasDefault(false)
	{
	}

	std::string type;
	std::string defaultValue;
	bool notNull;
	bool hasDefault;
};

typedef KBEUnordered_map<std::string, PostgreSQLColumnInfo> PostgreSQLColumnInfos;

class EntityTableItemPostgresql : public EntityTableItem
{
	friend class EntityTablePostgresql;

public:
	typedef std::vector< std::pair< std::string, KBEShared_ptr<EntityTableItem> > > FIXEDDICT_KEYTYPES;

	EntityTableItemPostgresql(std::string type, std::string defaultVal);
	virtual ~EntityTableItemPostgresql();

	virtual uint8 type() const;

	virtual bool initialize(const PropertyDescription* pPropertyDescription,
		const DataType* pDataType, std::string itemName);

	virtual bool syncToDB(DBInterface* pdbi, void* pData = NULL);
	virtual bool writeItem(DBInterface* pdbi, DBID dbid, MemoryStream* s, ScriptDefModule* pModule);
	virtual bool queryTable(DBInterface* pdbi, DBID dbid, MemoryStream* s, ScriptDefModule* pModule);

	virtual bool isSameKey(std::string key);
	void initDBItemNames(const char* exstrFlag = "");
	void collectDBItemNames(std::vector<std::string>& values) const;
	uint32 getItemDatabaseLength(const std::string& name);
	bool removeChildRows(DBInterface* pdbi, DBID parentID);

private:
	bool syncOneColumn(DBInterface* pdbi, const std::string& columnName, const std::string& columnType,
		const PostgreSQLColumnInfo* pColumnInfo);
	bool writeSimpleItem(DBInterface* pdbi, DBID dbid, MemoryStream* s);
	bool writeFixedDictItem(DBInterface* pdbi, DBID dbid, MemoryStream* s, ScriptDefModule* pModule);
	bool readFixedDictWriteValues(DBInterface* pdbi, DBID dbid, MemoryStream* s, ScriptDefModule* pModule,
		std::vector<std::pair<std::string, std::string> >& values);
	bool querySimpleItem(DBInterface* pdbi, DBID dbid, MemoryStream* s);
	bool appendQueryResultValue(DBInterface* pdbi, DBID dbid, MemoryStream* s, ScriptDefModule* pModule,
		postgresql::SqlStatementQuery& querycmd, int& columnIndex);
	bool readSqlValues(DBInterface* pdbi, MemoryStream* s, std::vector<std::pair<std::string, std::string> >& values);
	bool appendDefaultValue(MemoryStream* s, const DataType* pDataType);
	std::string binarySqlValue(const std::string& data);
	std::string escapedSqlValue(DBInterface* pdbi, const std::string& data);

	std::string dataSType_;
	std::string defaultVal_;
	std::string columnType_;
	std::vector<std::string> db_item_names_;
	FIXEDDICT_KEYTYPES keyTypes_;
	EntityTable* pChildTable_;
};

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
	virtual DBID writeTable(DBInterface* pdbi, DBID dbid, int8 shouldAutoLoad, MemoryStream* s, ScriptDefModule* pModule);
	virtual bool removeEntity(DBInterface* pdbi, DBID dbid, ScriptDefModule* pModule);
	virtual bool queryTable(DBInterface* pdbi, DBID dbid, MemoryStream* s, ScriptDefModule* pModule);
	virtual void entityShouldAutoLoad(DBInterface* pdbi, DBID dbid, bool shouldAutoLoad);
	virtual void queryAutoLoadEntities(DBInterface* pdbi, ScriptDefModule* pModule,
		ENTITY_ID start, ENTITY_ID end, std::vector<DBID>& outs);

	DBID insertChildRow(DBInterface* pdbi, DBID parentID);
	bool removeChildRowsByParentID(DBInterface* pdbi, DBID parentID);
	bool writeFixedOrderItems(DBInterface* pdbi, DBID dbid, MemoryStream* s, ScriptDefModule* pModule);
	bool queryFixedOrderItems(DBInterface* pdbi, DBID dbid, MemoryStream* s, ScriptDefModule* pModule);
	void collectDBItemNames(std::vector<std::string>& values) const;
};

}

#endif // KBE_POSTGRESQL_ENTITY_TABLE_H
