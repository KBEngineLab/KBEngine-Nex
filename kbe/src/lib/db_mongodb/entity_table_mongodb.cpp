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

#include "entity_table_mongodb.h"
#include "kbe_table_mongodb.h"
#include "mongo_cursor_guard.h"
#include "entitydef/scriptdef_module.h"
#include "entitydef/property.h"
#include "db_interface/db_interface.h"
#include "db_interface/entity_table.h"
#include "network/fixed_messages.h"

#ifndef CODE_INLINE
#include "entity_table_mongodb.inl"
#endif

namespace KBEngine {
	namespace
	{
		bool bson_iter_to_dbid(const bson_iter_t* iter, DBID& dbid)
		{
			if (BSON_ITER_HOLDS_INT32(iter))
			{
				dbid = static_cast<DBID>(bson_iter_int32(iter));
				return true;
			}

			if (BSON_ITER_HOLDS_INT64(iter))
			{
				dbid = static_cast<DBID>(bson_iter_int64(iter));
				return true;
			}

			return false;
		}
	}

	EntityTableMongodb::EntityTableMongodb(EntityTables* pEntityTables) :
		EntityTable(pEntityTables)
	{
	}

	EntityTableMongodb::~EntityTableMongodb()
	{

	}

	bool EntityTableMongodb::initialize(ScriptDefModule* sm, std::string name)
	{
		// 实体模块名直接映射为集合名，保持各数据库后端的逻辑表名一致。
		// Map the entity module name directly to the collection name to keep logical table names consistent across backends.
		tableName(name);

		// 只为持久化属性创建字段适配器，非持久化属性不进入数据库文档。
		// Create field adapters only for persistent properties so transient properties never enter database documents.
		ScriptDefModule::PROPERTYDESCRIPTION_MAP& pdescrsMap = sm->getPersistentPropertyDescriptions();
		ScriptDefModule::PROPERTYDESCRIPTION_MAP::const_iterator iter = pdescrsMap.begin();
		std::string hasUnique = "";

		for (; iter != pdescrsMap.end(); ++iter)
		{
			PropertyDescription* pdescrs = iter->second;

			// 无 Cell 部分的实体忽略仅属于 Cell 的组件字段，避免读取数据流中不存在的值。
			// Entities without a Cell part skip component fields owned only by Cell to avoid reading values absent from the stream.
			if (!sm->hasCell())
			{
				if (pdescrs->getDataType()->type() == DATA_TYPE_ENTITY_COMPONENT && !pdescrs->hasBase())
					continue;
			}


			EntityTableItem* pETItem = this->createItem(pdescrs->getDataType()->getName(), pdescrs->getDefaultValStr());

			pETItem->pParentTable(this);
			pETItem->utype(pdescrs->getUType());
			pETItem->tableName(this->tableName());

			bool ret = pETItem->initialize(pdescrs, pdescrs->getDataType(), pdescrs->getName());

			if (!ret)
			{
				delete pETItem;
				return false;
			}

			tableItems_[pETItem->utype()].reset(pETItem);
			tableFixedOrderItems_.push_back(pETItem);
		}

		// 空间方向和位置由引擎隐式持久化，只有存在 Cell 部分时才加入固定字段顺序。
		// Direction and position are engine-managed persistent fields and join the fixed order only when a Cell part exists.
		if (sm->hasCell())
		{
			ENTITY_PROPERTY_UID posuid = ENTITY_BASE_PROPERTY_UTYPE_POSITION_XYZ;
			ENTITY_PROPERTY_UID diruid = ENTITY_BASE_PROPERTY_UTYPE_DIRECTION_ROLL_PITCH_YAW;

			Network::FixedMessages::MSGInfo* msgInfo =
				Network::FixedMessages::getSingleton().isFixed("Property::position");

			if (msgInfo != NULL)
			{
				posuid = msgInfo->msgid;
				msgInfo = NULL;
			}

			msgInfo = Network::FixedMessages::getSingleton().isFixed("Property::direction");
			if (msgInfo != NULL)
			{
				diruid = msgInfo->msgid;
				msgInfo = NULL;
			}

			EntityTableItem* pETItem = this->createItem("VECTOR3", "");
			pETItem->pParentTable(this);
			pETItem->utype(posuid);
			pETItem->tableName(this->tableName());
			pETItem->itemName("position");
			tableItems_[pETItem->utype()].reset(pETItem);
			tableFixedOrderItems_.push_back(pETItem);

			pETItem = this->createItem("VECTOR3", "");
			pETItem->pParentTable(this);
			pETItem->utype(diruid);
			pETItem->tableName(this->tableName());
			pETItem->itemName("direction");
			tableItems_[pETItem->utype()].reset(pETItem);
			tableFixedOrderItems_.push_back(pETItem);
		}

		init_db_item_name();

		return true;
	}

	//-------------------------------------------------------------------------------------
	void EntityTableMongodb::init_db_item_name()
	{
		EntityTable::TABLEITEM_MAP::iterator iter = tableItems_.begin();
		for (; iter != tableItems_.end(); ++iter)
		{
			// FixedDict 子字段使用父字段前缀，避免同一文档中的键名发生冲突。
			// FixedDict child fields use their parent prefix to prevent key collisions within one document.
			std::string exstrFlag = "";
			if (iter->second->type() == TABLE_ITEM_TYPE_FIXEDDICT)
			{
				exstrFlag = iter->second->itemName();
				if (exstrFlag.size() > 0)
					exstrFlag += "_";
			}

			static_cast<EntityTableItemMongodbBase*>(iter->second.get())->init_db_item_name(exstrFlag.c_str());
		}
	}

	bool EntityTableMongodb::syncIndexToDB(DBInterface* pdbi)
	{
		// 从实体定义收集期望索引，随后与 MongoDB 当前索引做差量同步。
		// Collect expected indexes from the entity definition, then reconcile them with MongoDB's current indexes.
		std::vector<EntityTableItem*> indexs;

		EntityTable::TABLEITEM_MAP::iterator iter = tableItems_.begin();
		for (; iter != tableItems_.end(); ++iter)
		{
			if (strlen(iter->second->indexType()) == 0)
				continue;

			indexs.push_back(iter->second.get());
		}

		char name[MAX_BUF];
		kbe_snprintf(name, MAX_BUF, ENTITY_TABLE_PERFIX "_%s", tableName());

		KBEUnordered_map<std::string, std::string> currDBKeys;
		DBInterfaceMongodb* pdbiMongodb = static_cast<DBInterfaceMongodb*>(pdbi);
		std::unique_ptr<MongoCursorGuard> guard = pdbiMongodb->collectionFindIndexes(name);

		const bson_t* indexinfo;
		bson_iter_t idx_spec_iter;
		while (mongoc_cursor_next(guard->cursor(), &indexinfo))
		{
			if (bson_iter_init_find(&idx_spec_iter, indexinfo, "name") &&
				BSON_ITER_HOLDS_UTF8(&idx_spec_iter))
			{
				std::string keyname = bson_iter_utf8(&idx_spec_iter, NULL);
				if (bson_iter_init_find(&idx_spec_iter, indexinfo, "unique"))
				{

					currDBKeys[keyname] = "UNIQUE";
				}
				else
				{
					currDBKeys[keyname] = "INDEX";
				}
			}
		}

		bson_error_t cursorError = {};
		if (mongoc_cursor_error(guard->cursor(), &cursorError))
		{
			pdbiMongodb->setLastError(cursorError);
			ERROR_MSG(fmt::format("EntityTableMongodb::syncIndexToDB({}): {}\n", name, cursorError.message));
			pdbiMongodb->throwError();
		}

		std::vector<EntityTableItem*>::iterator iiter = indexs.begin();
		for (; iiter != indexs.end();)
		{
			std::string itemName = fmt::format(TABLE_ITEM_PERFIX"_{}_1", (*iiter)->itemName());
			std::string itemIndexsName = fmt::format(TABLE_ITEM_PERFIX"_{}", (*iiter)->itemName());
			KBEUnordered_map<std::string, std::string>::iterator fiter = currDBKeys.find(itemName);
			if (fiter != currDBKeys.end())
			{
				bool deleteIndex = fiter->second != (*iiter)->indexType();

				// 从映射移除已处理索引，循环结束后剩余项就是实体定义已经删除的索引。
				// Remove handled indexes from the map; remaining entries represent indexes removed from the entity definition.
				currDBKeys.erase(fiter);

				if (deleteIndex)
				{
					if (!pdbiMongodb->collectionDropIndex(name, itemName.c_str()))
						return false;
				}
				else
				{
					++iiter;
					continue;
				}
			}

			bson_t keys;
			bson_t opt;
			bson_init(&keys);
			bson_init(&opt);

			BSON_APPEND_INT32(&keys, itemIndexsName.c_str(), 1);

			if (std::string("UNIQUE") == (*iiter)->indexType())
			{
				BSON_APPEND_BOOL(&opt, "unique", true);
			}

			bool created = pdbiMongodb->collectionCreateIndex(name, &keys, &opt);
			bson_destroy(&opt);
			bson_destroy(&keys);
			if (!created)
				return false;

			++iiter;
		}

		// 删除定义中不再存在的索引，但保留 MongoDB 主键和 KBEngine 实体 ID 索引。
		// Drop indexes absent from the definition while preserving MongoDB's primary key and KBEngine's entity ID index.
		KBEUnordered_map<std::string, std::string>::iterator dbkey_iter = currDBKeys.begin();
		for (; dbkey_iter != currDBKeys.end(); ++dbkey_iter)
		{
			if (dbkey_iter->first == "_id_" || dbkey_iter->first == "id_1")
				continue;

			if (!pdbiMongodb->collectionDropIndex(name, dbkey_iter->first.c_str()))
				return false;
		}

		return true;
	}

