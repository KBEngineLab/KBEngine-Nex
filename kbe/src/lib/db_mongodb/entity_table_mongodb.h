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

#pragma once
#include "db_interface_mongodb.h"
#include "common.h"
#include "common/common.h"
#include "common/singleton.h"
#include "helper/debug_helper.h"
#include "db_interface/entity_table.h"

namespace KBEngine {

	class ScriptDefModule;
	class EntityTableMongodb;

	/*
	维护entity在数据库表中的一个字段
	Represent one persisted entity property in a MongoDB document.
	*/
	class EntityTableItemMongodbBase : public EntityTableItem
	{
	public:
		EntityTableItemMongodbBase(uint32 datalength, uint32 flags) :
			EntityTableItem("", datalength, flags)
		{
			memset(db_item_name_, 0, MAX_BUF);
		};

		virtual ~EntityTableItemMongodbBase()
		{
		};

		uint8 type() const { return TABLE_ITEM_TYPE_UNKONWN; }

		/**
		初始化字段适配器及其持久化名称。
		Initialize the field adapter and its persisted name.
		*/
		virtual bool initialize(const PropertyDescription* pPropertyDescription,
			const DataType* pDataType, std::string name);

		/**
		同步实体字段或集合元数据到 MongoDB。
		Synchronize entity-field or collection metadata with MongoDB.
		*/
		virtual bool syncToDB(DBInterface* pdbi, void* pData = NULL) = 0;

		/**
		更新数据
		Update one persisted entity property.
		*/
		virtual bool writeItem(DBInterface* pdbi, DBID dbid, MemoryStream* s, ScriptDefModule* pModule) { return true; }

		/**
		查询表
		Query one entity document and decode this property.
		*/
		virtual bool queryTable(DBInterface* pdbi, DBID dbid, MemoryStream* s, ScriptDefModule* pModule) { return true; }

		/**
		从 BSON 文档解码当前字段并写入实体数据流。
		Decode this field from a BSON document into the entity data stream.
		*/
		virtual void addToStream(MemoryStream* s, mongodb::DBContext& context, DBID resultDBID, const bson_t* doc) {};

		/**
		在写入时追加 BSON 字段，在读取时登记字段上下文。
		Append the BSON field on writes and register its field context on reads.
		*/
		virtual void getWriteSqlItem(DBInterface* pdbi, MemoryStream* s, mongodb::DBContext& context, bson_t* doc) = 0;
		virtual void getReadSqlItem(mongodb::DBContext& context) = 0;

		virtual void init_db_item_name(const char* exstrFlag = "");
		const char* db_item_name() { return db_item_name_; }

		virtual bool isSameKey(std::string key) { return key == db_item_name(); }

	protected:
		char db_item_name_[MAX_BUF];

	};

	template<class T>
	class EntityTableItemMongodb_DIGIT : public EntityTableItemMongodbBase
	{
	public:
		EntityTableItemMongodb_DIGIT(std::string dataSType, T defaultValue,
			uint32 datalength, uint32 flags) :
			EntityTableItemMongodbBase(datalength, flags),
			dataSType_(dataSType),
			defaultValue_(defaultValue)
		{
		};

		virtual ~EntityTableItemMongodb_DIGIT() {};

		uint8 type() const { return TABLE_ITEM_TYPE_DIGIT; }

		/**
		同步实体字段或集合元数据到 MongoDB。
		Synchronize entity-field or collection metadata with MongoDB.
		*/
		virtual bool syncToDB(DBInterface* pdbi, void* pData = NULL) { return true; }

		/**
		从 BSON 文档解码当前字段并写入实体数据流。
		Decode this field from a BSON document into the entity data stream.
		*/
		void addToStream(MemoryStream* s, mongodb::DBContext& context, DBID resultDBID, const bson_t* doc);

		/**
		在写入时追加 BSON 字段，在读取时登记字段上下文。
		Append the BSON field on writes and register its field context on reads.
		*/
		virtual void getWriteSqlItem(DBInterface* pdbi, MemoryStream* s, mongodb::DBContext& context, bson_t* doc);
		virtual void getReadSqlItem(mongodb::DBContext& context);
	protected:
		std::string dataSType_;
		T defaultValue_;
	};

