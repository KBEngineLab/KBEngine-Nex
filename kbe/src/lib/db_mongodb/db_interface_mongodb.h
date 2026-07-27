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
#include "common.h"
#include "db_transaction.h"
#include "common/common.h"
#include "common/singleton.h"
#include "common/memorystream.h"
#include "helper/debug_helper.h"
#include "db_interface/db_interface.h"
#include "entitydef/entitydef.h"
#include <mutex>
#include <string>

#include "mongoc/mongoc.h"

class MongoCursorGuard;

namespace KBEngine
{
	class DBInterfaceMongodb : public DBInterface
	{
	public:
		DBInterfaceMongodb(const char* name);
		virtual ~DBInterfaceMongodb();

		static bool initInterface(DBInterface* pdbi);

		/**
		与某个数据库关联
		Attach this interface to a configured MongoDB database.
		*/
		bool reattach();
		virtual bool attach(const char* databaseName = NULL);
		virtual bool detach();

		bool ping(mongoc_client_t* pMongoClient = NULL);

		void inTransaction(bool value)
		{
			KBE_ASSERT(inTransaction_ != value);
			inTransaction_ = value;
		}
		bool inTransaction() const { return inTransaction_; }

		bool hasLostConnection() const { return hasLostConnection_; }
		void hasLostConnection(bool v) { hasLostConnection_ = v; }

		/**
		检查环境
		Validate the MongoDB environment required by KBEngine.
		*/
		virtual bool checkEnvironment();

		/**
		检查并修复可恢复的数据库结构错误，无法安全修复时返回失败。
		Inspect and repair recoverable schema errors, returning false when repair is unsafe.
		*/
		virtual bool checkErrors();

		virtual bool query(const char* strCommand, uint32 size, bool printlog = true, MemoryStream* result = NULL);
		bool executeFindCommand(MemoryStream* result, std::vector<std::string> strcmd, const char* tableName);
		bool executeUpdateCommand(std::vector<std::string> strcmd, const char* tableName);
		bool executeRemoveCommand(std::vector<std::string> strcmd, const char* tableName);
		bool executeInsertCommand(std::vector<std::string> strcmd, const char* tableName);
		bool executeFunctionCommand(MemoryStream* result, std::string strcmd);
		bool extuteFunction(const bson_t* command, const mongoc_read_prefs_t* read_prefs, bson_t* reply);
		std::vector<std::string> splitParameter(std::string value);

		bool write_query_result(MemoryStream* result, const char* strcmd = NULL);

		/**
		获取数据库所有的表名
		Enumerate collection names in the active database.
		*/
		virtual bool getTableNames(std::vector<std::string>& tableNames, const char* pattern);

		/**
		获取数据库某个表所有的字段名称
		Read top-level field names from a representative document in a collection.
		*/
		virtual bool getTableItemNames(const char* tableName, std::vector<std::string>& itemNames);

		/**
		返回这个接口的描述
		Return a diagnostic description of this database interface.
		*/
		virtual const char* c_str();

		/**
		获取错误
		Return the latest owned MongoDB error message.
		*/
		virtual const char* getstrerror();

		/**
		获取错误编号
		Return the latest MongoDB driver or server error code.
		*/
		virtual int getlasterror();

		/**
		如果数据库不存在则创建一个数据库
		Prepare the configured database, which MongoDB creates lazily on first write.
		*/
		virtual bool createDatabaseIfNotExist();

		/**
		创建一个entity存储表
		Create the KBEngine entity-table adapter for MongoDB documents.
		*/
		virtual EntityTable* createEntityTable(EntityTables* pEntityTables);

		/**
		从数据库删除entity表
		Drop an obsolete entity collection from MongoDB.
		*/
		virtual bool dropEntityTableFromDB(const char* tableName);

		/**
		从数据库删除entity表字段
		Remove an obsolete entity property from every document in a collection.
		*/
		virtual bool dropEntityTableItemFromDB(const char* tableName, const char* tableItemName);

		mongoc_client_t* mongo() { return _pMongoClient; }

		/**
		锁住接口操作
		Begin and end the transaction scope surrounding one DB worker task.
		*/
		virtual bool lock();
		virtual DBTransactionResult unlock();
		virtual bool rollback();
		// MongoDB 不允许在多文档事务中执行 listIndexes，结构同步必须在普通会话中完成。
		// MongoDB forbids listIndexes in multi-document transactions, so schema synchronization must run in a regular session.
		virtual bool supportsTransactionalSchemaSynchronization() const { return false; }

		void throwError();
		bool beginTransaction();
		DBTransactionResult commitTransaction();
		bool abortTransaction();

		/**
		处理异常
		Classify database exceptions and reconnect or retry only when safe.
		*/
		bool isLostConnection(std::exception& e);
		bool processException(std::exception& e);

		/**
		执行与数据库相关的操作
		Execute collection-level operations used by entity and system tables.
		*/
		bool createCollection(const char* tableName);

		bool insertCollection(const char* tableName, mongoc_insert_flags_t flags, const bson_t* document, const mongoc_write_concern_t* write_concern);

		std::unique_ptr<MongoCursorGuard> collectionFind(const char* tableName, mongoc_query_flags_t flags, uint32_t skip, uint32_t limit, uint32_t  batch_size, const bson_t* query, const bson_t* fields, const mongoc_read_prefs_t* read_prefs);

		bool updateCollection(const char* tableName, mongoc_update_flags_t uflags, const bson_t* selector, const bson_t* update, const mongoc_write_concern_t* write_concern);

		bool collectionRemove(const char* tableName, mongoc_remove_flags_t flags, const bson_t* selector, const mongoc_write_concern_t* write_concern);

		std::unique_ptr<MongoCursorGuard> collectionFindIndexes(const char* tableName);

		bool collectionCreateIndex(const char* tableName, const bson_t* keys, const bson_t* opt);

		bool collectionDropIndex(const char* tableName, const char* index_name);

		static bool getTopLevelKey(const bson_t* doc, std::string& key);

		void setLastError(const bson_error_t& error);
		void setLastError(const char* error, uint32 domain = 0, uint32 code = 0);
		uint32 lastErrorDomain() const { return lastErrorDomain_; }
		uint32 lastErrorCode() const { return lastErrorCode_; }

	protected:
		bool appendSession(bson_t* options);
		void detectTransactionSupport(const bson_t* helloReply);

		mongoc_client_t* _pMongoClient;
		mongoc_database_t* database;
		mongoc_client_session_t* session_;
		bool hasLostConnection_;
		bool inTransaction_;
		bool transactionSupported_;
		bool transactionWarningLogged_;
		mongodb::DBTransaction lock_;
		std::string lastError_;
		uint32 lastErrorDomain_;
		uint32 lastErrorCode_;

		static std::once_flag s_mongocInitFlag_;
	};
}