	bool EntityTableMongodb::syncToDB(DBInterface* pdbi)
	{
		if (hasSync())
			return true;

		sync_ = true;

		char name[MAX_BUF];
		kbe_snprintf(name, MAX_BUF, ENTITY_TABLE_PERFIX "_%s", tableName());

		if (!isChild_)
		{
			// 顶层实体使用独立集合；子结构以内嵌文档保存，不创建额外集合。
			// Top-level entities use dedicated collections, while child structures remain embedded and require no extra collection.
			DBInterfaceMongodb* pdbiMongodb = static_cast<DBInterfaceMongodb*>(pdbi);
			if (!pdbiMongodb->createCollection(name))
				return false;

			bson_t keys;
			bson_t opts;
			bson_init(&keys);
			bson_init(&opts);
			BSON_APPEND_INT32(&keys, "id", 1);
			BSON_APPEND_BOOL(&opts, "unique", true);
			bool idIndexReady = pdbiMongodb->collectionCreateIndex(name, &keys, &opts);
			bson_destroy(&keys);
			bson_destroy(&opts);
			if (!idIndexReady)
				return false;

			// 只同步顶层属性索引，内嵌数组和组件随所属实体文档原子读写，不单独建表。
			// Synchronize indexes only for top-level properties; embedded arrays and components are atomically stored with their entity document.
			if (!syncIndexToDB(pdbi))
				return false;
		}

		return true;
	}

	void EntityTableMongodb::queryAutoLoadEntities(DBInterface* pdbi, ScriptDefModule* pModule,
		ENTITY_ID start, ENTITY_ID end, std::vector<DBID>& outs)
	{
		if (end <= start)
			return;

		DBInterfaceMongodb* pdbiMongodb = static_cast<DBInterfaceMongodb*>(pdbi);

		char name[MAX_BUF];
		kbe_snprintf(name, MAX_BUF, ENTITY_TABLE_PERFIX "_%s", tableName());

		bson_t query;
		bson_init(&query);
		BSON_APPEND_INT32(&query, TABLE_ITEM_PERFIX"_" TABLE_AUTOLOAD_CONST_STR, 1);

		bson_t fields;
		bson_init(&fields);
		BSON_APPEND_INT32(&fields, TABLE_ID_CONST_STR, 1);
		BSON_APPEND_INT32(&fields, "_id", 0);

		const uint32_t skip = static_cast<uint32_t>(start);
		const uint32_t limit = static_cast<uint32_t>(end - start);
		std::unique_ptr<MongoCursorGuard> guard =
			pdbiMongodb->collectionFind(name, MONGOC_QUERY_NONE, skip, limit, 0, &query, &fields, NULL);

		const bson_t* doc = NULL;
		while (mongoc_cursor_next(guard->cursor(), &doc))
		{
			bson_iter_t iter;
			DBID dbid = 0;
			if (bson_iter_init_find(&iter, doc, TABLE_ID_CONST_STR) && bson_iter_to_dbid(&iter, dbid))
				outs.push_back(dbid);
		}

		bson_error_t error = {};
		if (mongoc_cursor_error(guard->cursor(), &error))
		{
			pdbiMongodb->setLastError(error);
			ERROR_MSG(fmt::format("EntityTableMongodb::queryAutoLoadEntities: {}\n", error.message));
			pdbiMongodb->throwError();
		}

		bson_destroy(&fields);
		bson_destroy(&query);
	}

	EntityTableItem* EntityTableMongodb::createItem(std::string type, std::string defaultVal)
	{

		if (type == "INT8")
		{
			int8 v = 0;

			try
			{
				StringConv::str2value(v, defaultVal.c_str());
			}
			catch (...)
			{
				v = 0;
			}

			return new EntityTableItemMongodb_DIGIT<int8>(type, v, 1, 0);
		}
		else if (type == "INT16")
		{
			int16 v = 0;
			try
			{
				StringConv::str2value(v, defaultVal.c_str());
			}
			catch (...)
			{
				v = 0;
			}

			return new EntityTableItemMongodb_DIGIT<int16>(type, v, 2, 0);
		}
		else if (type == "INT32")
		{
			int32 v = 0;
			try
			{
				StringConv::str2value(v, defaultVal.c_str());
			}
			catch (...)
			{
				v = 0;
			}

			return new EntityTableItemMongodb_DIGIT<int32>(type, v, 4, 0);
		}
		else if (type == "INT64")
		{
			int64 v = 0;
			try
			{
				StringConv::str2value(v, defaultVal.c_str());
			}
			catch (...)
			{
				v = 0;
			}
			return new EntityTableItemMongodb_DIGIT<int64>(type, v, 8, 0);
		}
		else if (type == "UINT8")
		{
			uint8 v = 0;
			try
			{
				StringConv::str2value(v, defaultVal.c_str());
			}
			catch (...)
			{
				v = 0;
			}

			return new EntityTableItemMongodb_DIGIT<uint8>(type, v, 1, 0);
		}
		else if (type == "UINT16")
		{
			uint16 v = 0;
			try
			{
				StringConv::str2value(v, defaultVal.c_str());
			}
			catch (...)
			{
				v = 0;
			}

			return new EntityTableItemMongodb_DIGIT<uint16>(type, v, 2, 0);
		}
		else if (type == "UINT32")
		{
			uint32 v = 0;
			try
			{
				StringConv::str2value(v, defaultVal.c_str());
			}
			catch (...)
			{
				v = 0;
			}
			return new EntityTableItemMongodb_DIGIT<uint32>(type, v, 4, 0);
		}
		else if (type == "UINT64")
		{
			uint64 v = 0;
			try
			{
				StringConv::str2value(v, defaultVal.c_str());
			}
			catch (...)
			{
				v = 0;
			}

			return new EntityTableItemMongodb_DIGIT<uint64>(type, v, 8, 0);
		}
		else if (type == "FLOAT")
		{
			float v = 0;
			try
			{
				StringConv::str2value(v, defaultVal.c_str());
			}
			catch (...)
			{
				v = 0;
			}

			return new EntityTableItemMongodb_DIGIT<float>(type, v, 0, 0);
		}
		else if (type == "DOUBLE")
		{
			double v = 0;
			try
			{
				StringConv::str2value(v, defaultVal.c_str());
			}
			catch (...)
			{
				v = 0;
			}

			return new EntityTableItemMongodb_DIGIT<double>(type, v, 0, 0);
		}
		else if (type == "STRING")
		{
			return new EntityTableItemMongodb_STRING(defaultVal, 0, 0);
		}
		else if (type == "UNICODE")
		{
			return new EntityTableItemMongodb_UNICODE(defaultVal, 0, 0);
		}
		else if (type == "PYTHON")
		{
			return new EntityTableItemMongodb_PYTHON(defaultVal, 0, 0);
		}
		else if (type == "PY_DICT")
		{
			return new EntityTableItemMongodb_PYTHON(defaultVal, 0, 0);
		}
		else if (type == "PY_TUPLE")
		{
			return new EntityTableItemMongodb_PYTHON(defaultVal, 0, 0);
		}
		else if (type == "PY_LIST")
		{
			return new EntityTableItemMongodb_PYTHON(defaultVal, 0, 0);
		}
		else if (type == "BLOB")
		{
			return new EntityTableItemMongodb_BLOB(defaultVal, 0, 0);
		}
		else if (type == "ARRAY")
		{
			return new EntityTableItemMongodb_ARRAY("", 0, 0);
		}
		else if (type == "FIXED_DICT")
		{
			return new EntityTableItemMongodb_FIXED_DICT("", 0, 0);
		}
		else if (type == "VECTOR2")
		{
			return new EntityTableItemMongodb_VECTOR2(0, 0, 0);
		}
		else if (type == "VECTOR3")
		{
			return new EntityTableItemMongodb_VECTOR3(0, 0, 0);
		}
		else if (type == "VECTOR4")
		{
			return new EntityTableItemMongodb_VECTOR4(0, 0, 0);
		}
		else if (type == "ENTITYCALL")
		{
			return new EntityTableItemMongodb_ENTITYCALL("", 0, 0);
		}
		else if (type == "ENTITY_COMPONENT")
		{
			return new EntityTableItemMongdb_Component("", 0, 0);
		}

		KBE_ASSERT(false && "not found type.\n");
		return new EntityTableItemMongodb_STRING("", 0, 0);
	}