	class EntityTableItemMongodb_STRING : public EntityTableItemMongodbBase
	{
	public:
		EntityTableItemMongodb_STRING(std::string defaultValue,
			uint32 datalength, uint32 flags) :
			EntityTableItemMongodbBase(datalength, flags),
			defaultValue_(defaultValue)
		{
		}

		virtual ~EntityTableItemMongodb_STRING() {};

		uint8 type() const { return TABLE_ITEM_TYPE_STRING; }

		/**
		同步实体字段或集合元数据到 MongoDB。
		Synchronize entity-field or collection metadata with MongoDB.
		*/
		virtual bool syncToDB(DBInterface* pdbi, void* pData = NULL) { return true; }

		/**
		从 BSON 文档解码当前字段并写入实体数据流。
		Decode this field from a BSON document into the entity data stream.
		*/
		void addToStream(MemoryStream* s, mongodb::DBContext& context, DBID resultDBID, const bson_t* doc);

		/**
		在写入时追加 BSON 字段，在读取时登记字段上下文。
		Append the BSON field on writes and register its field context on reads.
		*/
		virtual void getWriteSqlItem(DBInterface* pdbi, MemoryStream* s, mongodb::DBContext& context, bson_t* doc);
		virtual void getReadSqlItem(mongodb::DBContext& context);
	protected:
		std::string defaultValue_;
	};

	class EntityTableItemMongodb_UNICODE : public EntityTableItemMongodbBase
	{
	public:
		EntityTableItemMongodb_UNICODE(std::string defaultValue,
			uint32 datalength, uint32 flags) :
			EntityTableItemMongodbBase(datalength, flags),
			defaultValue_(defaultValue)
		{
		}

		virtual ~EntityTableItemMongodb_UNICODE() {};

		uint8 type() const { return TABLE_ITEM_TYPE_UNICODE; }

		/**
		同步实体字段或集合元数据到 MongoDB。
		Synchronize entity-field or collection metadata with MongoDB.
		*/
		virtual bool syncToDB(DBInterface* pdbi, void* pData = NULL) { return true; }

		/**
		从 BSON 文档解码当前字段并写入实体数据流。
		Decode this field from a BSON document into the entity data stream.
		*/
		void addToStream(MemoryStream* s, mongodb::DBContext& context, DBID resultDBID, const bson_t* doc);

		/**
		在写入时追加 BSON 字段，在读取时登记字段上下文。
		Append the BSON field on writes and register its field context on reads.
		*/
		virtual void getWriteSqlItem(DBInterface* pdbi, MemoryStream* s, mongodb::DBContext& context, bson_t* doc);
		virtual void getReadSqlItem(mongodb::DBContext& context);
	protected:
		std::string defaultValue_;
	};

	class EntityTableItemMongodb_PYTHON : public EntityTableItemMongodbBase
	{
	public:
		EntityTableItemMongodb_PYTHON(std::string defaultValue,
			uint32 datalength, uint32 flags) :
			EntityTableItemMongodbBase(datalength, flags),
			defaultValue_(defaultValue)
		{
		}

		virtual ~EntityTableItemMongodb_PYTHON() {};

		uint8 type() const { return TABLE_ITEM_TYPE_PYTHON; }

		/**
		同步实体字段或集合元数据到 MongoDB。
		Synchronize entity-field or collection metadata with MongoDB.
		*/
		virtual bool syncToDB(DBInterface* pdbi, void* pData = NULL) { return true; }

		/**
		从 BSON 文档解码当前字段并写入实体数据流。
		Decode this field from a BSON document into the entity data stream.
		*/
		void addToStream(MemoryStream* s, mongodb::DBContext& context, DBID resultDBID, const bson_t* doc);

		/**
		在写入时追加 BSON 字段，在读取时登记字段上下文。
		Append the BSON field on writes and register its field context on reads.
		*/
		virtual void getWriteSqlItem(DBInterface* pdbi, MemoryStream* s, mongodb::DBContext& context, bson_t* doc);
		virtual void getReadSqlItem(mongodb::DBContext& context);
	protected:
		std::string defaultValue_;
	};

