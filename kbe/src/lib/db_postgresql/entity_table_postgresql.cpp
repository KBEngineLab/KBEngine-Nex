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

bool EntityTableItemPostgresql::syncOneColumn(DBInterface* pdbi, const std::string& columnName, const std::string& columnType)
{
	std::string sql = fmt::format("ALTER TABLE {} ADD COLUMN IF NOT EXISTS {} {}",
		tableSqlName(pdbi, tableName()).c_str(),
		columnSqlName(pdbi, columnName.c_str()).c_str(),
		columnType.c_str());

	return pdbi->query(sql);
}

bool EntityTableItemPostgresql::syncToDB(DBInterface* pdbi, void* /*pData*/)
{
	if (dataSType_ == "ARRAY" || dataSType_ == "ENTITY_COMPONENT")
		return true;

	if (dataSType_ == "FIXED_DICT")
	{
		FIXEDDICT_KEYTYPES::iterator iter = keyTypes_.begin();
		for (; iter != keyTypes_.end(); ++iter)
		{
			if (!iter->second->syncToDB(pdbi))
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
		if (!syncOneColumn(pdbi, db_item_names_[i], type))
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
	if (dataSType_ == "FIXED_DICT")
	{
		FIXEDDICT_KEYTYPES::iterator iter = keyTypes_.begin();
		for (; iter != keyTypes_.end(); ++iter)
		{
			if (!static_cast<EntityTableItemPostgresql*>(iter->second.get())->readSqlValues(pdbi, s, values))
				return false;
		}

		return true;
	}

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

	std::vector<EntityTableItem*>::iterator iter = tableFixedOrderItems_.begin();
	for (; iter != tableFixedOrderItems_.end(); ++iter)
	{
		if (!(*iter)->syncToDB(pdbi))
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
	std::string sql = fmt::format("CREATE INDEX IF NOT EXISTS {} ON {} ({})",
		columnSqlName(pdbi, idxName.c_str()).c_str(),
		tableSqlName(pdbi, tableName()).c_str(),
		columnSqlName(pdbi, TABLE_ITEM_PERFIX "_" TABLE_AUTOLOAD_CONST_STR).c_str());

	if (!pdbi->query(sql))
		return false;

	if (isChild())
	{
		idxName = std::string("idx_") + ENTITY_TABLE_PERFIX "_" + tableName() + "_" TABLE_PARENTID_CONST_STR;
		sql = fmt::format("CREATE INDEX IF NOT EXISTS {} ON {} ({})",
			columnSqlName(pdbi, idxName.c_str()).c_str(),
			tableSqlName(pdbi, tableName()).c_str(),
			columnSqlName(pdbi, TABLE_PARENTID_CONST_STR).c_str());

		return pdbi->query(sql);
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

	bool wroteAnyItem = false;
	while (s->length() > 0)
	{
		size_t itemStart = s->rpos();
		ENTITY_PROPERTY_UID pid;
		ENTITY_PROPERTY_UID child_pid;
		(*s) >> pid >> child_pid;

		// 空账号实体模板可能只带一个 0,0 作为占位，末尾没有实际属性数据时直接跳过。
		if (pid == 0 && child_pid == 0 && s->length() == 0)
			break;

		EntityTableItem* pTableItem = findItem(child_pid);
		if (pTableItem == NULL)
		{
			/*
				账号默认实体流有两种来源：新的流会带 parent/child uid，旧模板可能只按
				表字段顺序写属性值。固定顺序值流的开头经常是数组 size=0，按 uid 读出来就是 0,0。
			*/
			if (!wroteAnyItem && pid == 0 && child_pid == 0)
			{
				KBE_ASSERT(itemStart <= static_cast<size_t>(std::numeric_limits<int>::max()));
				s->rpos(static_cast<int>(itemStart));
				if (!writeFixedOrderItems(pdbi, dbid, s, pModule))
					return 0;
				break;
			}

			ERROR_MSG(fmt::format("EntityTablePostgresql::writeTable: not found item, table={}, parent={}, child={}, items={}\n",
				tableName(), pid, child_pid, describeTableItemUtypes(tableItems_)));
			return 0;
		}

		if (!pTableItem->writeItem(pdbi, dbid, s, pModule))
			return 0;

		wroteAnyItem = true;
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