	DBID EntityTableMongodb::writeTable(DBInterface* pdbi, DBID dbid, int8 shouldAutoLoad, MemoryStream* s, ScriptDefModule* pModule)
	{
		mongodb::DBContext context;
		context.parentTableName = "";
		context.parentTableDBID = 0;
		context.dbid = dbid;
		context.tableName = pModule->getName();
		context.isEmpty = false;
		context.readresultIdx = 0;

		bson_t doc;
		bson_init(&doc);

		while (s->length() > 0)
		{
			ENTITY_PROPERTY_UID pid;
			ENTITY_PROPERTY_UID child_pid;
			(*s) >> pid >> child_pid;

			EntityTableItem* pTableItem = this->findItem(child_pid);
			if (pTableItem == NULL)
			{
				ERROR_MSG(fmt::format("EntityTable::writeTable: not found item[{}].\n", child_pid));
				return dbid;
			}

			static_cast<EntityTableItemMongodbBase*>(pTableItem)->getWriteSqlItem(pdbi, s, context, &doc);
		}

		bool writeOK = false;

		// 已有 DBID 表示更新整个实体文档的持久化字段。
		// An existing DBID updates the persisted fields of the entity document.
		if (context.dbid > 0)
		{
			bson_t query;
			bson_init(&query);
			BSON_APPEND_INT64(&query, "id", context.dbid);

			DBInterfaceMongodb* pdbiMongodb = static_cast<DBInterfaceMongodb*>(pdbi);
			char name[MAX_BUF];
			kbe_snprintf(name, MAX_BUF, ENTITY_TABLE_PERFIX "_%s", context.tableName.c_str());

			BSON_APPEND_INT64(&doc, "id", context.dbid);


			// 使用 $set 保留文档中的引擎元数据和未来兼容字段，只覆盖本次实体属性。
			// Wrap fields in $set to preserve engine metadata and forward-compatible fields while replacing current entity properties.
			bson_t update;
			bson_init(&update);
			BSON_APPEND_DOCUMENT(&update, "$set", &doc);


			writeOK = pdbiMongodb->updateCollection(name, MONGOC_UPDATE_NONE, &query, &update, NULL);

			bson_destroy(&update);
			bson_destroy(&query);

		}
		// 新实体在插入前生成全局 DBID，并与自动加载标志一起写入文档。
		// A new entity receives its global DBID before insertion together with the auto-load flag.
		else
		{
			DBInterfaceMongodb* pdbiMongodb = static_cast<DBInterfaceMongodb*>(pdbi);
			char name[MAX_BUF];
			kbe_snprintf(name, MAX_BUF, ENTITY_TABLE_PERFIX "_%s", context.tableName.c_str());

			context.dbid = genUUID64();
			BSON_APPEND_INT64(&doc, TABLE_ID_CONST_STR, context.dbid);
			BSON_APPEND_INT32(&doc, TABLE_ITEM_PERFIX"_" TABLE_AUTOLOAD_CONST_STR, 0);
			writeOK = pdbiMongodb->insertCollection(name, MONGOC_INSERT_NONE, &doc, NULL);
		}

		bson_destroy(&doc);

		if (!writeOK)
			return 0;

		dbid = context.dbid;

		// 仅在调用方明确提供标志时更新自动加载状态，负值表示保持原状态。
		// Update auto-load state only when explicitly supplied; a negative value preserves the stored state.
		if (shouldAutoLoad > -1)
			entityShouldAutoLoad(pdbi, dbid, shouldAutoLoad > 0);

		return dbid;
	}

	void EntityTableMongodb::entityShouldAutoLoad(DBInterface* pdbi, DBID dbid, bool shouldAutoLoad)
	{
		if (dbid == 0)
			return;

		DBInterfaceMongodb* pdbiMongodb = static_cast<DBInterfaceMongodb*>(pdbi);

		char name[MAX_BUF];
		kbe_snprintf(name, MAX_BUF, ENTITY_TABLE_PERFIX "_%s", tableName());

		bson_t query;
		bson_init(&query);
		BSON_APPEND_INT64(&query, TABLE_ID_CONST_STR, dbid);

		bson_t values;
		bson_init(&values);
		BSON_APPEND_INT32(&values, TABLE_ITEM_PERFIX"_" TABLE_AUTOLOAD_CONST_STR, shouldAutoLoad ? 1 : 0);

		bson_t update;
		bson_init(&update);
		BSON_APPEND_DOCUMENT(&update, "$set", &values);

		pdbiMongodb->updateCollection(name, MONGOC_UPDATE_NONE, &query, &update, NULL);

		bson_destroy(&update);
		bson_destroy(&values);
		bson_destroy(&query);
	}

	bool EntityTableMongodb::removeEntity(DBInterface* pdbi, DBID dbid, ScriptDefModule* pModule)
	{
		KBE_ASSERT(pModule && dbid > 0);

		DBInterfaceMongodb* pdbiMongodb = static_cast<DBInterfaceMongodb*>(pdbi);

		char name[MAX_BUF];
		kbe_snprintf(name, MAX_BUF, ENTITY_TABLE_PERFIX "_%s", pModule->getName());

		bson_t query;
		bson_init(&query);
		BSON_APPEND_INT64(&query, TABLE_ID_CONST_STR, dbid);

		bool ret = pdbiMongodb->collectionRemove(name, MONGOC_REMOVE_SINGLE_REMOVE, &query, NULL);
		bson_destroy(&query);
		return ret;
	}

	/**
	获取所有的数据放到流中
	Decode a complete entity document into the KBEngine entity stream.
	*/
	bool EntityTableMongodb::queryTable(DBInterface* pdbi, DBID dbid, MemoryStream* s, ScriptDefModule* pModule)
	{
		KBE_ASSERT(pModule && s && dbid > 0);

		mongodb::DBContext context;
		context.parentTableName = "";
		context.parentTableDBID = 0;
		context.dbid = dbid;
		context.tableName = pModule->getName();
		context.isEmpty = false;
		context.readresultIdx = 0;

		std::vector<EntityTableItem*>::iterator iter = tableFixedOrderItems_.begin();
		for (; iter != tableFixedOrderItems_.end(); ++iter)
		{
			static_cast<EntityTableItemMongodbBase*>((*iter))->getReadSqlItem(context);
		}

		// 先取得完整实体文档，再按固定属性顺序写入数据流，保证脚本解码稳定。
		// Fetch the complete entity document first, then write properties in fixed order for stable script decoding.
		bson_t query;
		bson_init(&query);
		BSON_APPEND_INT64(&query, "id", dbid);

		DBInterfaceMongodb* pdbiMongodb = static_cast<DBInterfaceMongodb*>(pdbi);
		char name[MAX_BUF];
		kbe_snprintf(name, MAX_BUF, ENTITY_TABLE_PERFIX "_%s", context.tableName.c_str());

		std::unique_ptr<MongoCursorGuard> guard = pdbiMongodb->collectionFind(name, MONGOC_QUERY_NONE, 0, 0, 0, &query, NULL, NULL);
		bson_destroy(&query);

		const bson_t* doc = NULL;
		bson_error_t  error;
		while (mongoc_cursor_more(guard->cursor()) && mongoc_cursor_next(guard->cursor(), &doc)) {
			break;
		}

		if (mongoc_cursor_error(guard->cursor(), &error)) {
			pdbiMongodb->setLastError(error);
			ERROR_MSG(fmt::format("An error occurred: {}\n", error.message));
			pdbiMongodb->throwError();
		}

		if (doc == NULL)
		{
			return false;
		}

		iter = tableFixedOrderItems_.begin();
		for (; iter != tableFixedOrderItems_.end(); ++iter)
		{
			static_cast<EntityTableItemMongodbBase*>((*iter))->addToStream(s, context, dbid, doc);
		}

		return true;
	}


	//-------------------------------------------------------------------------------------
	void EntityTableItemMongodbBase::init_db_item_name(const char* exstrFlag)
	{
		kbe_snprintf(db_item_name_, MAX_BUF, TABLE_ITEM_PERFIX"_%s%s", exstrFlag, itemName());
	}

	bool EntityTableItemMongodbBase::initialize(const PropertyDescription* pPropertyDescription,
		const DataType* pDataType, std::string name)
	{
		itemName(name);

		pDataType_ = pDataType;
		pPropertyDescription_ = pPropertyDescription;
		indexType_ = pPropertyDescription->indexType();
		return true;
	}

	//-------------------------------------------------------------------------------------