	class EntityTableItemMongodb_BLOB : public EntityTableItemMongodbBase
	{
	public:
		EntityTableItemMongodb_BLOB(std::string defaultValue,
			uint32 datalength, uint32 flags) :
			EntityTableItemMongodbBase(datalength, flags),
			defaultValue_(defaultValue)
		{
		}

		virtual ~EntityTableItemMongodb_BLOB() {};

		uint8 type() const { return TABLE_ITEM_TYPE_BLOB; }

		/**
		同步实体字段或集合元数据到 MongoDB。
		Synchronize entity-field or collection metadata with MongoDB.
		*/
		virtual bool syncToDB(DBInterface* pdbi, void* pData = NULL) { return true; }

		/**
		从 BSON 文档解码当前字段并写入实体数据流。
		Decode this field from a BSON document into the entity data stream.
		*/
		void addToStream(MemoryStream* s, mongodb::DBContext& context, DBID resultDBID, const bson_t* doc);

		/**
		在写入时追加 BSON 字段，在读取时登记字段上下文。
		Append the BSON field on writes and register its field context on reads.
		*/
		virtual void getWriteSqlItem(DBInterface* pdbi, MemoryStream* s, mongodb::DBContext& context, bson_t* doc);
		virtual void getReadSqlItem(mongodb::DBContext& context);
	protected:
		std::string defaultValue_;
	};

	class EntityTableItemMongodb_VECTOR2 : public EntityTableItemMongodbBase
	{
	public:
		EntityTableItemMongodb_VECTOR2(float defaultValue,
			uint32 datalength, uint32 flags) :
			EntityTableItemMongodbBase(datalength, flags),
			defaultValue_(defaultValue)
		{
		}

		virtual ~EntityTableItemMongodb_VECTOR2() {};

		uint8 type() const { return TABLE_ITEM_TYPE_VECTOR2; }

		virtual bool isSameKey(std::string key);

		/**
		同步实体字段或集合元数据到 MongoDB。
		Synchronize entity-field or collection metadata with MongoDB.
		*/
		virtual bool syncToDB(DBInterface* pdbi, void* pData = NULL) { return true; }

		/**
		从 BSON 文档解码当前字段并写入实体数据流。
		Decode this field from a BSON document into the entity data stream.
		*/
		void addToStream(MemoryStream* s, mongodb::DBContext& context, DBID resultDBID, const bson_t* doc);

		/**
		在写入时追加 BSON 字段，在读取时登记字段上下文。
		Append the BSON field on writes and register its field context on reads.
		*/
		virtual void getWriteSqlItem(DBInterface* pdbi, MemoryStream* s, mongodb::DBContext& context, bson_t* doc);
		virtual void getReadSqlItem(mongodb::DBContext& context);

		virtual void init_db_item_name(const char* exstrFlag = "")
		{
			for (int i = 0; i < 2; ++i)
				kbe_snprintf(db_item_names_[i], MAX_BUF, TABLE_ITEM_PERFIX"_%d_%s%s", i, exstrFlag, itemName());
		}

	protected:
		char db_item_names_[2][MAX_BUF];
		float defaultValue_;
	};

	class EntityTableItemMongodb_VECTOR3 : public EntityTableItemMongodbBase
	{
	public:
		EntityTableItemMongodb_VECTOR3(float defaultValue,
			uint32 datalength, uint32 flags) :
			EntityTableItemMongodbBase(datalength, flags),
			defaultValue_(defaultValue)
		{
		}

		virtual ~EntityTableItemMongodb_VECTOR3() {};

		uint8 type() const { return TABLE_ITEM_TYPE_VECTOR3; }

		virtual bool isSameKey(std::string key);

		/**
		同步实体字段或集合元数据到 MongoDB。
		Synchronize entity-field or collection metadata with MongoDB.
		*/
		virtual bool syncToDB(DBInterface* pdbi, void* pData = NULL) { return true; }

