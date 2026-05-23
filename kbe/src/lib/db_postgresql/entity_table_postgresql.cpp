// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved.

#include "entity_table_postgresql.h"
#include "common.h"
#include "db_interface_postgresql.h"
#include "db_transaction.h"
#include "db_interface/db_interface.h"
#include "entity_sqlstatement_mapping.h"
#include "entitydef/datatypes.h"
#include "entitydef/entitydef.h"
#include "entitydef/property.h"
#include "entitydef/scriptdef_module.h"
#include "network/fixed_messages.h"
#include "sqlstatement.h"

#include <limits>
#include <sstream>

namespace KBEngine {
using postgresql::columnSqlName;
using postgresql::DBItemValues;
using postgresql::hexEncode;
using postgresql::pg;
using postgresql::tableSqlName;

namespace
{
std::string normalizeDataSType(const std::string& type)
{
	if (type == "DBID" || type == "UID" || type == "COMPONENT_ID")
		return "UINT64";

	if (type == "ENTITY_ID")
		return "INT32";

	if (type == "SPACE_ID")
		return "UINT32";

	return type;
}

std::string columnBaseType(const std::string& columnType)
{
	size_t end = columnType.size();
	size_t notNull = columnType.find(" NOT NULL");
	size_t defaultPos = columnType.find(" DEFAULT ");

	if (notNull != std::string::npos)
		end = std::min(end, notNull);
	if (defaultPos != std::string::npos)
		end = std::min(end, defaultPos);

	return columnType.substr(0, end);
}

std::string normalizedTypeName(const std::string& type)
{
	std::string value = type;
	std::transform(value.begin(), value.end(), value.begin(), tolower);

	KBEngine::strutil::kbe_replace(value, "character varying", "varchar");
	KBEngine::strutil::kbe_replace(value, " without time zone", "");
	KBEngine::strutil::kbe_trim(value);
	return value;
}

std::string normalizedDefaultValue(const std::string& value)
{
	std::string normalized = value;
	KBEngine::strutil::kbe_trim(normalized);

	size_t castPos = normalized.find("::");
	if (castPos != std::string::npos)
		normalized = normalized.substr(0, castPos);

	return normalized;
}

bool columnWantsNotNull(const std::string& columnType)
{
	return columnType.find("NOT NULL") != std::string::npos;
}

bool columnDefaultValue(const std::string& columnType, std::string& defaultValue)
{
	size_t defaultPos = columnType.find(" DEFAULT ");
	if (defaultPos == std::string::npos)
		return false;

	defaultValue = columnType.substr(defaultPos + strlen(" DEFAULT "));
	return true;
}

bool loadPostgresqlColumnInfos(DBInterface* pdbi, const std::string& tableName, PostgreSQLColumnInfos& columnInfos)
{
	std::string sql = fmt::format(
		"SELECT a.attname, format_type(a.atttypid, a.atttypmod), a.attnotnull, pg_get_expr(d.adbin, d.adrelid) "
		"FROM pg_attribute a "
		"JOIN pg_class c ON c.oid=a.attrelid "
		"JOIN pg_namespace n ON n.oid=c.relnamespace "
		"LEFT JOIN pg_attrdef d ON d.adrelid=a.attrelid AND d.adnum=a.attnum "
		"WHERE n.nspname='public' AND c.relname='{}' AND a.attnum>0 AND NOT a.attisdropped "
		"ORDER BY a.attnum",
		pg(pdbi)->escapeString(tableName.c_str(), tableName.size()));

	/*
		字段同步需要知道当前类型、默认值、NOT NULL 状态。这里一次查完整张表，
		后面逐列判断是否真的需要 ALTER，避免“什么都没改”时也反复 SET NOT NULL。
	*/
	PGresult* result = pg(pdbi)->queryResult(sql, "loadPostgresqlColumnInfos", PGRES_TUPLES_OK, false);
	for (int i = 0; i < PQntuples(result); ++i)
	{
		PostgreSQLColumnInfo& info = columnInfos[PQgetvalue(result, i, 0)];
		info.type = normalizedTypeName(PQgetvalue(result, i, 1));
		info.notNull = strcmp(PQgetvalue(result, i, 2), "t") == 0;
		info.hasDefault = PQgetisnull(result, i, 3) == 0;
		if (info.hasDefault)
			info.defaultValue = normalizedDefaultValue(PQgetvalue(result, i, 3));
	}

	PQclear(result);
	return true;
}

std::string indexSqlType(const char* indexType)
{
	if (kbe_stricmp(indexType, "UNIQUE") == 0)
		return "UNIQUE INDEX";

	return "INDEX";
}

bool stringVectorContains(const std::vector<std::string>& values, const std::string& value)
{
	for (size_t i = 0; i < values.size(); ++i)
	{
		if (kbe_stricmp(values[i].c_str(), value.c_str()) == 0)
			return true;
	}

	return false;
}

bool loadPostgresqlIndexNames(DBInterface* pdbi, const std::string& tableName, std::vector<std::string>& indexNames)
{
	std::string sql = fmt::format(
		"SELECT indexname FROM pg_indexes WHERE schemaname='public' AND tablename='{}'",
		pg(pdbi)->escapeString(tableName.c_str(), tableName.size()));

	/*
		这是同步阶段的元数据探测，不代表真正执行的业务 SQL。
		不写 lastquery_，否则 DBTask 慢任务日志会把这条探测 SQL 打出来，排查时容易误判。
	*/
	PGresult* result = pg(pdbi)->queryResult(sql, "loadPostgresqlIndexNames", PGRES_TUPLES_OK, false);
	for (int i = 0; i < PQntuples(result); ++i)
		indexNames.push_back(PQgetvalue(result, i, 0));

	PQclear(result);
	return true;
}

std::string describeTableItemUtypes(const EntityTable::TABLEITEM_MAP& items)
{
	std::string text;
	EntityTable::TABLEITEM_MAP::const_iterator iter = items.begin();
	for (; iter != items.end(); ++iter)
	{
		if (!text.empty())
			text += ",";

		text += fmt::format("{}:{}", iter->first, iter->second->itemName());
	}

	return text;
}
}

EntityTableItemPostgresql::EntityTableItemPostgresql(std::string type, std::string defaultVal) :
	EntityTableItem("", 0, 0),
	dataSType_(normalizeDataSType(type)),
	defaultVal_(defaultVal),
	columnType_(),
	db_item_names_(),
	keyTypes_(),
	pChildTable_(NULL)
{
	/*
		DBID、UID 这类类型在实体定义里是别名，数据库表和流读写都按底层整数处理。
		这里统一使用归一化后的 dataSType_，避免列同步和写入流消费各走一套判断。
	*/
	const std::string& dbType = dataSType_;

	if (dbType == "INT8")
		columnType_ = "SMALLINT NOT NULL DEFAULT 0";
	else if (dbType == "INT16")
		columnType_ = "SMALLINT NOT NULL DEFAULT 0";
	else if (dbType == "INT32")
		columnType_ = "INTEGER NOT NULL DEFAULT 0";
	else if (dbType == "INT64")
		columnType_ = "BIGINT NOT NULL DEFAULT 0";
	else if (dbType == "UINT8")
		columnType_ = "SMALLINT NOT NULL DEFAULT 0";
	else if (dbType == "UINT16")
		columnType_ = "INTEGER NOT NULL DEFAULT 0";
	else if (dbType == "UINT32")
		columnType_ = "BIGINT NOT NULL DEFAULT 0";
	else if (dbType == "UINT64")
		columnType_ = "NUMERIC(20,0) NOT NULL DEFAULT 0";
	else if (dbType == "FLOAT")
		columnType_ = "REAL NOT NULL DEFAULT 0";
	else if (dbType == "DOUBLE")
		columnType_ = "DOUBLE PRECISION NOT NULL DEFAULT 0";
	else if (dbType == "STRING" || dbType == "UNICODE")
		columnType_ = "VARCHAR(255) NOT NULL DEFAULT ''";
	else if (dbType == "BLOB" || dbType == "PYTHON" || dbType == "PY_DICT" || dbType == "PY_TUPLE" || dbType == "PY_LIST")
		columnType_ = "BYTEA";
	else if (dbType == "VECTOR2" || dbType == "VECTOR3" || dbType == "VECTOR4")
#ifdef CLIENT_NO_FLOAT
		columnType_ = "INTEGER NOT NULL DEFAULT 0";
#else
		columnType_ = "REAL NOT NULL DEFAULT 0";
#endif
}

EntityTableItemPostgresql::~EntityTableItemPostgresql()
{
}

uint8 EntityTableItemPostgresql::type() const
{
	if (dataSType_ == "ARRAY")
		return TABLE_ITEM_TYPE_FIXEDARRAY;
	if (dataSType_ == "FIXED_DICT")
		return TABLE_ITEM_TYPE_FIXEDDICT;
	if (dataSType_ == "ENTITY_COMPONENT")
		return TABLE_ITEM_TYPE_COMPONENT;
	if (dataSType_ == "VECTOR2")
		return TABLE_ITEM_TYPE_VECTOR2;
	if (dataSType_ == "VECTOR3")
		return TABLE_ITEM_TYPE_VECTOR3;
	if (dataSType_ == "VECTOR4")
		return TABLE_ITEM_TYPE_VECTOR4;
	if (dataSType_ == "STRING")
		return TABLE_ITEM_TYPE_STRING;
	if (dataSType_ == "UNICODE")
		return TABLE_ITEM_TYPE_UNICODE;
	if (dataSType_ == "BLOB")
		return TABLE_ITEM_TYPE_BLOB;
	if (dataSType_ == "PYTHON" || dataSType_ == "PY_DICT" || dataSType_ == "PY_TUPLE" || dataSType_ == "PY_LIST")
		return TABLE_ITEM_TYPE_PYTHON;
	if (dataSType_ == "ENTITYCALL")
		return TABLE_ITEM_TYPE_ENTITYCALL;

	return TABLE_ITEM_TYPE_DIGIT;
}

bool EntityTableItemPostgresql::initialize(const PropertyDescription* pPropertyDescription,
	const DataType* pDataType, std::string itemName)
{
	itemName_ = itemName;
	pDataType_ = pDataType;
	pPropertyDescription_ = pPropertyDescription;
	indexType_ = pPropertyDescription ? pPropertyDescription->indexType() : "";
	initDBItemNames();

	if (dataSType_ == "ARRAY")
	{
		EntityTablePostgresql* pTable = new EntityTablePostgresql(pParentTable_->pEntityTables());
		std::string tname = pParentTable_->tableName();
		std::vector<std::string> qname;
		EntityTableItem* pparentItem = pParentTableItem_;
		while (pparentItem != NULL)
		{
			if (strlen(pparentItem->itemName()) > 0)
				qname.push_back(pparentItem->itemName());
			pparentItem = pparentItem->pParentTableItem();
		}

		for (int i = static_cast<int>(qname.size()) - 1; i >= 0; --i)
			tname += "_" + qname[i];

		std::string childTableName = tname + "_" + (itemName.empty() ? TABLE_ARRAY_ITEM_VALUES_CONST_STR : itemName);
		std::string childItemName;
		FixedArrayType* pArrayType = static_cast<FixedArrayType*>(const_cast<DataType*>(pDataType));
		if (pArrayType->getDataType()->type() != DATA_TYPE_FIXEDDICT)
			childItemName = TABLE_ARRAY_ITEM_VALUE_CONST_STR;

		pTable->tableName(childTableName);
		pTable->isChild(true);

		EntityTableItem* pArrayItem = pParentTable_->createItem(pArrayType->getDataType()->getName(),
			pPropertyDescription ? pPropertyDescription->getDefaultValStr() : "");
		pArrayItem->utype(pPropertyDescription ? -pPropertyDescription->getUType() : 0);
		pArrayItem->pParentTable(pParentTable_);
		pArrayItem->pParentTableItem(this);
		pArrayItem->tableName(pTable->tableName());

		if (!pArrayItem->initialize(pPropertyDescription, pArrayType->getDataType(), childItemName))
		{
			delete pTable;
			return false;
		}

		pTable->addItem(pArrayItem);
		pChildTable_ = pTable;
		pTable->pEntityTables()->addTable(pTable);
		return true;
	}

	if (dataSType_ == "FIXED_DICT")
	{
		FixedDictType* pFixedDictType = static_cast<FixedDictType*>(const_cast<DataType*>(pDataType));
		FixedDictType::FIXEDDICT_KEYTYPE_MAP& keyTypes = pFixedDictType->getKeyTypes();
		FixedDictType::FIXEDDICT_KEYTYPE_MAP::iterator iter = keyTypes.begin();
		for (; iter != keyTypes.end(); ++iter)
		{
			if (!iter->second->persistent)
				continue;

			EntityTableItem* pItem = pParentTable_->createItem(iter->second->dataType->getName(),
				pPropertyDescription ? pPropertyDescription->getDefaultValStr() : "");
			pItem->pParentTable(pParentTable_);
			pItem->pParentTableItem(this);
			pItem->utype(pPropertyDescription ? -pPropertyDescription->getUType() : 0);
			pItem->tableName(tableName_);

			if (!pItem->initialize(pPropertyDescription, iter->second->dataType, iter->first))
			{
				delete pItem;
				return false;
			}

			keyTypes_.push_back(std::make_pair(iter->first, KBEShared_ptr<EntityTableItem>(pItem)));
		}

		initDBItemNames();
		return true;
	}

	if (dataSType_ == "ENTITY_COMPONENT")
	{
		EntityComponentType* pComponentType = static_cast<EntityComponentType*>(const_cast<DataType*>(pDataType));
		ScriptDefModule* pComponentModule = pComponentType->pScriptDefModule();
		EntityTablePostgresql* pParentTable = static_cast<EntityTablePostgresql*>(pParentTable_);
		EntityTablePostgresql* pTable = new EntityTablePostgresql(pParentTable->pEntityTables());
		pTable->tableName(std::string(pParentTable->tableName()) + "_" + itemName);
		pTable->isChild(true);

		ScriptDefModule* pOwnerModule = EntityDef::findScriptModule(pParentTable->tableName(), false);
		ScriptDefModule::PROPERTYDESCRIPTION_MAP& pdescrsMap = pComponentModule->getPersistentPropertyDescriptions();
		ScriptDefModule::PROPERTYDESCRIPTION_MAP::const_iterator iter = pdescrsMap.begin();

		for (; iter != pdescrsMap.end(); ++iter)
		{
			PropertyDescription* pdescrs = iter->second;
			if (pOwnerModule && !pOwnerModule->hasCell() && pdescrs->hasCell() && !pdescrs->hasBase())
				continue;

			EntityTableItem* pItem = pParentTable->createItem(pdescrs->getDataType()->getName(), pdescrs->getDefaultValStr());
			pItem->pParentTable(pParentTable);
			pItem->pParentTableItem(this);
			pItem->utype(pdescrs->getUType());
			pItem->tableName(pTable->tableName());

			if (!pItem->initialize(pdescrs, pdescrs->getDataType(), pdescrs->getName()))
			{
				delete pTable;
				return false;
			}

			pTable->addItem(pItem);
		}

		pChildTable_ = pTable;
		pTable->pEntityTables()->addTable(pTable);
		return true;
	}

	return pDataType_ != NULL || dataSType_ == "ENTITYCALL";
}

void EntityTableItemPostgresql::initDBItemNames(const char* exstrFlag)
{
	db_item_names_.clear();

	if (dataSType_ == "VECTOR2" || dataSType_ == "VECTOR3" || dataSType_ == "VECTOR4")
	{
		int count = dataSType_ == "VECTOR2" ? 2 : (dataSType_ == "VECTOR3" ? 3 : 4);
		for (int i = 0; i < count; ++i)
			db_item_names_.push_back(fmt::format(TABLE_ITEM_PERFIX "_{}_{}{}", i, exstrFlag, itemName()));
		return;
	}

	if (dataSType_ == "FIXED_DICT")
	{
		FIXEDDICT_KEYTYPES::iterator iter = keyTypes_.begin();
		for (; iter != keyTypes_.end(); ++iter)
		{
			std::string nextFlag = exstrFlag;
			if (iter->second->type() == TABLE_ITEM_TYPE_FIXEDDICT)
				nextFlag += iter->first + "_";

			static_cast<EntityTableItemPostgresql*>(iter->second.get())->initDBItemNames(nextFlag.c_str());
		}
		return;
	}

	if (dataSType_ == "ARRAY" || dataSType_ == "ENTITY_COMPONENT")
		return;

	db_item_names_.push_back(std::string(TABLE_ITEM_PERFIX "_") + exstrFlag + itemName());
}

bool EntityTableItemPostgresql::isSameKey(std::string key)
{
	for (size_t i = 0; i < db_item_names_.size(); ++i)
	{
		if (db_item_names_[i] == key)
			return true;
	}

	FIXEDDICT_KEYTYPES::iterator iter = keyTypes_.begin();
	for (; iter != keyTypes_.end(); ++iter)
	{
		if (iter->second->isSameKey(key))
			return true;
	}

	return false;
}

void EntityTableItemPostgresql::collectDBItemNames(std::vector<std::string>& values) const
{
	if (dataSType_ == "FIXED_DICT")
	{
		FIXEDDICT_KEYTYPES::const_iterator iter = keyTypes_.begin();
		for (; iter != keyTypes_.end(); ++iter)
			static_cast<EntityTableItemPostgresql*>(iter->second.get())->collectDBItemNames(values);

		return;
	}

	values.insert(values.end(), db_item_names_.begin(), db_item_names_.end());
}

uint32 EntityTableItemPostgresql::getItemDatabaseLength(const std::string& name)
{
	if (dataSType_ != "FIXED_DICT")
		return 0;

	FixedDictType* pFixedDictType = static_cast<FixedDictType*>(const_cast<DataType*>(pDataType_));
	FixedDictType::FIXEDDICT_KEYTYPE_MAP& keyTypes = pFixedDictType->getKeyTypes();
	FixedDictType::FIXEDDICT_KEYTYPE_MAP::iterator iter = keyTypes.begin();
	for (; iter != keyTypes.end(); ++iter)
	{
		if (iter->first == name)
			return iter->second->databaseLength;
	}

	return 0;
}

bool EntityTableItemPostgresql::syncOneColumn(DBInterface* pdbi, const std::string& columnName, const std::string& columnType,
	const PostgreSQLColumnInfo* pColumnInfo)
{
	std::string sqlTable = tableSqlName(pdbi, tableName());
	std::string sqlColumn = columnSqlName(pdbi, columnName.c_str());
	bool columnExists = pColumnInfo != NULL;

	if (!columnExists)
	{
		DEBUG_MSG(fmt::format("syncToDB(): {}->{}({}).\n", tableName(), columnName, columnType));
	}

	std::string sql = fmt::format("ALTER TABLE {} ADD COLUMN IF NOT EXISTS {} {}",
		sqlTable.c_str(),
		sqlColumn.c_str(),
		columnType.c_str());

	if (!pdbi->query(sql))
		return false;

	/*
		MySQL 后端在字段已存在时会继续 MODIFY COLUMN；PostgreSQL 的
		ADD COLUMN IF NOT EXISTS 只负责补字段，不会管类型、长度、默认值是否已经变了。
		实体 def 调整 DatabaseLength 或基础类型后，如果这里不跟进，运行期读写就会和定义脱节。
		因此每次同步都再执行一组幂等 ALTER：类型相同基本无成本，类型不兼容则让数据库明确报错。
	*/
	std::string baseType = columnBaseType(columnType);
	std::string expectedType = normalizedTypeName(baseType);
	if (!columnExists || pColumnInfo->type != expectedType)
	{
		sql = fmt::format("ALTER TABLE {} ALTER COLUMN {} TYPE {} USING {}::{}",
			sqlTable.c_str(), sqlColumn.c_str(), baseType.c_str(), sqlColumn.c_str(), baseType.c_str());

		if (!pdbi->query(sql))
			return false;
	}

	std::string defaultValue;
	bool hasDefault = false;
	if (columnDefaultValue(columnType, defaultValue))
	{
		hasDefault = true;
		if (!columnExists || !pColumnInfo->hasDefault || pColumnInfo->defaultValue != normalizedDefaultValue(defaultValue))
		{
			sql = fmt::format("ALTER TABLE {} ALTER COLUMN {} SET DEFAULT {}",
				sqlTable.c_str(), sqlColumn.c_str(), defaultValue.c_str());
			if (!pdbi->query(sql))
				return false;
		}
	}
	else
	{
		if (columnExists && pColumnInfo->hasDefault)
		{
			sql = fmt::format("ALTER TABLE {} ALTER COLUMN {} DROP DEFAULT",
				sqlTable.c_str(), sqlColumn.c_str());
			if (!pdbi->query(sql))
				return false;
		}
	}

	// 老表字段如果曾经允许 NULL，先按当前默认值补齐，再切成 NOT NULL，少一次人工清表。
	bool wantsNotNull = columnWantsNotNull(columnType);
	if (wantsNotNull && hasDefault && (!columnExists || !pColumnInfo->notNull))
	{
		sql = fmt::format("UPDATE {} SET {}={} WHERE {} IS NULL",
			sqlTable.c_str(), sqlColumn.c_str(), defaultValue.c_str(), sqlColumn.c_str());
		if (!pdbi->query(sql))
			return false;
	}

	if (!columnExists || pColumnInfo->notNull != wantsNotNull)
	{
		sql = fmt::format("ALTER TABLE {} ALTER COLUMN {} {} NOT NULL",
			sqlTable.c_str(), sqlColumn.c_str(), wantsNotNull ? "SET" : "DROP");
		return pdbi->query(sql);
	}

	return true;
}

bool EntityTableItemPostgresql::syncToDB(DBInterface* pdbi, void* pData)
{
	if (dataSType_ == "ARRAY" || dataSType_ == "ENTITY_COMPONENT")
		return true;

	PostgreSQLColumnInfos* pColumnInfos = static_cast<PostgreSQLColumnInfos*>(pData);

	if (dataSType_ == "FIXED_DICT")
	{
		FIXEDDICT_KEYTYPES::iterator iter = keyTypes_.begin();
		for (; iter != keyTypes_.end(); ++iter)
		{
			if (!iter->second->syncToDB(pdbi, pData))
				return false;
		}

		return true;
	}

	if (dataSType_ == "ENTITYCALL")
		return true;

	std::string type = columnType_;
	if ((dataSType_ == "STRING" || dataSType_ == "UNICODE") && pPropertyDescription_)
	{
		uint32 length = pPropertyDescription_->getDatabaseLength();
		if (pParentTableItem_ && pParentTableItem_->type() == TABLE_ITEM_TYPE_FIXEDDICT)
			length = static_cast<EntityTableItemPostgresql*>(pParentTableItem_)->getItemDatabaseLength(itemName());

		type = length > 0 ? fmt::format("VARCHAR({}) NOT NULL DEFAULT ''", length) : "VARCHAR(255) NOT NULL DEFAULT ''";
	}

	for (size_t i = 0; i < db_item_names_.size(); ++i)
	{
		PostgreSQLColumnInfo* pColumnInfo = NULL;
		if (pColumnInfos != NULL)
		{
			PostgreSQLColumnInfos::iterator iter = pColumnInfos->find(db_item_names_[i]);
			if (iter != pColumnInfos->end())
				pColumnInfo = &iter->second;
		}

		if (!syncOneColumn(pdbi, db_item_names_[i], type, pColumnInfo))
			return false;
	}

	return true;
}

std::string EntityTableItemPostgresql::escapedSqlValue(DBInterface* pdbi, const std::string& data)
{
	return fmt::format("'{}'", pg(pdbi)->escapeString(data.data(), data.size()));
}

std::string EntityTableItemPostgresql::binarySqlValue(const std::string& data)
{
	return fmt::format("decode('{}', 'hex')", hexEncode(data.data(), data.size()));
}

bool EntityTableItemPostgresql::readSqlValues(DBInterface* pdbi, MemoryStream* s, std::vector<std::pair<std::string, std::string> >& values)
{
	if (dataSType_ == "ENTITYCALL")
		return true;

	char buf[MAX_BUF];
	if (dataSType_ == "INT8")
	{
		int8 v; (*s) >> v; kbe_snprintf(buf, MAX_BUF, "%d", v); values.push_back(std::make_pair(db_item_names_[0], buf));
	}
	else if (dataSType_ == "INT16")
	{
		int16 v; (*s) >> v; kbe_snprintf(buf, MAX_BUF, "%d", v); values.push_back(std::make_pair(db_item_names_[0], buf));
	}
	else if (dataSType_ == "INT32")
	{
		int32 v; (*s) >> v; kbe_snprintf(buf, MAX_BUF, "%d", v); values.push_back(std::make_pair(db_item_names_[0], buf));
	}
	else if (dataSType_ == "INT64")
	{
		int64 v; (*s) >> v; kbe_snprintf(buf, MAX_BUF, "%" PRI64, v); values.push_back(std::make_pair(db_item_names_[0], buf));
	}
	else if (dataSType_ == "UINT8")
	{
		uint8 v; (*s) >> v; kbe_snprintf(buf, MAX_BUF, "%u", v); values.push_back(std::make_pair(db_item_names_[0], buf));
	}
	else if (dataSType_ == "UINT16")
	{
		uint16 v; (*s) >> v; kbe_snprintf(buf, MAX_BUF, "%u", v); values.push_back(std::make_pair(db_item_names_[0], buf));
	}
	else if (dataSType_ == "UINT32")
	{
		uint32 v; (*s) >> v; kbe_snprintf(buf, MAX_BUF, "%u", v); values.push_back(std::make_pair(db_item_names_[0], buf));
	}
	else if (dataSType_ == "UINT64")
	{
		uint64 v; (*s) >> v; kbe_snprintf(buf, MAX_BUF, "%" PRIu64, v); values.push_back(std::make_pair(db_item_names_[0], buf));
	}
	else if (dataSType_ == "FLOAT")
	{
		float v; (*s) >> v; kbe_snprintf(buf, MAX_BUF, "%f", v); values.push_back(std::make_pair(db_item_names_[0], buf));
	}
	else if (dataSType_ == "DOUBLE")
	{
		double v; (*s) >> v; kbe_snprintf(buf, MAX_BUF, "%lf", v); values.push_back(std::make_pair(db_item_names_[0], buf));
	}
	else if (dataSType_ == "STRING")
	{
		std::string v; (*s) >> v; values.push_back(std::make_pair(db_item_names_[0], escapedSqlValue(pdbi, v)));
	}
	else if (dataSType_ == "UNICODE" || dataSType_ == "BLOB" || dataSType_ == "PYTHON" || dataSType_ == "PY_DICT" || dataSType_ == "PY_TUPLE" || dataSType_ == "PY_LIST")
	{
		std::string v; s->readBlob(v);
		values.push_back(std::make_pair(db_item_names_[0], dataSType_ == "UNICODE" ? escapedSqlValue(pdbi, v) : binarySqlValue(v)));
	}
	else if (dataSType_ == "VECTOR2" || dataSType_ == "VECTOR3" || dataSType_ == "VECTOR4")
	{
		int count = dataSType_ == "VECTOR2" ? 2 : (dataSType_ == "VECTOR3" ? 3 : 4);
		for (int i = 0; i < count; ++i)
		{
#ifdef CLIENT_NO_FLOAT
			int32 v; (*s) >> v; kbe_snprintf(buf, MAX_BUF, "%d", v);
#else
			float v; (*s) >> v; kbe_snprintf(buf, MAX_BUF, "%f", v);
#endif
			values.push_back(std::make_pair(db_item_names_[i], buf));
		}
	}
	else
	{
		ERROR_MSG(fmt::format("EntityTableItemPostgresql::readSqlValues: unsupported type, table={}, item={}, type={}\n",
			tableName(), itemName(), dataSType_));
		return false;
	}

	return true;
}

bool EntityTableItemPostgresql::writeSimpleItem(DBInterface* pdbi, DBID dbid, MemoryStream* s)
{
	DBItemValues values;
	if (!readSqlValues(pdbi, s, values))
		return false;

	if (values.empty())
		return true;

	postgresql::SqlStatementUpdate sqlcmd(pdbi, tableName(), dbid, values);
	return sqlcmd.query();
}

bool EntityTableItemPostgresql::readFixedDictWriteValues(DBInterface* pdbi, DBID dbid, MemoryStream* s,
	ScriptDefModule* pModule, std::vector<std::pair<std::string, std::string> >& values)
{
	KBE_ASSERT(dataSType_ == "FIXED_DICT");

	FIXEDDICT_KEYTYPES::iterator iter = keyTypes_.begin();
	for (; iter != keyTypes_.end(); ++iter)
	{
		EntityTableItemPostgresql* pItem = static_cast<EntityTableItemPostgresql*>(iter->second.get());

		/*
			固定字典只是属性的组织方式，不代表所有子项都在当前表里。
			如果子项本身需要拆表，直接让子项按自己的规则消费流并写入子表。
		*/
		if (pItem->dataSType_ == "ARRAY" || pItem->dataSType_ == "ENTITY_COMPONENT")
		{
			if (!pItem->writeItem(pdbi, dbid, s, pModule))
				return false;

			continue;
		}

		if (pItem->dataSType_ == "FIXED_DICT")
		{
			if (!pItem->readFixedDictWriteValues(pdbi, dbid, s, pModule, values))
				return false;

			continue;
		}

		if (!pItem->readSqlValues(pdbi, s, values))
			return false;
	}

	return true;
}

bool EntityTableItemPostgresql::writeFixedDictItem(DBInterface* pdbi, DBID dbid, MemoryStream* s, ScriptDefModule* pModule)
{
	DBItemValues values;
	if (!readFixedDictWriteValues(pdbi, dbid, s, pModule, values))
		return false;

	if (values.empty())
		return true;

	postgresql::SqlStatementUpdate sqlcmd(pdbi, tableName(), dbid, values);
	return sqlcmd.query();
}

bool EntityTableItemPostgresql::writeItem(DBInterface* pdbi, DBID dbid, MemoryStream* s, ScriptDefModule* pModule)
{
	if (dataSType_ == "ARRAY")
	{
		ArraySize size = 0;
		(*s) >> size;

		if (pChildTable_)
		{
			if (pChildTable_->tableFixedOrderItems().empty())
				return true;

			if (!static_cast<EntityTablePostgresql*>(pChildTable_)->removeChildRowsByParentID(pdbi, dbid))
				return false;

			for (ArraySize i = 0; i < size; ++i)
			{
				DBID childDBID = static_cast<EntityTablePostgresql*>(pChildTable_)->insertChildRow(pdbi, dbid);
				if (childDBID == 0 || !static_cast<EntityTablePostgresql*>(pChildTable_)->writeFixedOrderItems(pdbi, childDBID, s, pModule))
					return false;
			}
		}

		return true;
	}

	if (dataSType_ == "ENTITY_COMPONENT")
	{
		if (!pChildTable_)
			return true;

		if (pChildTable_->tableFixedOrderItems().empty())
			return true;

		if (!static_cast<EntityTablePostgresql*>(pChildTable_)->removeChildRowsByParentID(pdbi, dbid))
			return false;

		DBID childDBID = static_cast<EntityTablePostgresql*>(pChildTable_)->insertChildRow(pdbi, dbid);
		return childDBID > 0 && static_cast<EntityTablePostgresql*>(pChildTable_)->writeFixedOrderItems(pdbi, childDBID, s, pModule);
	}

	if (dataSType_ == "FIXED_DICT")
		return writeFixedDictItem(pdbi, dbid, s, pModule);

	return writeSimpleItem(pdbi, dbid, s);
}

bool EntityTableItemPostgresql::appendDefaultValue(MemoryStream* s, const DataType* pDataType)
{
	if (pPropertyDescription_ && pParentTableItem_ == NULL && pPropertyDescription_->getDataType() == pDataType)
	{
		const_cast<PropertyDescription*>(pPropertyDescription_)->addPersistentToStream(s, NULL);
		return true;
	}

	if (pDataType)
	{
		PyObject* pyValue = const_cast<DataType*>(pDataType)->parseDefaultStr(defaultVal_);
		if (pyValue == NULL)
			return false;
		const_cast<DataType*>(pDataType)->addToStream(s, pyValue);
		Py_DECREF(pyValue);
	}

	return true;
}

bool EntityTableItemPostgresql::appendQueryResultValue(DBInterface* pdbi, DBID dbid, MemoryStream* s, ScriptDefModule* pModule,
	postgresql::SqlStatementQuery& querycmd, int& columnIndex)
{
	if (dataSType_ == "FIXED_DICT")
	{
		FIXEDDICT_KEYTYPES::iterator iter = keyTypes_.begin();
		for (; iter != keyTypes_.end(); ++iter)
		{
			if (!static_cast<EntityTableItemPostgresql*>(iter->second.get())->appendQueryResultValue(pdbi, dbid, s, pModule, querycmd, columnIndex))
				return false;
		}

		return true;
	}

	if (dataSType_ == "ARRAY" || dataSType_ == "ENTITY_COMPONENT")
		return queryTable(pdbi, dbid, s, pModule);

	if (dataSType_ == "ENTITYCALL")
		return true;

	if (db_item_names_.empty())
		return appendDefaultValue(s, pDataType_);

	int firstColumn = columnIndex;
	for (size_t i = 0; i < db_item_names_.size(); ++i)
	{
		if (querycmd.isNull(0, firstColumn + static_cast<int>(i)))
		{
			columnIndex += static_cast<int>(db_item_names_.size());
			return appendDefaultValue(s, pDataType_);
		}
	}

	std::stringstream stream;
	if (dataSType_ == "INT8")
	{
		int32 v = atoi(querycmd.value(0, firstColumn)); (*s) << static_cast<int8>(v);
	}
	else if (dataSType_ == "INT16")
	{
		int16 v = static_cast<int16>(atoi(querycmd.value(0, firstColumn))); (*s) << v;
	}
	else if (dataSType_ == "INT32")
	{
		int32 v = atoi(querycmd.value(0, firstColumn)); (*s) << v;
	}
	else if (dataSType_ == "INT64")
	{
		int64 v; StringConv::str2value(v, querycmd.value(0, firstColumn)); (*s) << v;
	}
	else if (dataSType_ == "UINT8")
	{
		uint32 v = static_cast<uint32>(atoi(querycmd.value(0, firstColumn))); (*s) << static_cast<uint8>(v);
	}
	else if (dataSType_ == "UINT16")
	{
		uint16 v = static_cast<uint16>(atoi(querycmd.value(0, firstColumn))); (*s) << v;
	}
	else if (dataSType_ == "UINT32")
	{
		uint32 v; StringConv::str2value(v, querycmd.value(0, firstColumn)); (*s) << v;
	}
	else if (dataSType_ == "UINT64")
	{
		uint64 v; StringConv::str2value(v, querycmd.value(0, firstColumn)); (*s) << v;
	}
	else if (dataSType_ == "FLOAT")
	{
		float v = static_cast<float>(atof(querycmd.value(0, firstColumn))); (*s) << v;
	}
	else if (dataSType_ == "DOUBLE")
	{
		double v = atof(querycmd.value(0, firstColumn)); (*s) << v;
	}
	else if (dataSType_ == "STRING")
	{
		(*s) << std::string(querycmd.value(0, firstColumn), querycmd.length(0, firstColumn));
	}
	else if (dataSType_ == "UNICODE")
	{
		s->appendBlob(querycmd.value(0, firstColumn), static_cast<ArraySize>(querycmd.length(0, firstColumn)));
	}
	else if (dataSType_ == "BLOB" || dataSType_ == "PYTHON" || dataSType_ == "PY_DICT" || dataSType_ == "PY_TUPLE" || dataSType_ == "PY_LIST")
	{
		size_t size = 0;
		unsigned char* data = PQunescapeBytea(reinterpret_cast<const unsigned char*>(querycmd.value(0, firstColumn)), &size);
		if (data == NULL || size > static_cast<size_t>(std::numeric_limits<ArraySize>::max()))
		{
			if (data)
				PQfreemem(data);

			ERROR_MSG(fmt::format("EntityTableItemPostgresql::querySimpleItem: bytea decode failed, table={}, item={}, dbid={}\n",
				tableName(), itemName(), dbid));
			return false;
		}

		s->appendBlob(reinterpret_cast<const char*>(data), static_cast<ArraySize>(size));
		PQfreemem(data);
	}
	else if (dataSType_ == "VECTOR2" || dataSType_ == "VECTOR3" || dataSType_ == "VECTOR4")
	{
		int count = dataSType_ == "VECTOR2" ? 2 : (dataSType_ == "VECTOR3" ? 3 : 4);
		for (int i = 0; i < count; ++i)
		{
#ifdef CLIENT_NO_FLOAT
			int32 v = atoi(querycmd.value(0, firstColumn + i)); (*s) << v;
#else
			float v = static_cast<float>(atof(querycmd.value(0, firstColumn + i))); (*s) << v;
#endif
		}
	}

	columnIndex += static_cast<int>(db_item_names_.size());
	return true;
}

bool EntityTableItemPostgresql::querySimpleItem(DBInterface* pdbi, DBID dbid, MemoryStream* s)
{
	if (dataSType_ == "FIXED_DICT")
	{
		FIXEDDICT_KEYTYPES::iterator iter = keyTypes_.begin();
		for (; iter != keyTypes_.end(); ++iter)
		{
			if (!iter->second->queryTable(pdbi, dbid, s, NULL))
				return false;
		}

		return true;
	}

	if (dataSType_ == "ENTITYCALL")
		return true;

	if (db_item_names_.empty())
		return appendDefaultValue(s, pDataType_);

	postgresql::SqlStatementQuery querycmd(pdbi, tableName(), db_item_names_, postgresql::whereID(dbid), postgresql::orderByIDLimit(1));
	if (!querycmd.query())
	{
		ERROR_MSG(fmt::format("EntityTableItemPostgresql::querySimpleItem: query failed, table={}, item={}, dbid={}, error={}\n",
			tableName(), itemName(), dbid, pdbi->getstrerror()));
		return false;
	}
	if (querycmd.rows() == 0)
	{
		ERROR_MSG(fmt::format("EntityTableItemPostgresql::querySimpleItem: row not found, table={}, item={}, dbid={}\n",
			tableName(), itemName(), dbid));
		return false;
	}

	int columnIndex = 0;
	return appendQueryResultValue(pdbi, dbid, s, NULL, querycmd, columnIndex);
}

bool EntityTableItemPostgresql::queryTable(DBInterface* pdbi, DBID dbid, MemoryStream* s, ScriptDefModule* pModule)
{
	if (dataSType_ == "ARRAY")
	{
		if (pChildTable_ == NULL || pChildTable_->tableFixedOrderItems().empty())
		{
			ArraySize size = 0;
			(*s) << size;
			return true;
		}

		postgresql::SqlStatementQueryIDs querycmd(pdbi, pChildTable_->tableName(),
			postgresql::whereParentID(pdbi, dbid), postgresql::orderByID());
		if (!querycmd.query())
			return false;

		const std::vector<DBID>& childDBIDs = querycmd.dbids();
		ArraySize size = static_cast<ArraySize>(childDBIDs.size());
		(*s) << size;
		for (ArraySize i = 0; i < size; ++i)
		{
			if (!static_cast<EntityTablePostgresql*>(pChildTable_)->queryFixedOrderItems(pdbi, childDBIDs[i], s, pModule))
				return false;
		}

		return true;
	}

	if (dataSType_ == "ENTITY_COMPONENT")
	{
		if (pChildTable_ == NULL || pChildTable_->tableFixedOrderItems().empty())
			return true;

		postgresql::SqlStatementQueryIDs querycmd(pdbi, pChildTable_->tableName(),
			postgresql::whereParentID(pdbi, dbid), postgresql::orderByIDLimit(1));
		if (!querycmd.query())
			return false;

		const std::vector<DBID>& childDBIDs = querycmd.dbids();
		bool found = !childDBIDs.empty();
		(*s) << found;
		if (found)
		{
			if (!static_cast<EntityTablePostgresql*>(pChildTable_)->queryFixedOrderItems(pdbi, childDBIDs[0], s, pModule))
				return false;
		}

		return true;
	}

	return querySimpleItem(pdbi, dbid, s);
}

bool EntityTableItemPostgresql::removeChildRows(DBInterface* pdbi, DBID parentID)
{
	if (dataSType_ == "FIXED_DICT")
	{
		FIXEDDICT_KEYTYPES::iterator iter = keyTypes_.begin();
		for (; iter != keyTypes_.end(); ++iter)
		{
			if (!static_cast<EntityTableItemPostgresql*>(iter->second.get())->removeChildRows(pdbi, parentID))
				return false;
		}

		return true;
	}

	if ((dataSType_ != "ARRAY" && dataSType_ != "ENTITY_COMPONENT") || pChildTable_ == NULL)
		return true;

	postgresql::SqlStatementQueryIDs querycmd(pdbi, pChildTable_->tableName(),
		postgresql::whereParentID(pdbi, parentID));
	if (!querycmd.query())
		return false;

	const std::vector<DBID>& childDBIDs = querycmd.dbids();
	for (size_t i = 0; i < childDBIDs.size(); ++i)
	{
		std::vector<EntityTableItem*>::const_iterator iter = pChildTable_->tableFixedOrderItems().begin();
		for (; iter != pChildTable_->tableFixedOrderItems().end(); ++iter)
		{
			if (!static_cast<EntityTableItemPostgresql*>((*iter))->removeChildRows(pdbi, childDBIDs[i]))
				return false;
		}
	}

	postgresql::SqlStatementDelete deletecmd(pdbi, pChildTable_->tableName(),
		postgresql::whereParentID(pdbi, parentID));
	return deletecmd.query();
}

EntityTablePostgresql::EntityTablePostgresql(EntityTables* pEntityTables) :
	EntityTable(pEntityTables)
{
}

EntityTablePostgresql::~EntityTablePostgresql()
{
}

bool EntityTablePostgresql::initialize(ScriptDefModule* sm, std::string name)
{
	tableName(name);

	ScriptDefModule::PROPERTYDESCRIPTION_MAP& pdescrsMap = sm->getPersistentPropertyDescriptions();
	ScriptDefModule::PROPERTYDESCRIPTION_MAP::const_iterator iter = pdescrsMap.begin();
	for (; iter != pdescrsMap.end(); ++iter)
	{
		PropertyDescription* pdescrs = iter->second;
		if (!sm->hasCell() && pdescrs->getDataType()->type() == DATA_TYPE_ENTITY_COMPONENT && !pdescrs->hasBase())
			continue;

		EntityTableItem* pETItem = createItem(pdescrs->getDataType()->getName(), pdescrs->getDefaultValStr());
		pETItem->pParentTable(this);
		pETItem->utype(pdescrs->getUType());
		pETItem->tableName(this->tableName());

		if (!pETItem->initialize(pdescrs, pdescrs->getDataType(), pdescrs->getName()))
		{
			delete pETItem;
			return false;
		}

		addItem(pETItem);
	}

	if (sm->hasCell())
	{
		ENTITY_PROPERTY_UID posuid = ENTITY_BASE_PROPERTY_UTYPE_POSITION_XYZ;
		ENTITY_PROPERTY_UID diruid = ENTITY_BASE_PROPERTY_UTYPE_DIRECTION_ROLL_PITCH_YAW;
		Network::FixedMessages::MSGInfo* msgInfo = Network::FixedMessages::getSingleton().isFixed("Property::position");
		if (msgInfo != NULL)
			posuid = msgInfo->msgid;
		msgInfo = Network::FixedMessages::getSingleton().isFixed("Property::direction");
		if (msgInfo != NULL)
			diruid = msgInfo->msgid;

		DataType* pVector3Type = DataTypes::getDataType("VECTOR3");
		EntityTableItem* pETItem = createItem("VECTOR3", "");
		pETItem->pParentTable(this);
		pETItem->utype(posuid);
		pETItem->tableName(this->tableName());
		if (!pETItem->initialize(NULL, pVector3Type, "position"))
		{
			delete pETItem;
			return false;
		}
		addItem(pETItem);

		if (posuid != ENTITY_BASE_PROPERTY_UTYPE_POSITION_XYZ)
			tableItems_[ENTITY_BASE_PROPERTY_UTYPE_POSITION_XYZ] = tableItems_[posuid];

		pETItem = createItem("VECTOR3", "");
		pETItem->pParentTable(this);
		pETItem->utype(diruid);
		pETItem->tableName(this->tableName());
		if (!pETItem->initialize(NULL, pVector3Type, "direction"))
		{
			delete pETItem;
			return false;
		}
		addItem(pETItem);

		if (diruid != ENTITY_BASE_PROPERTY_UTYPE_DIRECTION_ROLL_PITCH_YAW)
			tableItems_[ENTITY_BASE_PROPERTY_UTYPE_DIRECTION_ROLL_PITCH_YAW] = tableItems_[diruid];
	}

	return true;
}

bool EntityTablePostgresql::syncToDB(DBInterface* pdbi)
{
	if (hasSync())
		return true;

	std::string exItems;
	if (isChild())
		exItems = fmt::format(", {} BIGINT NOT NULL", columnSqlName(pdbi, TABLE_PARENTID_CONST_STR).c_str());

	std::string sql = fmt::format("CREATE TABLE IF NOT EXISTS {} (id BIGSERIAL PRIMARY KEY, {} SMALLINT DEFAULT 0{})",
		tableSqlName(pdbi, tableName()).c_str(),
		columnSqlName(pdbi, TABLE_ITEM_PERFIX "_" TABLE_AUTOLOAD_CONST_STR).c_str(),
		exItems.c_str());

	if (!pdbi->query(sql))
		return false;

	std::vector<std::string> dbTableItemNames;
	std::string sqlTableName = std::string(ENTITY_TABLE_PERFIX "_") + tableName();
	if (!pdbi->getTableItemNames(sqlTableName.c_str(), dbTableItemNames))
		return false;

	PostgreSQLColumnInfos columnInfos;
	if (!loadPostgresqlColumnInfos(pdbi, sqlTableName, columnInfos))
		return false;

	std::vector<EntityTableItem*>::iterator iter = tableFixedOrderItems_.begin();
	for (; iter != tableFixedOrderItems_.end(); ++iter)
	{
		if (!(*iter)->syncToDB(pdbi, &columnInfos))
			return false;
	}

	/*
		对齐 MySQL 后端的字段清理逻辑：def 里删掉普通属性后，数据库里对应的列也要删掉。
		id、sm_autoLoad、parentID 是框架列，不能按属性删除；FIXED_DICT 的子字段会通过
		isSameKey() 逐列识别，组件/数组是独立子表，表级清理由 EntityTables::syncToDB() 统一处理。
	*/
	for (size_t i = 0; i < dbTableItemNames.size(); ++i)
	{
		std::string columnName = dbTableItemNames[i];
		if (columnName == TABLE_ID_CONST_STR ||
			columnName == TABLE_ITEM_PERFIX "_" TABLE_AUTOLOAD_CONST_STR ||
			columnName == TABLE_PARENTID_CONST_STR)
		{
			continue;
		}

		bool found = false;
		EntityTable::TABLEITEM_MAP::iterator titer = tableItems_.begin();
		for (; titer != tableItems_.end(); ++titer)
		{
			if (titer->second->isSameKey(columnName))
			{
				found = true;
				break;
			}
		}

		if (!found && !pdbi->dropEntityTableItemFromDB(sqlTableName.c_str(), columnName.c_str()))
			return false;
	}

	std::vector<std::string> dbItemNames;
	collectDBItemNames(dbItemNames);

	/*
		这里注册的是表级 SQL 模板，不直接执行。
		模板里保存已经按 PostgreSQL 方言处理过的表名和列名，后面如果要把整行查询、
		prepared statement 或批量写入收敛起来，不需要再从 EntityTableItem 重新扫字段。
	*/
	postgresql::EntitySqlStatementMapping& mapping = postgresql::EntitySqlStatementMapping::getSingleton();
	if (mapping.findQuerySqlStatement(tableName()) == NULL)
		mapping.addQuerySqlStatement(tableName(), new postgresql::SqlStatement(pdbi, tableName(), dbItemNames));
	if (mapping.findInsertSqlStatement(tableName()) == NULL)
		mapping.addInsertSqlStatement(tableName(), new postgresql::SqlStatement(pdbi, tableName(), dbItemNames));
	if (mapping.findUpdateSqlStatement(tableName()) == NULL)
		mapping.addUpdateSqlStatement(tableName(), new postgresql::SqlStatement(pdbi, tableName(), dbItemNames));

	if (!syncIndexToDB(pdbi))
		return false;

	sync_ = true;
	return true;
}

bool EntityTablePostgresql::syncIndexToDB(DBInterface* pdbi)
{
	std::string idxName = std::string("idx_") + ENTITY_TABLE_PERFIX "_" + tableName() + "_" TABLE_ITEM_PERFIX "_" TABLE_AUTOLOAD_CONST_STR;
	std::string sql;
	std::string rawTableName = std::string(ENTITY_TABLE_PERFIX "_") + tableName();
	std::vector<std::string> indexNames;

	if (!loadPostgresqlIndexNames(pdbi, rawTableName, indexNames))
		return false;

	if (!stringVectorContains(indexNames, idxName))
	{
		sql = fmt::format("CREATE INDEX IF NOT EXISTS {} ON {} ({})",
			columnSqlName(pdbi, idxName.c_str()).c_str(),
			tableSqlName(pdbi, tableName()).c_str(),
			columnSqlName(pdbi, TABLE_ITEM_PERFIX "_" TABLE_AUTOLOAD_CONST_STR).c_str());

		if (!pdbi->query(sql))
			return false;
	}

	if (isChild())
	{
		idxName = std::string("idx_") + ENTITY_TABLE_PERFIX "_" + tableName() + "_" TABLE_PARENTID_CONST_STR;
		if (!stringVectorContains(indexNames, idxName))
		{
			sql = fmt::format("CREATE INDEX IF NOT EXISTS {} ON {} ({})",
				columnSqlName(pdbi, idxName.c_str()).c_str(),
				tableSqlName(pdbi, tableName()).c_str(),
				columnSqlName(pdbi, TABLE_PARENTID_CONST_STR).c_str());

			if (!pdbi->query(sql))
				return false;
		}
	}

	/*
		MySQL 版本会按 def 里的 indexType() 同步普通字段索引。PostgreSQL 之前只建了
		autoLoad/parentID 两个内部索引，业务字段上的 UNIQUE/INDEX 会被悄悄忽略。
		这里不主动删除旧索引，先做到“定义里要求的索引一定存在”；删索引涉及线上数据和查询计划，
		应该由迁移脚本显式处理，更稳一点。
	*/
	EntityTable::TABLEITEM_MAP::iterator iter = tableItems_.begin();
	for (; iter != tableItems_.end(); ++iter)
	{
		if (strlen(iter->second->indexType()) == 0)
			continue;

		std::vector<std::string> columns;
		static_cast<EntityTableItemPostgresql*>(iter->second.get())->collectDBItemNames(columns);
		if (columns.empty())
			continue;

		std::string sqlColumns;
		for (size_t i = 0; i < columns.size(); ++i)
		{
			if (i > 0)
				sqlColumns += ",";

			sqlColumns += columnSqlName(pdbi, columns[i].c_str());
		}

		idxName = fmt::format("{}_{}_{}", kbe_stricmp(iter->second->indexType(), "UNIQUE") == 0 ? "uk" : "idx",
			ENTITY_TABLE_PERFIX "_" + std::string(tableName()), iter->second->itemName());
		if (stringVectorContains(indexNames, idxName))
			continue;

		sql = fmt::format("CREATE {} IF NOT EXISTS {} ON {} ({})",
			indexSqlType(iter->second->indexType()).c_str(),
			columnSqlName(pdbi, idxName.c_str()).c_str(),
			tableSqlName(pdbi, tableName()).c_str(),
			sqlColumns.c_str());

		if (!pdbi->query(sql))
			return false;
	}

	return true;
}

EntityTableItem* EntityTablePostgresql::createItem(std::string type, std::string defaultVal)
{
	return new EntityTableItemPostgresql(type, defaultVal);
}

DBID EntityTablePostgresql::insertChildRow(DBInterface* pdbi, DBID parentID)
{
	DBItemValues values;
	values.push_back(std::make_pair(TABLE_PARENTID_CONST_STR, fmt::format("{}", parentID)));

	postgresql::SqlStatementInsert sqlcmd(pdbi, tableName(), values);
	if (!sqlcmd.query())
		return 0;

	return sqlcmd.dbid();
}

bool EntityTablePostgresql::removeChildRowsByParentID(DBInterface* pdbi, DBID parentID)
{
	postgresql::SqlStatementQueryIDs querycmd(pdbi, tableName(),
		postgresql::whereParentID(pdbi, parentID));
	if (!querycmd.query())
		return false;

	const std::vector<DBID>& childDBIDs = querycmd.dbids();
	for (size_t i = 0; i < childDBIDs.size(); ++i)
	{
		std::vector<EntityTableItem*>::iterator iter = tableFixedOrderItems_.begin();
		for (; iter != tableFixedOrderItems_.end(); ++iter)
		{
			if (!static_cast<EntityTableItemPostgresql*>((*iter))->removeChildRows(pdbi, childDBIDs[i]))
				return false;
		}
	}

	postgresql::SqlStatementDelete deletecmd(pdbi, tableName(),
		postgresql::whereParentID(pdbi, parentID));
	return deletecmd.query();
}

bool EntityTablePostgresql::writeFixedOrderItems(DBInterface* pdbi, DBID dbid, MemoryStream* s, ScriptDefModule* pModule)
{
	std::vector<EntityTableItem*>::iterator iter = tableFixedOrderItems_.begin();
	for (; iter != tableFixedOrderItems_.end(); ++iter)
	{
		if (!(*iter)->writeItem(pdbi, dbid, s, pModule))
			return false;
	}

	return true;
}

bool EntityTablePostgresql::queryFixedOrderItems(DBInterface* pdbi, DBID dbid, MemoryStream* s, ScriptDefModule* pModule)
{
	postgresql::EntitySqlStatementMapping& mapping = postgresql::EntitySqlStatementMapping::getSingleton();
	postgresql::SqlStatement* pQueryStatement = mapping.findQuerySqlStatement(tableName());
	if (pQueryStatement != NULL && !pQueryStatement->columns().empty())
	{
		postgresql::SqlStatementQuery querycmd(pdbi, *pQueryStatement, postgresql::whereID(dbid), postgresql::orderByIDLimit(1));
		if (!querycmd.query())
		{
			ERROR_MSG(fmt::format("EntityTablePostgresql::queryFixedOrderItems: query failed, table={}, dbid={}, error={}\n",
				tableName(), dbid, pdbi->getstrerror()));
			return false;
		}
		if (querycmd.rows() == 0)
		{
			ERROR_MSG(fmt::format("EntityTablePostgresql::queryFixedOrderItems: row not found, table={}, dbid={}\n",
				tableName(), dbid));
			return false;
		}

		/*
			mapping 的列模板从表公共列开始保存，真正写入实体流时只消费实体属性列。
			主表跳过 sm_autoLoad，子表还要再跳过 parentID。
		*/
		int columnIndex = isChild() ? 2 : 1;
		std::vector<EntityTableItem*>::iterator iter = tableFixedOrderItems_.begin();
		for (; iter != tableFixedOrderItems_.end(); ++iter)
		{
			if (!static_cast<EntityTableItemPostgresql*>((*iter))->appendQueryResultValue(pdbi, dbid, s, pModule, querycmd, columnIndex))
				return false;
		}

		return true;
	}

	std::vector<EntityTableItem*>::iterator iter = tableFixedOrderItems_.begin();
	for (; iter != tableFixedOrderItems_.end(); ++iter)
	{
		if (!(*iter)->queryTable(pdbi, dbid, s, pModule))
			return false;
	}

	return true;
}

void EntityTablePostgresql::collectDBItemNames(std::vector<std::string>& values) const
{
	values.push_back(TABLE_ITEM_PERFIX "_" TABLE_AUTOLOAD_CONST_STR);

	if (isChild())
		values.push_back(TABLE_PARENTID_CONST_STR);

	std::vector<EntityTableItem*>::const_iterator iter = tableFixedOrderItems_.begin();
	for (; iter != tableFixedOrderItems_.end(); ++iter)
		static_cast<EntityTableItemPostgresql*>((*iter))->collectDBItemNames(values);
}

DBID EntityTablePostgresql::writeTable(DBInterface* pdbi, DBID dbid, int8 shouldAutoLoad, MemoryStream* s, ScriptDefModule* pModule)
{
	KBE_ASSERT(pModule && s);

	postgresql::DBTransaction transaction(pdbi);
	if (!transaction.active())
		return 0;

	if (dbid == 0)
	{
		DBItemValues values;
		values.push_back(std::make_pair(TABLE_ITEM_PERFIX "_" TABLE_AUTOLOAD_CONST_STR, fmt::format("{}", shouldAutoLoad > 0 ? 1 : 0)));

		postgresql::SqlStatementInsert sqlcmd(pdbi, tableName(), values);
		if (!sqlcmd.query())
			return 0;

		dbid = sqlcmd.dbid();
	}

	while (s->length() > 0)
	{
		ENTITY_PROPERTY_UID pid;
		ENTITY_PROPERTY_UID child_pid;
		(*s) >> pid >> child_pid;

		// 空账号实体模板可能只带一个 0,0 作为占位，末尾没有实际属性数据时直接跳过。
		if (pid == 0 && child_pid == 0 && s->length() == 0)
			break;

		EntityTableItem* pTableItem = findItem(child_pid);
		if (pTableItem == NULL)
		{
			ERROR_MSG(fmt::format("EntityTablePostgresql::writeTable: not found item, table={}, parent={}, child={}, items={}\n",
				tableName(), pid, child_pid, describeTableItemUtypes(tableItems_)));
			return 0;
		}

		if (!pTableItem->writeItem(pdbi, dbid, s, pModule))
			return 0;
	}

	if (shouldAutoLoad > -1)
		entityShouldAutoLoad(pdbi, dbid, shouldAutoLoad > 0);

	if (!transaction.commit())
		return 0;

	return dbid;
}

bool EntityTablePostgresql::removeEntity(DBInterface* pdbi, DBID dbid, ScriptDefModule* pModule)
{
	KBE_ASSERT(pModule && dbid > 0);

	postgresql::DBTransaction transaction(pdbi);
	if (!transaction.active())
		return false;

	std::vector<EntityTableItem*>::iterator iter = tableFixedOrderItems_.begin();
	for (; iter != tableFixedOrderItems_.end(); ++iter)
	{
		if (!static_cast<EntityTableItemPostgresql*>((*iter))->removeChildRows(pdbi, dbid))
			return false;
	}

	postgresql::SqlStatementDelete deletecmd(pdbi, pModule->getName(), postgresql::whereID(dbid));
	if (!deletecmd.query())
		return false;

	return transaction.commit();
}

bool EntityTablePostgresql::queryTable(DBInterface* pdbi, DBID dbid, MemoryStream* s, ScriptDefModule* pModule)
{
	KBE_ASSERT(pModule && s && dbid > 0);

	postgresql::SqlStatementQueryIDs querycmd(pdbi, pModule->getName(), postgresql::whereID(dbid), postgresql::orderByIDLimit(1));
	if (!querycmd.query())
		return false;

	return querycmd.found() && queryFixedOrderItems(pdbi, dbid, s, pModule);
}

void EntityTablePostgresql::entityShouldAutoLoad(DBInterface* pdbi, DBID dbid, bool shouldAutoLoad)
{
	if (dbid == 0)
		return;

	DBItemValues values;
	values.push_back(std::make_pair(TABLE_ITEM_PERFIX "_" TABLE_AUTOLOAD_CONST_STR, fmt::format("{}", shouldAutoLoad ? 1 : 0)));

	postgresql::SqlStatementUpdate sqlcmd(pdbi, tableName(), dbid, values);
	sqlcmd.query();
}

void EntityTablePostgresql::queryAutoLoadEntities(DBInterface* pdbi, ScriptDefModule* pModule,
	ENTITY_ID start, ENTITY_ID end, std::vector<DBID>& outs)
{
	if (end <= start)
		return;

	postgresql::SqlStatementQueryIDs querycmd(pdbi, pModule->getName(),
		fmt::format("{}=1", columnSqlName(pdbi, TABLE_ITEM_PERFIX "_" TABLE_AUTOLOAD_CONST_STR)),
		postgresql::orderByIDLimitOffset(end - start, start));

	if (!querycmd.query())
		return;

	const std::vector<DBID>& dbids = querycmd.dbids();
	outs.insert(outs.end(), dbids.begin(), dbids.end());
}

}