	//-------------------------------------------------------------------------------------
	void EntityTableMongodb::getWriteSqlItem(DBInterface* pdbi, MemoryStream* s, mongodb::DBContext& context, bson_t* doc)
	{
		if (tableFixedOrderItems_.size() == 0)
			return;

		std::vector<EntityTableItem*>::iterator iter = tableFixedOrderItems_.begin();

		mongodb::DBContext* context1 = new mongodb::DBContext();
		context1->parentTableName = (*iter)->pParentTable()->tableName();
		context1->tableName = (*iter)->tableName();
		context1->parentTableDBID = 0;
		context1->dbid = 0;
		context1->isEmpty = (s == NULL);
		context1->readresultIdx = 0;

		KBEShared_ptr< mongodb::DBContext > opTableValBox1Ptr(context1);
		context.optable.push_back(std::pair<std::string, KBEShared_ptr< mongodb::DBContext > >
			((*iter)->tableName(), opTableValBox1Ptr));

		for (; iter != tableFixedOrderItems_.end(); ++iter)
		{
			static_cast<EntityTableItemMongodbBase*>((*iter))->getWriteSqlItem(pdbi, s, *context1, doc);
		}
	}

	void EntityTableMongodb::getReadSqlItem(mongodb::DBContext& context)
	{
		if (tableFixedOrderItems_.size() == 0)
			return;

		std::vector<EntityTableItem*>::iterator iter = tableFixedOrderItems_.begin();

		mongodb::DBContext* context1 = new mongodb::DBContext();
		context1->parentTableName = (*iter)->pParentTable()->tableName();
		context1->tableName = (*iter)->tableName();
		context1->parentTableDBID = 0;
		context1->dbid = 0;
		context1->isEmpty = true;
		context1->readresultIdx = 0;

		KBEShared_ptr< mongodb::DBContext > opTableValBox1Ptr(context1);
		context.optable.push_back(std::pair<std::string, KBEShared_ptr< mongodb::DBContext > >
			((*iter)->tableName(), opTableValBox1Ptr));

		for (; iter != tableFixedOrderItems_.end(); ++iter)
		{
			static_cast<EntityTableItemMongodbBase*>((*iter))->getReadSqlItem(*context1);
		}
	}

	void EntityTableMongodb::addToStream(MemoryStream* s, mongodb::DBContext& context, DBID resultDBID, const bson_t* doc)
	{
		std::vector<EntityTableItem*>::iterator iter = tableFixedOrderItems_.begin();
		for (; iter != tableFixedOrderItems_.end(); ++iter)
		{
			static_cast<EntityTableItemMongodbBase*>((*iter))->addToStream(s, context, resultDBID, doc);
		}
	}

	//-------------------------------------------------------------------------------------
	bool EntityTableItemMongodb_VECTOR2::isSameKey(std::string key)
	{
		for (int i = 0; i < 2; ++i)
		{
			if (key == db_item_names_[i])
				return true;
		}

		return false;
	}

	void EntityTableItemMongodb_VECTOR2::getWriteSqlItem(DBInterface* pdbi, MemoryStream* s, mongodb::DBContext& context, bson_t* doc)
	{
		if (s == NULL)
			return;

#ifdef CLIENT_NO_FLOAT
		int32 v;
#else
		float v;
#endif

		for (ArraySize i = 0; i < 2; ++i)
		{
			(*s) >> v;
			mongodb::DBContext::DB_ITEM_DATA* pSotvs = new mongodb::DBContext::DB_ITEM_DATA();
			pSotvs->sqlkey = db_item_names_[i];

#ifdef CLIENT_NO_FLOAT
			kbe_snprintf(pSotvs->sqlval, MAX_BUF, "%d", v);
#else
			kbe_snprintf(pSotvs->sqlval, MAX_BUF, "%f", v);
#endif

			context.items.push_back(KBEShared_ptr<mongodb::DBContext::DB_ITEM_DATA>(pSotvs));

			BSON_APPEND_DOUBLE(doc, pSotvs->sqlkey, v);
		}
	}

	void EntityTableItemMongodb_VECTOR2::getReadSqlItem(mongodb::DBContext& context)
	{
		for (ArraySize i = 0; i < 2; ++i)
		{
			mongodb::DBContext::DB_ITEM_DATA* pSotvs = new mongodb::DBContext::DB_ITEM_DATA();
			pSotvs->sqlkey = db_item_names_[i];
			memset(pSotvs->sqlval, 0, MAX_BUF);
			context.items.push_back(KBEShared_ptr<mongodb::DBContext::DB_ITEM_DATA>(pSotvs));
		}
	}

	void EntityTableItemMongodb_VECTOR2::addToStream(MemoryStream* s, mongodb::DBContext& context, DBID resultDBID, const bson_t* doc)
	{
		for (ArraySize i = 0; i < 2; ++i)
		{
			bson_iter_t iter;
			if (!bson_iter_init_find(&iter, doc, db_item_names_[i]))
			{
				// 缺失向量字段按实体定义默认值回填，以兼容新增持久化属性的旧文档。
				// Fill missing vector fields from entity defaults to support old documents after persistent properties are added.
#ifdef CLIENT_NO_FLOAT
				(*s) << (int32)0;
#else
				(*s) << (float)0;
#endif
				continue;
			}
#ifdef CLIENT_NO_FLOAT
			int32 v = bson_iter_int32(&iter);
#else
			double vv = bson_iter_double(&iter);
			float v = static_cast<float>(vv);
#endif
			(*s) << v;
		}
	}

	//-------------------------------------------------------------------------------------
	bool EntityTableItemMongodb_VECTOR3::isSameKey(std::string key)
	{
		for (int i = 0; i < 3; ++i)
		{
			if (key == db_item_names_[i])
				return true;
		}

		return false;
	}

	void EntityTableItemMongodb_VECTOR3::getWriteSqlItem(DBInterface* pdbi, MemoryStream* s, mongodb::DBContext& context, bson_t* doc)
	{
		if (s == NULL)
			return;

#ifdef CLIENT_NO_FLOAT
		int32 v;
#else
		float v;
#endif

		for (ArraySize i = 0; i < 3; ++i)
		{
			(*s) >> v;
			mongodb::DBContext::DB_ITEM_DATA* pSotvs = new mongodb::DBContext::DB_ITEM_DATA();
			pSotvs->sqlkey = db_item_names_[i];

#ifdef CLIENT_NO_FLOAT
			kbe_snprintf(pSotvs->sqlval, MAX_BUF, "%d", v);
#else
			kbe_snprintf(pSotvs->sqlval, MAX_BUF, "%f", v);
#endif
			context.items.push_back(KBEShared_ptr<mongodb::DBContext::DB_ITEM_DATA>(pSotvs));

			BSON_APPEND_DOUBLE(doc, pSotvs->sqlkey, v);
		}
	}

	void EntityTableItemMongodb_VECTOR3::getReadSqlItem(mongodb::DBContext& context)
	{
		for (ArraySize i = 0; i < 3; ++i)
		{
			mongodb::DBContext::DB_ITEM_DATA* pSotvs = new mongodb::DBContext::DB_ITEM_DATA();
			pSotvs->sqlkey = db_item_names_[i];
			memset(pSotvs->sqlval, 0, MAX_BUF);
			context.items.push_back(KBEShared_ptr<mongodb::DBContext::DB_ITEM_DATA>(pSotvs));
		}
	}

	void EntityTableItemMongodb_VECTOR3::addToStream(MemoryStream* s, mongodb::DBContext& context, DBID resultDBID, const bson_t* doc)
	{
		for (ArraySize i = 0; i < 3; ++i)
		{
			bson_iter_t iter;
			if (!bson_iter_init_find(&iter, doc, db_item_names_[i]))
			{
				// 缺失向量字段按实体定义默认值回填，以兼容新增持久化属性的旧文档。
				// Fill missing vector fields from entity defaults to support old documents after persistent properties are added.
#ifdef CLIENT_NO_FLOAT
				(*s) << (int32)0;
#else
				(*s) << (float)0;
#endif
				continue;
			}

#ifdef CLIENT_NO_FLOAT
			int32 v = bson_iter_int32(&iter);
#else
			double vv = bson_iter_double(&iter);
			float v = static_cast<float>(vv);
#endif
			(*s) << v;
		}
	}

	//-------------------------------------------------------------------------------------
	bool EntityTableItemMongodb_VECTOR4::isSameKey(std::string key)
	{
		for (int i = 0; i < 4; ++i)
		{
			if (key == db_item_names_[i])
				return true;
		}

		return false;
	}