		/**
		从 BSON 文档解码当前字段并写入实体数据流。
		Decode this field from a BSON document into the entity data stream.
		*/
		void addToStream(MemoryStream* s, mongodb::DBContext& context, DBID resultDBID, const bson_t* doc);

		/**
		在写入时追加 BSON 字段，在读取时登记字段上下文。
		Append the BSON field on writes and register its field context on reads.
		*/
		virtual void getWriteSqlItem(DBInterface* pdbi, MemoryStream* s, mongodb::DBContext& context, bson_t* doc);
		virtual void getReadSqlItem(mongodb::DBContext& context);

		virtual void init_db_item_name(const char* exstrFlag = "")
		{
			for (int i = 0; i < 3; ++i)
				kbe_snprintf(db_item_names_[i], MAX_BUF, TABLE_ITEM_PERFIX"_%d_%s%s", i, exstrFlag, itemName());
		}

	protected:
		char db_item_names_[3][MAX_BUF];
		float defaultValue_;
	};

	class EntityTableItemMongodb_VECTOR4 : public EntityTableItemMongodbBase
	{
	public:
		EntityTableItemMongodb_VECTOR4(float defaultValue,
			uint32 datalength, uint32 flags) :
			EntityTableItemMongodbBase(datalength, flags),
			defaultValue_(defaultValue)
		{
		}

		virtual ~EntityTableItemMongodb_VECTOR4() {};

		uint8 type() const { return TABLE_ITEM_TYPE_VECTOR4; }

		virtual bool isSameKey(std::string key);

		/**
		同步实体字段或集合元数据到 MongoDB。
		Synchronize entity-field or collection metadata with MongoDB.
		*/
		virtual bool syncToDB(DBInterface* pdbi, void* pData = NULL) { return true; }

		/**
		从 BSON 文档解码当前字段并写入实体数据流。
		Decode this field from a BSON document into the entity data stream.
		*/
		void addToStream(MemoryStream* s, mongodb::DBContext& context, DBID resultDBID, const bson_t* doc);

		/**
		在写入时追加 BSON 字段，在读取时登记字段上下文。
		Append the BSON field on writes and register its field context on reads.
		*/
		virtual void getWriteSqlItem(DBInterface* pdbi, MemoryStream* s, mongodb::DBContext& context, bson_t* doc);
		virtual void getReadSqlItem(mongodb::DBContext& context);

		virtual void init_db_item_name(const char* exstrFlag = "")
		{
			for (int i = 0; i < 4; ++i)
				kbe_snprintf(db_item_names_[i], MAX_BUF, TABLE_ITEM_PERFIX"_%d_%s%s", i, exstrFlag, itemName());
		}

	protected:
		char db_item_names_[4][MAX_BUF];
		float defaultValue_;
	};

	class EntityTableItemMongodb_ENTITYCALL : public EntityTableItemMongodbBase
	{
	public:
		EntityTableItemMongodb_ENTITYCALL(std::string defaultValue,
			uint32 datalength, uint32 flags) :
			EntityTableItemMongodbBase(datalength, flags)
		{
		}

		virtual ~EntityTableItemMongodb_ENTITYCALL() {};

		uint8 type() const { return TABLE_ITEM_TYPE_ENTITYCALL; }

		/**
		同步实体字段或集合元数据到 MongoDB。
		Synchronize entity-field or collection metadata with MongoDB.
		*/
		virtual bool syncToDB(DBInterface* pdbi, void* pData = NULL) { return true; }

		/**
		从 BSON 文档解码当前字段并写入实体数据流。
		Decode this field from a BSON document into the entity data stream.
		*/
		void addToStream(MemoryStream* s, mongodb::DBContext& context, DBID resultDBID, const bson_t* doc);

		/**
		在写入时追加 BSON 字段，在读取时登记字段上下文。
		Append the BSON field on writes and register its field context on reads.
		*/
		virtual void getWriteSqlItem(DBInterface* pdbi, MemoryStream* s, mongodb::DBContext& context, bson_t* doc);
		virtual void getReadSqlItem(mongodb::DBContext& context);
	protected:
		std::string defaultValue_;
	};