	void EntityTableItemMongodb_VECTOR4::getWriteSqlItem(DBInterface* pdbi, MemoryStream* s, mongodb::DBContext& context, bson_t* doc)
	{
		if (s == NULL)
			return;

#ifdef CLIENT_NO_FLOAT
		int32 v;
#else
		float v;
#endif

		for (ArraySize i = 0; i < 4; ++i)
		{
			(*s) >> v;
			mongodb::DBContext::DB_ITEM_DATA* pSotvs = new mongodb::DBContext::DB_ITEM_DATA();
			pSotvs->sqlkey = db_item_names_[i];

#ifdef CLIENT_NO_FLOAT
			kbe_snprintf(pSotvs->sqlval, MAX_BUF, "%d", v);
#else
			kbe_snprintf(pSotvs->sqlval, MAX_BUF, "%f", v);
#endif

			context.items.push_back(KBEShared_ptr<mongodb::DBContext::DB_ITEM_DATA>(pSotvs));

			BSON_APPEND_DOUBLE(doc, pSotvs->sqlkey, v);
		}
	}

	void EntityTableItemMongodb_VECTOR4::getReadSqlItem(mongodb::DBContext& context)
	{
		for (ArraySize i = 0; i < 4; ++i)
		{
			mongodb::DBContext::DB_ITEM_DATA* pSotvs = new mongodb::DBContext::DB_ITEM_DATA();
			pSotvs->sqlkey = db_item_names_[i];
			memset(pSotvs->sqlval, 0, MAX_BUF);
			context.items.push_back(KBEShared_ptr<mongodb::DBContext::DB_ITEM_DATA>(pSotvs));
		}
	}

	void EntityTableItemMongodb_VECTOR4::addToStream(MemoryStream* s, mongodb::DBContext& context, DBID resultDBID, const bson_t* doc)
	{
		for (ArraySize i = 0; i < 4; ++i)
		{
			bson_iter_t iter;
			if (!bson_iter_init_find(&iter, doc, db_item_names_[i]))
			{
				// 缺失向量字段按实体定义默认值回填，以兼容新增持久化属性的旧文档。
				// Fill missing vector fields from entity defaults to support old documents after persistent properties are added.
#ifdef CLIENT_NO_FLOAT
				(*s) << (int32)0;
#else
				(*s) << (float)0;
#endif
				continue;
			}

#ifdef CLIENT_NO_FLOAT
			int32 v = bson_iter_int32(&iter);
#else
			double vv = bson_iter_double(&iter);
			float v = static_cast<float>(vv);
#endif
			(*s) << v;
		}
	}

	void EntityTableItemMongodb_ENTITYCALL::getWriteSqlItem(DBInterface* pdbi, MemoryStream* s, mongodb::DBContext& context, bson_t* doc)
	{
	}

	void EntityTableItemMongodb_ENTITYCALL::getReadSqlItem(mongodb::DBContext& context)
	{
	}

	void EntityTableItemMongodb_ENTITYCALL::addToStream(MemoryStream* s, mongodb::DBContext& context, DBID resultDBID, const bson_t* doc)
	{
	}

	//-------------------------------------------------------------------------------------
	bool EntityTableItemMongodb_ARRAY::initialize(const PropertyDescription* pPropertyDescription,
		const DataType* pDataType, std::string name)
	{
		bool ret = EntityTableItemMongodbBase::initialize(pPropertyDescription, pDataType, name);
		if (!ret)
			return false;

		// 数组元素使用子表适配器复用字段编解码，但仍以内嵌文档存储在父实体中。
		// Array elements reuse a child-table adapter for field encoding while remaining embedded in the parent entity document.
		EntityTableMongodb* pTable = new EntityTableMongodb(this->pParentTable()->pEntityTables());

		std::string tname = this->pParentTable()->tableName();
		std::vector<std::string> qname;
		EntityTableItem* pparentItem = this->pParentTableItem();
		while (pparentItem != NULL)
		{
			if (strlen(pparentItem->itemName()) > 0)
				qname.push_back(pparentItem->itemName());
			pparentItem = pparentItem->pParentTableItem();
		}

		if (qname.size() > 0)
		{
			for (int i = (int)qname.size() - 1; i >= 0; i--)
			{
				tname += "_";
				tname += qname[i];
			}
		}

		std::string tableName = tname + "_";
		std::string itemName = "";

		if (name.size() > 0)
		{
			tableName += name;
		}
		else
		{
			tableName += TABLE_ARRAY_ITEM_VALUES_CONST_STR;
		}

		if (itemName.size() == 0)
		{
			if (static_cast<FixedArrayType*>(const_cast<DataType*>(pDataType))->getDataType()->type() != DATA_TYPE_FIXEDDICT)
				itemName = TABLE_ARRAY_ITEM_VALUE_CONST_STR;
		}

		pTable->tableName(tableName);
		pTable->isChild(true);

		EntityTableItem* pArrayTableItem;
		pArrayTableItem = pParentTable_->createItem(static_cast<FixedArrayType*>(const_cast<DataType*>(pDataType))->getDataType()->getName(), pPropertyDescription->getDefaultValStr());
		pArrayTableItem->utype(-pPropertyDescription->getUType());
		pArrayTableItem->pParentTable(this->pParentTable());
		pArrayTableItem->pParentTableItem(this);
		pArrayTableItem->tableName(pTable->tableName());

		ret = pArrayTableItem->initialize(pPropertyDescription,
			static_cast<FixedArrayType*>(const_cast<DataType*>(pDataType))->getDataType(), itemName.c_str());

		if (!ret)
		{
			delete pTable;
			return ret;
		}

		pTable->addItem(pArrayTableItem);
		pChildTable_ = pTable;

		pTable->pEntityTables()->addTable(pTable);
		return true;
	}

	bool EntityTableItemMongodb_ARRAY::isSameKey(std::string key)
	{
		// MongoDB 数组以内嵌字段保存，因此用子表逻辑名匹配键，而不是关联独立 SQL 表。
		// MongoDB arrays are embedded fields, so match the child logical name instead of joining a separate SQL table.
		return pChildTable_->tableName() == key;
	}

	void EntityTableItemMongodb_ARRAY::init_db_item_name(const char* exstrFlag)
	{
		if (pChildTable_)
		{
			static_cast<EntityTableMongodb*>(pChildTable_)->init_db_item_name();
		}
	}

	void EntityTableItemMongodb_ARRAY::getWriteSqlItem(DBInterface* pdbi, MemoryStream* s, mongodb::DBContext& context, bson_t* doc)
	{
		ArraySize size = 0;
		if (s)
			(*s) >> size;

		if (pChildTable_)
		{

			bson_t child;

			bson_append_array_begin(doc, pChildTable_->tableName(), -1, &child);

			if (size > 0)
			{
				for (ArraySize i = 0; i < size; ++i)
				{
					bson_t item;
					bson_init(&item);
					static_cast<EntityTableMongodb*>(pChildTable_)->getWriteSqlItem(pdbi, s, context, &item);
					char str[16];
					kbe_snprintf(str, sizeof(str), "%u", static_cast<uint32>(i));
					bson_append_document(&child, str, -1, &item);
					bson_destroy(&item);
				}
			}
			bson_append_array_end(doc, &child);
		}
	}

	void EntityTableItemMongodb_ARRAY::getReadSqlItem(mongodb::DBContext& context)
	{
		if (pChildTable_)
		{
			static_cast<EntityTableMongodb*>(pChildTable_)->getReadSqlItem(context);
		}
	}

	void EntityTableItemMongodb_ARRAY::addToStream(MemoryStream* s, mongodb::DBContext& context, DBID resultDBID, const bson_t* doc)
	{
		if (pChildTable_)
		{
			bson_iter_t biter;
			bson_iter_t array_iter;
			if (!bson_iter_init_find(&biter, doc, pChildTable_->tableName()) || !BSON_ITER_HOLDS_ARRAY(&biter))
			{
				// 旧文档缺少数组字段时写入空数组，保持数据流结构完整。
				// Emit an empty array when old documents lack the field so the entity stream remains structurally valid.
				(*s) << (ArraySize)0;
				return;
			}

			bson_iter_recurse(&biter, &array_iter);

			mongodb::DBContext::DB_RW_CONTEXTS::iterator iter = context.optable.begin();
			for (; iter != context.optable.end(); ++iter)
			{
				if (pChildTable_->tableName() == iter->first)
				{
					ArraySize size = 0;
					std::list<bson_t*> bsonlist;

					// 先复制所有内嵌文档，再写入流，避免游标迭代期间借用 BSON 内存越界。
					// Copy embedded documents before streaming so borrowed BSON memory never outlives iterator traversal.
					while (true)
					{
						char str[16];
						kbe_snprintf(str, sizeof(str), "%u", static_cast<uint32>(size));
						// BSON 数组键是十进制序号，按连续序号恢复原始元素顺序。
						// BSON array keys are decimal ordinals; scan contiguous ordinals to restore element order.
						if (!bson_iter_find(&array_iter, str))
							break;

						// 为每个内嵌元素创建自有 BSON 副本，随后统一销毁。
						// Create an owned BSON copy for every embedded element and destroy each after decoding.
						const uint8_t* buf;
						uint32_t len;
						bson_iter_document(&array_iter, &len, &buf);

						bson_t* rec = bson_new_from_data(buf, len);
						if (!rec)
						{
							ERROR_MSG("EntityTableItemMongodb_ARRAY::addToStream: invalid embedded BSON document.\n");
							break;
						}
						bsonlist.push_back(rec);

						size++;
					}

					(*s) << size;

					while (!bsonlist.empty())
					{
						static_cast<EntityTableMongodb*>(pChildTable_)->addToStream(s, *iter->second.get(), 0, bsonlist.front());

						bson_destroy(bsonlist.front());
						bsonlist.pop_front();
					}

					return;
				}
			}
		}

		ArraySize size = 0;
		(*s) << size;
	}



	//-------------------------------------------------------------------------------------


	bool EntityTableItemMongdb_Component::isSameKey(std::string key)
	{
		return pChildTable_->tableName() == key;
	}

	bool EntityTableItemMongdb_Component::initialize(const PropertyDescription* pPropertyDescription,
		const DataType* pDataType, std::string name)
	{
		bool ret = EntityTableItemMongodbBase::initialize(
			pPropertyDescription, pDataType, name);


		if (!ret)
			return false;



		EntityComponentType* pEntityComponentType = const_cast<EntityComponentType*>(static_cast<const EntityComponentType*>(pDataType));
		ScriptDefModule* pEntityComponentScriptDefModule = pEntityComponentType->pScriptDefModule();

		EntityTableMongodb* pparentTable = static_cast<EntityTableMongodb*>(this->pParentTable());
		EntityTableMongodb* pTable = new EntityTableMongodb(pparentTable->pEntityTables());

		std::string tableName = std::string(pparentTable->tableName()) + "_" + name;

		pTable->tableName(tableName);
		pTable->isChild(true);

		// 1.x 通过实体表名直接解析宿主模块；组件字段过滤必须和 MySQL/PostgreSQL 后端使用同一宿主定义。
		// The 1.x API resolves the owner directly by table name; component filtering must use the same owner definition as MySQL/PostgreSQL.
		ScriptDefModule* pScriptDefModule = EntityDef::findScriptModule(pparentTable->tableName());

		ScriptDefModule::PROPERTYDESCRIPTION_MAP& pdescrsMap = pEntityComponentScriptDefModule->getPersistentPropertyDescriptions();
		ScriptDefModule::PROPERTYDESCRIPTION_MAP::const_iterator iter = pdescrsMap.begin();

		for (; iter != pdescrsMap.end(); ++iter)
		{
			PropertyDescription* pdescrs = iter->second;

			if (!pScriptDefModule->hasCell() && pdescrs->hasCell() && !pdescrs->hasBase())
			{
				continue;
			}

			EntityTableItem* pETItem = pparentTable->createItem(pdescrs->getDataType()->getName(), pdescrs->getDefaultValStr());

			pETItem->pParentTable(pparentTable);
			pETItem->utype(pdescrs->getUType());
			pETItem->tableName(pTable->tableName());
			pETItem->pParentTableItem(this);

			bool ret = pETItem->initialize(pdescrs, pdescrs->getDataType(), pdescrs->getName());

			if (!ret)
			{
				delete pETItem;
				return false;
			}

			pTable->addItem(pETItem);
		}

		pChildTable_ = pTable;
		pTable->pEntityTables()->addTable(pTable);
		return true;
	}

	bool EntityTableItemMongdb_Component::syncToDB(DBInterface* pdbi, void* pData)
	{
		// 组件以内嵌文档存储，不需要独立集合或 schema 同步。
		// Components are embedded documents and require neither a separate collection nor schema synchronization.
		return true;
	}

	void EntityTableItemMongdb_Component::addToStream(MemoryStream* s, mongodb::DBContext& context, DBID resultDBID, const bson_t* doc)
	{
		if (!pChildTable_)
			return;

		bson_iter_t iter;
		bool foundData = bson_iter_init_find(&iter, doc, pChildTable_->tableName());

		(*s) << foundData;

		if (!foundData)
			return;

		KBE_ASSERT(BSON_ITER_HOLDS_DOCUMENT(&iter));

		const uint8_t* buf;
		uint32_t len;
		bson_iter_document(&iter, &len, &buf);

		bson_t subdoc;
		bson_init_static(&subdoc, buf, len);

		static_cast<EntityTableMongodb*>(pChildTable_)
			->addToStream(s, context, resultDBID, &subdoc);
	}

	void EntityTableItemMongdb_Component::getWriteSqlItem(DBInterface* pdbi, MemoryStream* s,
		mongodb::DBContext& context, bson_t* doc)
	{
		if (!pChildTable_)
			return;

		// 组件写入独立子文档，保持属性命名空间并支持缺失组件的兼容读取。
		// Write a component as a nested document to preserve its property namespace and support compatible missing-component reads.
		bson_t child;
		bson_append_document_begin(
			doc,
			pChildTable_->tableName(),
			-1,
			&child);

		static_cast<EntityTableMongodb*>(pChildTable_)
			->getWriteSqlItem(pdbi, s, context, &child);

		bson_append_document_end(doc, &child);
	}

	void EntityTableItemMongdb_Component::getReadSqlItem(mongodb::DBContext& context)
	{
		if (pChildTable_)
		{
			static_cast<EntityTableMongodb*>(pChildTable_)
				->getReadSqlItem(context);
		}
	}

	void EntityTableItemMongdb_Component::init_db_item_name(const char* exstrFlag)
	{
		EntityTableItemMongodbBase::init_db_item_name(exstrFlag);

		if (pChildTable_)
		{
			static_cast<EntityTableMongodb*>(pChildTable_)
				->init_db_item_name();
		}
	}

	//-------------------------------------------------------------------------------------

	bool EntityTableItemMongodb_FIXED_DICT::isSameKey(std::string key)
	{
		FIXEDDICT_KEYTYPES::iterator fditer = keyTypes_.begin();
		bool tmpfound = false;

		for (; fditer != keyTypes_.end(); ++fditer)
		{
			if (fditer->second->isSameKey(key))
			{
				tmpfound = true;
				break;
			}
		}

		return tmpfound;
	}

	void EntityTableItemMongodb_FIXED_DICT::init_db_item_name(const char* exstrFlag)
	{
		FIXEDDICT_KEYTYPES::iterator fditer = keyTypes_.begin();

		for (; fditer != keyTypes_.end(); ++fditer)
		{
			std::string new_exstrFlag = exstrFlag;
			if (fditer->second->type() == TABLE_ITEM_TYPE_FIXEDDICT)
				new_exstrFlag += fditer->first + "_";

			static_cast<EntityTableItemMongodbBase*>(fditer->second.get())->init_db_item_name(new_exstrFlag.c_str());
		}
	}



	bool EntityTableItemMongodb_FIXED_DICT::initialize(const PropertyDescription* pPropertyDescription,
	                                                   const DataType* pDataType, std::string name)
	{
		bool ret = EntityTableItemMongodbBase::initialize(pPropertyDescription, pDataType, name);
		if (!ret)
			return false;

		KBEngine::FixedDictType* fdatatype = static_cast<KBEngine::FixedDictType*>(const_cast<DataType*>(pDataType));

		FixedDictType::FIXEDDICT_KEYTYPE_MAP& keyTypes = fdatatype->getKeyTypes();
		FixedDictType::FIXEDDICT_KEYTYPE_MAP::iterator iter = keyTypes.begin();

		for (; iter != keyTypes.end(); ++iter)
		{
			if (!iter->second->persistent)
				continue;

			EntityTableItem* tableItem = pParentTable_->createItem(iter->second->dataType->getName(), pPropertyDescription->getDefaultValStr());


			tableItem->pParentTable(this->pParentTable());
			tableItem->pParentTableItem(this);
			tableItem->utype(-pPropertyDescription->getUType());
			tableItem->tableName(this->tableName());
			if (!tableItem->initialize(pPropertyDescription, iter->second->dataType, iter->first))
				return false;

			std::pair< std::string, KBEShared_ptr<EntityTableItem> > itemVal;
			itemVal.first = iter->first;
			itemVal.second.reset(tableItem);

			keyTypes_.push_back(itemVal);
		}

		return true;
	}

	void EntityTableItemMongodb_FIXED_DICT::getWriteSqlItem(DBInterface* pdbi, MemoryStream* s, mongodb::DBContext& context, bson_t* doc)
	{
		FIXEDDICT_KEYTYPES::iterator fditer = keyTypes_.begin();


		for (; fditer != keyTypes_.end(); ++fditer)
		{
			static_cast<EntityTableItemMongodbBase*>(fditer->second.get())->getWriteSqlItem(pdbi, s, context, doc);
		}

	}