	class EntityTableItemMongodb_ARRAY : public EntityTableItemMongodbBase
	{
	public:
		EntityTableItemMongodb_ARRAY(std::string defaultValue,
			uint32 datalength, uint32 flags) :
			EntityTableItemMongodbBase(datalength, flags),
			pChildTable_(NULL)
		{
		}

		virtual ~EntityTableItemMongodb_ARRAY() {};

		virtual bool isSameKey(std::string key);

		/**
		初始化字段适配器及其持久化名称。
		Initialize the field adapter and its persisted name.
		*/
		virtual bool initialize(const PropertyDescription* pPropertyDescription,
			const DataType* pDataType, std::string name);

		uint8 type() const { return TABLE_ITEM_TYPE_FIXEDARRAY; }

		/**
		同步实体字段或集合元数据到 MongoDB。
		Synchronize entity-field or collection metadata with MongoDB.
		*/
		virtual bool syncToDB(DBInterface* pdbi, void* pData = NULL) { return true; }

		/**
		从 BSON 文档解码当前字段并写入实体数据流。
		Decode this field from a BSON document into the entity data stream.
		*/
		void addToStream(MemoryStream* s, mongodb::DBContext& context, DBID resultDBID, const bson_t* doc);

		/**
		在写入时追加 BSON 字段，在读取时登记字段上下文。
		Append the BSON field on writes and register its field context on reads.
		*/
		virtual void getWriteSqlItem(DBInterface* pdbi, MemoryStream* s, mongodb::DBContext& context, bson_t* doc);
		virtual void getReadSqlItem(mongodb::DBContext& context);

		virtual void init_db_item_name(const char* exstrFlag = "");

	protected:
		EntityTable* pChildTable_;
	};

	class EntityTableItemMongodb_FIXED_DICT : public EntityTableItemMongodbBase
	{
	public:
		EntityTableItemMongodb_FIXED_DICT(std::string defaultValue,
			uint32 datalength, uint32 flags) :
			EntityTableItemMongodbBase(datalength, flags)
		{
		}

		virtual ~EntityTableItemMongodb_FIXED_DICT() {};

		typedef std::vector< std::pair< std::string, KBEShared_ptr<EntityTableItem> > > FIXEDDICT_KEYTYPES;

		uint8 type() const { return TABLE_ITEM_TYPE_FIXEDDICT; }

		virtual bool isSameKey(std::string key);

		/**
		初始化字段适配器及其持久化名称。
		Initialize the field adapter and its persisted name.
		*/
		virtual bool initialize(const PropertyDescription* pPropertyDescription,
			const DataType* pDataType, std::string name);

		/**
		同步实体字段或集合元数据到 MongoDB。
		Synchronize entity-field or collection metadata with MongoDB.
		*/
		virtual bool syncToDB(DBInterface* pdbi, void* pData = NULL) { return true; }

		/**
		从 BSON 文档解码当前字段并写入实体数据流。
		Decode this field from a BSON document into the entity data stream.
		*/
		void addToStream(MemoryStream* s, mongodb::DBContext& context, DBID resultDBID, const bson_t* doc);

		/**
		在写入时追加 BSON 字段，在读取时登记字段上下文。
		Append the BSON field on writes and register its field context on reads.
		*/
		virtual void getWriteSqlItem(DBInterface* pdbi, MemoryStream* s, mongodb::DBContext& context, bson_t* doc);
		virtual void getReadSqlItem(mongodb::DBContext& context);

		virtual void init_db_item_name(const char* exstrFlag = "");

	protected:
		// 保存固定字典的字段名和对应类型适配器，顺序必须与实体数据流一致。
		// Preserve fixed-dictionary key names and type adapters in entity-stream order.
		EntityTableItemMongodb_FIXED_DICT::FIXEDDICT_KEYTYPES keyTypes_;
	};



	class EntityTableItemMongdb_Component : public EntityTableItemMongodbBase
	{
	public:
		EntityTableItemMongdb_Component(std::string itemDBType,
			uint32 datalength, uint32 flags) :
			EntityTableItemMongodbBase(datalength, flags),
			pChildTable_(NULL)
		{
		}

		virtual ~EntityTableItemMongdb_Component() {};