	void EntityTableItemMongodb_FIXED_DICT::getReadSqlItem(mongodb::DBContext& context)
	{
		FIXEDDICT_KEYTYPES::iterator fditer = keyTypes_.begin();

		for (; fditer != keyTypes_.end(); ++fditer)
		{
			static_cast<EntityTableItemMongodbBase*>(fditer->second.get())->getReadSqlItem(context);
		}
	}

	void EntityTableItemMongodb_FIXED_DICT::addToStream(MemoryStream* s, mongodb::DBContext& context, DBID resultDBID, const bson_t* doc)
	{
		FIXEDDICT_KEYTYPES::iterator fditer = keyTypes_.begin();

		for (; fditer != keyTypes_.end(); ++fditer)
		{
			static_cast<EntityTableItemMongodbBase*>(fditer->second.get())->addToStream(s, context, resultDBID, doc);
		}
	}

	//-------------------------------------------------------------------------------------
	template<class T>
	void EntityTableItemMongodb_DIGIT<T>::getWriteSqlItem(DBInterface* pdbi, MemoryStream* s, mongodb::DBContext& context, bson_t* doc)
	{
		if (s == NULL)
			return;
		mongodb::DBContext::DB_ITEM_DATA* pSotvs = new mongodb::DBContext::DB_ITEM_DATA();


		if (dataSType_ == "INT8")
		{
			int8 v;
			(*s) >> v;
			kbe_snprintf(pSotvs->sqlval, MAX_BUF, "%d", v);

			BSON_APPEND_INT32(doc, db_item_name(), v);
		}
		else if (dataSType_ == "INT16")
		{
			int16 v;
			(*s) >> v;
			kbe_snprintf(pSotvs->sqlval, MAX_BUF, "%d", v);

			BSON_APPEND_INT32(doc, db_item_name(), v);
		}
		else if (dataSType_ == "INT32")
		{
			int32 v;
			(*s) >> v;
			kbe_snprintf(pSotvs->sqlval, MAX_BUF, "%d", v);

			BSON_APPEND_INT32(doc, db_item_name(), v);
		}
		else if (dataSType_ == "INT64")
		{
			int64 v;
			(*s) >> v;
			kbe_snprintf(pSotvs->sqlval, MAX_BUF, "%" PRI64, v);

			BSON_APPEND_INT64(doc, db_item_name(), v);
		}
		else if (dataSType_ == "UINT8")
		{
			uint8 v;
			(*s) >> v;
			kbe_snprintf(pSotvs->sqlval, MAX_BUF, "%u", v);

			BSON_APPEND_INT32(doc, db_item_name(), v);
		}
		else if (dataSType_ == "UINT16")
		{
			uint16 v;
			(*s) >> v;
			kbe_snprintf(pSotvs->sqlval, MAX_BUF, "%u", v);

			BSON_APPEND_INT32(doc, db_item_name(), v);
		}
		else if (dataSType_ == "UINT32")
		{
			uint32 v;
			(*s) >> v;
			kbe_snprintf(pSotvs->sqlval, MAX_BUF, "%u", v);

			BSON_APPEND_INT32(doc, db_item_name(), v);
		}
		else if (dataSType_ == "UINT64")
		{
			uint64 v;
			(*s) >> v;
			kbe_snprintf(pSotvs->sqlval, MAX_BUF, "%" PRIu64, v);

			BSON_APPEND_INT64(doc, db_item_name(), v);
		}
		else if (dataSType_ == "FLOAT")
		{
			float v;
			(*s) >> v;
			kbe_snprintf(pSotvs->sqlval, MAX_BUF, "%f", v);

			BSON_APPEND_DOUBLE(doc, db_item_name(), v);
		}
		else if (dataSType_ == "DOUBLE")
		{
			double v;
			(*s) >> v;
			kbe_snprintf(pSotvs->sqlval, MAX_BUF, "%lf", v);

			BSON_APPEND_DOUBLE(doc, db_item_name(), v);
		}

		pSotvs->sqlkey = db_item_name();
		context.items.push_back(KBEShared_ptr<mongodb::DBContext::DB_ITEM_DATA>(pSotvs));
	}

	template<class T>
	void EntityTableItemMongodb_DIGIT<T>::getReadSqlItem(mongodb::DBContext& context)
	{
		mongodb::DBContext::DB_ITEM_DATA* pSotvs = new mongodb::DBContext::DB_ITEM_DATA();
		pSotvs->sqlkey = db_item_name();
		memset(pSotvs->sqlval, 0, MAX_BUF);
		context.items.push_back(KBEShared_ptr<mongodb::DBContext::DB_ITEM_DATA>(pSotvs));
	}

	template<class T>
	void EntityTableItemMongodb_DIGIT<T>::addToStream(MemoryStream* s, mongodb::DBContext& context, DBID resultDBID, const bson_t* doc)
	{
		bson_iter_t iter;
		bool isdefault = false;
		if (!bson_iter_init_find(&iter, doc, db_item_name()))
		{
			isdefault = true;
		}

		if (dataSType_ == "INT8")
		{
			if (isdefault || !BSON_ITER_HOLDS_INT32(&iter))
			{
				(*s) << (int8)defaultValue_;
				return;
			}

			int32 v = bson_iter_int32(&iter);
			int8 vv = static_cast<int8>(v);
			(*s) << vv;

		}
		else if (dataSType_ == "INT16")
		{
			if (isdefault || !BSON_ITER_HOLDS_INT32(&iter))
			{
				(*s) << (int16)defaultValue_;
				return;
			}

			int32 v = bson_iter_int32(&iter);
			int16 vv = static_cast<int16>(v);
			(*s) << vv;
		}
		else if (dataSType_ == "INT32")
		{
			if (isdefault || !BSON_ITER_HOLDS_INT32(&iter))
			{
				(*s) << (int32)defaultValue_;
				return;
			}

			int32 v = bson_iter_int32(&iter);
			(*s) << v;
		}
		else if (dataSType_ == "INT64")
		{
			if (isdefault || !BSON_ITER_HOLDS_INT64(&iter))
			{
				(*s) << (int64)defaultValue_;
				return;
			}

			int64 v = bson_iter_int64(&iter);
			(*s) << v;
		}
		else if (dataSType_ == "UINT8")
		{
			if (isdefault || !BSON_ITER_HOLDS_INT32(&iter))
			{
				(*s) << (uint8)defaultValue_;
				return;
			}

			int32 v = bson_iter_int32(&iter);
			uint8 vv = static_cast<uint8>(v);
			(*s) << vv;
		}
		else if (dataSType_ == "UINT16")
		{
			if (isdefault || !BSON_ITER_HOLDS_INT32(&iter))
			{
				(*s) << (uint16)defaultValue_;
				return;
			}

			int32 v = bson_iter_int32(&iter);
			uint16 vv = static_cast<uint16>(v);
			(*s) << vv;
		}
		else if (dataSType_ == "UINT32")
		{
			if (isdefault || !BSON_ITER_HOLDS_INT32(&iter))
			{
				(*s) << (uint32)defaultValue_;
				return;
			}

			uint32 v = bson_iter_int32(&iter);
			(*s) << v;
		}
		else if (dataSType_ == "UINT64")
		{
			if (isdefault || !BSON_ITER_HOLDS_INT64(&iter))
			{
				(*s) << (uint64)defaultValue_;
				return;
			}

			uint64 v = bson_iter_int64(&iter);
			(*s) << v;
		}
		else if (dataSType_ == "FLOAT")
		{
			if (isdefault || !BSON_ITER_HOLDS_DOUBLE(&iter))
			{
				(*s) << (float)defaultValue_;
				return;
			}

			double v = bson_iter_double(&iter);
			float vv = static_cast<float>(v);
			(*s) << vv;
		}
		else if (dataSType_ == "DOUBLE")
		{
			if (isdefault || !BSON_ITER_HOLDS_DOUBLE(&iter))
			{
				(*s) << (double)defaultValue_;
				return;
			}

			double v = bson_iter_double(&iter);
			(*s) << v;
		}
	}


	//-------------------------------------------------------------------------------------
	void EntityTableItemMongodb_STRING::getWriteSqlItem(DBInterface* pdbi,
		MemoryStream* s, mongodb::DBContext& context, bson_t* doc)
	{
		if (s == NULL)
			return;

		mongodb::DBContext::DB_ITEM_DATA* pSotvs = new mongodb::DBContext::DB_ITEM_DATA();

		std::string val;
		(*s) >> val;

		// BSON 编码负责字符串边界和转义，不再分配 SQL 转义缓冲区。
		// BSON encoding owns string boundaries and escaping, so no SQL escape buffer is allocated.
		pSotvs->extraDatas = val;
		memset(pSotvs->sqlval, 0, sizeof(pSotvs->sqlval));
		pSotvs->sqlkey = db_item_name();
		context.items.push_back(KBEShared_ptr<mongodb::DBContext::DB_ITEM_DATA>(pSotvs));

		bson_append_utf8(doc, pSotvs->sqlkey, (int)strlen(pSotvs->sqlkey), val.c_str(), static_cast<int>(val.length()));
	}