		virtual bool isSameKey(std::string key);

		/**
			初始化组件适配器及内嵌子表。
			Initialize the component adapter and its embedded child table.
		*/
		virtual bool initialize(const PropertyDescription* pPropertyDescription,
			const DataType* pDataType, std::string name);

		uint8 type() const { return TABLE_ITEM_TYPE_COMPONENT; }

		/**
			同步组件字段元数据到 MongoDB。
			Synchronize component-field metadata with MongoDB.
		*/
		virtual bool syncToDB(DBInterface* pdbi, void* pData = NULL);

		/**
			从 BSON 文档解码组件字段并写入实体数据流。
			Decode the component field from BSON into the entity data stream.
		*/
		void addToStream(MemoryStream* s, mongodb::DBContext& context, DBID resultDBID, const bson_t* doc);

		/**
			在写入时追加组件 BSON，在读取时登记组件上下文。
			Append component BSON on writes and register its component context on reads.
		*/
		virtual void getWriteSqlItem(DBInterface* pdbi, MemoryStream* s, mongodb::DBContext& context, bson_t* doc);
		virtual void getReadSqlItem(mongodb::DBContext& context);

		virtual void init_db_item_name(const char* exstrFlag = "");

	protected:
		EntityTable* pChildTable_;
	};



	/*
	维护entity在数据库中的表
	Manage one entity type as a top-level MongoDB collection.
	*/
	class EntityTableMongodb : public EntityTable
	{
	public:
		EntityTableMongodb(EntityTables* pEntityTables);
		virtual ~EntityTableMongodb();

		/**
		初始化字段适配器及其持久化名称。
		Initialize the field adapter and its persisted name.
		*/
		virtual bool initialize(ScriptDefModule* sm, std::string name);

		/**
		同步实体字段或集合元数据到 MongoDB。
		Synchronize entity-field or collection metadata with MongoDB.
		*/
		virtual bool syncToDB(DBInterface* pdbi);

		/**
		同步表索引
		Reconcile MongoDB indexes with the current entity definition.
		*/
		virtual bool syncIndexToDB(DBInterface* pdbi);

		/**
		创建一个表item
		Create the MongoDB field adapter for one entity data type.
		*/
		virtual EntityTableItem* createItem(std::string type, std::string defaultVal);

		DBID writeTable(DBInterface* pdbi, DBID dbid, int8 shouldAutoLoad, MemoryStream* s, ScriptDefModule* pModule);

		/**
		从数据库删除entity
		Delete one entity document from its collection.
		*/
		bool removeEntity(DBInterface* pdbi, DBID dbid, ScriptDefModule* pModule);

		/**
		获取所有的数据放到流中
		Decode one complete entity document into the entity data stream.
		*/
		virtual bool queryTable(DBInterface* pdbi, DBID dbid, MemoryStream* s, ScriptDefModule* pModule);

		/**
		设置是否自动加载
		Persist whether an entity should be restored automatically at startup.
		*/
		virtual void entityShouldAutoLoad(DBInterface* pdbi, DBID dbid, bool shouldAutoLoad);

		/**
		查询自动加载的实体
		Query DBIDs for entities marked for automatic loading.
		*/
		virtual void queryAutoLoadEntities(DBInterface* pdbi, ScriptDefModule* pModule,
			ENTITY_ID start, ENTITY_ID end, std::vector<DBID>& outs);

		/**
		从 BSON 文档解码当前字段并写入实体数据流。
		Decode this field from a BSON document into the entity data stream.
		*/
		void addToStream(MemoryStream* s, mongodb::DBContext& context, DBID resultDBID, const bson_t* doc);

		/**
		在写入时追加 BSON 字段，在读取时登记字段上下文。
		Append the BSON field on writes and register its field context on reads.
		*/
		virtual void getWriteSqlItem(DBInterface* pdbi, MemoryStream* s, mongodb::DBContext& context, bson_t* doc);
		virtual void getReadSqlItem(mongodb::DBContext& context);

		void init_db_item_name();

	protected:

	};
}

#ifdef CODE_INLINE
#include "entity_table_mongodb.inl"
#endif