	void EntityTableItemMongodb_STRING::getReadSqlItem(mongodb::DBContext& context)
	{
		mongodb::DBContext::DB_ITEM_DATA* pSotvs = new mongodb::DBContext::DB_ITEM_DATA();
		pSotvs->sqlkey = db_item_name();
		memset(pSotvs->sqlval, 0, MAX_BUF);
		context.items.push_back(KBEShared_ptr<mongodb::DBContext::DB_ITEM_DATA>(pSotvs));
	}

	void EntityTableItemMongodb_STRING::addToStream(MemoryStream* s,
		mongodb::DBContext& context, DBID resultDBID, const bson_t* doc)
	{
		bson_iter_t iter;
		if (!bson_iter_init_find(&iter, doc, db_item_name()) || !BSON_ITER_HOLDS_UTF8(&iter))
		{
			// 旧文档缺少字符串字段时使用实体定义默认值。
			// Use the entity-definition default when an old document lacks this string field.
			(*s) << defaultValue_;
			return;
		}

		uint32_t len = 0;
		const char* value = bson_iter_utf8(&iter, &len);
		std::string datas(value, len);
		(*s) << datas;
	}

	//-------------------------------------------------------------------------------------
	void EntityTableItemMongodb_UNICODE::getWriteSqlItem(DBInterface* pdbi, MemoryStream* s,
		mongodb::DBContext& context, bson_t* doc)
	{
		if (s == NULL)
			return;

		mongodb::DBContext::DB_ITEM_DATA* pSotvs = new mongodb::DBContext::DB_ITEM_DATA();

		std::string val;
		s->readBlob(val);

		pSotvs->extraDatas = val;
		memset(pSotvs->sqlval, 0, sizeof(pSotvs->sqlval));
		pSotvs->sqlkey = db_item_name();
		context.items.push_back(KBEShared_ptr<mongodb::DBContext::DB_ITEM_DATA>(pSotvs));

		bson_append_utf8(doc, pSotvs->sqlkey, (int)strlen(pSotvs->sqlkey), val.c_str(), static_cast<int>(val.length()));
	}

	void EntityTableItemMongodb_UNICODE::getReadSqlItem(mongodb::DBContext& context)
	{
		mongodb::DBContext::DB_ITEM_DATA* pSotvs = new mongodb::DBContext::DB_ITEM_DATA();
		pSotvs->sqlkey = db_item_name();
		memset(pSotvs->sqlval, 0, MAX_BUF);
		context.items.push_back(KBEShared_ptr<mongodb::DBContext::DB_ITEM_DATA>(pSotvs));
	}

	void EntityTableItemMongodb_UNICODE::addToStream(MemoryStream* s,
		mongodb::DBContext& context, DBID resultDBID, const bson_t* doc)
	{
		bson_iter_t iter;
		if (!bson_iter_init_find(&iter, doc, db_item_name()) || !BSON_ITER_HOLDS_UTF8(&iter))
		{
			// 旧文档缺少字符串字段时按当前实体定义回填默认值。
			// Fill the current entity-definition default when an old document lacks this string field.
			(*s).appendBlob(defaultValue_.data(), static_cast<ArraySize>(defaultValue_.size()));
			return;
		}

		uint32_t len = 0;
		const char* value = bson_iter_utf8(&iter, &len);
		std::string datas(value, len);
		(*s).appendBlob(datas.data(), static_cast<ArraySize>(datas.size()));
	}

	//-------------------------------------------------------------------------------------
	void EntityTableItemMongodb_BLOB::getWriteSqlItem(DBInterface* pdbi, MemoryStream* s, mongodb::DBContext& context, bson_t* doc)
	{
		if (s == NULL)
			return;

		mongodb::DBContext::DB_ITEM_DATA* pSotvs = new mongodb::DBContext::DB_ITEM_DATA();

		std::string val;
		s->readBlob(val);

		pSotvs->extraDatas = val;
		memset(pSotvs->sqlval, 0, sizeof(pSotvs->sqlval));
		pSotvs->sqlkey = db_item_name();
		context.items.push_back(KBEShared_ptr<mongodb::DBContext::DB_ITEM_DATA>(pSotvs));

		BSON_APPEND_BINARY(doc, pSotvs->sqlkey, BSON_SUBTYPE_BINARY,
			reinterpret_cast<const uint8_t*>(val.data()), static_cast<uint32_t>(val.size()));
	}

	void EntityTableItemMongodb_BLOB::getReadSqlItem(mongodb::DBContext& context)
	{
		mongodb::DBContext::DB_ITEM_DATA* pSotvs = new mongodb::DBContext::DB_ITEM_DATA();
		pSotvs->sqlkey = db_item_name();
		memset(pSotvs->sqlval, 0, MAX_BUF);
		context.items.push_back(KBEShared_ptr<mongodb::DBContext::DB_ITEM_DATA>(pSotvs));
	}

	void EntityTableItemMongodb_BLOB::addToStream(MemoryStream* s, mongodb::DBContext& context, DBID resultDBID, const bson_t* doc)
	{
		bson_iter_t iter;
		if (!bson_iter_init_find(&iter, doc, db_item_name()))
		{
			// 旧文档缺少二进制字段时按当前实体定义回填默认值。
			// Fill the current entity-definition default when an old document lacks this binary field.
			(*s).appendBlob(defaultValue_.data(), static_cast<ArraySize>(defaultValue_.size()));
			return;
		}

		if (BSON_ITER_HOLDS_BINARY(&iter))
		{
			bson_subtype_t subtype;
			uint32_t len = 0;
			const uint8_t* value = NULL;
			bson_iter_binary(&iter, &subtype, &len, &value);
			(*s).appendBlob(reinterpret_cast<const char*>(value), static_cast<ArraySize>(len));
			return;
		}

		// 兼容迁移前被错误编码为 BSON UTF-8 的 BLOB 文档，下一次实体写回时自动转为 Binary。
		// Accept legacy BLOB values incorrectly encoded as BSON UTF-8; the next entity write converts them to Binary.
		if (BSON_ITER_HOLDS_UTF8(&iter))
		{
			uint32_t len = 0;
			const char* value = bson_iter_utf8(&iter, &len);
			(*s).appendBlob(value, static_cast<ArraySize>(len));
			return;
		}

		(*s).appendBlob(defaultValue_.data(), static_cast<ArraySize>(defaultValue_.size()));

	}

	//-------------------------------------------------------------------------------------
	void EntityTableItemMongodb_PYTHON::getWriteSqlItem(DBInterface* pdbi, MemoryStream* s, mongodb::DBContext& context, bson_t* doc)
	{
		if (s == NULL)
			return;

		mongodb::DBContext::DB_ITEM_DATA* pSotvs = new mongodb::DBContext::DB_ITEM_DATA();

		std::string val;
		s->readBlob(val);

		pSotvs->extraDatas = val;
		memset(pSotvs->sqlval, 0, sizeof(pSotvs->sqlval));
		pSotvs->sqlkey = db_item_name();
		context.items.push_back(KBEShared_ptr<mongodb::DBContext::DB_ITEM_DATA>(pSotvs));

		BSON_APPEND_BINARY(doc, pSotvs->sqlkey, BSON_SUBTYPE_BINARY, reinterpret_cast<const uint8_t*>(val.c_str()), static_cast<uint32_t>(val.length()));
	}

	void EntityTableItemMongodb_PYTHON::getReadSqlItem(mongodb::DBContext& context)
	{
		mongodb::DBContext::DB_ITEM_DATA* pSotvs = new mongodb::DBContext::DB_ITEM_DATA();
		pSotvs->sqlkey = db_item_name();
		memset(pSotvs->sqlval, 0, MAX_BUF);
		context.items.push_back(KBEShared_ptr<mongodb::DBContext::DB_ITEM_DATA>(pSotvs));
	}

	void EntityTableItemMongodb_PYTHON::addToStream(MemoryStream* s, mongodb::DBContext& context, DBID resultDBID, const bson_t* doc)
	{
		bson_iter_t iter;
		if (!bson_iter_init_find(&iter, doc, db_item_name()) || !BSON_ITER_HOLDS_BINARY(&iter))
		{
			// 旧文档缺少二进制字段时按当前实体定义回填默认值。
			// Fill the current entity-definition default when an old document lacks this binary field.
			(*s).appendBlob(defaultValue_.data(), static_cast<ArraySize>(defaultValue_.size()));
			return;
		}

		bson_subtype_t btype;
		uint32_t len = 0;
		const char* value;
		bson_iter_binary(&iter, &btype, &len, (const uint8_t**)&value);
		std::string datas(value, len);
		(*s).appendBlob(datas);
	}
}
