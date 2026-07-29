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

#include "db_interface_mongodb.h"
#include "entity_table_mongodb.h"
#include "kbe_table_mongodb.h"
#include "db_exception.h"
#include "mongo_cursor_guard.h"
#include "thread/threadguard.h"
#include "helper/watcher.h"
#include "server/serverconfig.h"

namespace KBEngine {

	static KBEngine::thread::ThreadMutex _g_logMutex;
	static KBEUnordered_map< std::string, uint32 > g_querystatistics;
	static std::once_flag _g_watcherInitFlag;
	static bool _g_debug = false;
	std::once_flag KBEngine::DBInterfaceMongodb::s_mongocInitFlag_;

	static void querystatistics(const char* strCommand)
	{
		std::string op(strCommand);

		if (op.empty())
			return;

		// 公共转换函数按无符号字节处理输入，避免 UTF-8 高位字节触发 MSVC ctype 断言。
		// The shared converter treats input as unsigned bytes and prevents MSVC ctype assertions on UTF-8 high bytes.
		op = strutil::toUpper(op);

		KBEngine::thread::ThreadGuard tg(&_g_logMutex);
		g_querystatistics[op] += 1;
	}

	static uint32 watcher_query(std::string cmd)
	{
		KBEngine::thread::ThreadGuard tg(&_g_logMutex);

		KBEUnordered_map< std::string, uint32 >::iterator iter = g_querystatistics.find(cmd);
		if (iter != g_querystatistics.end())
		{
			return iter->second;
		}

		return 0;
	}


	static uint32 watcher_select()
	{
		return watcher_query("SELECT");
	}

	static uint32 watcher_delete()
	{
		return watcher_query("DELETE");
	}

	static uint32 watcher_insert()
	{
		return watcher_query("INSERT");
	}

	static uint32 watcher_update()
	{
		return watcher_query("UPDATE");
	}

	static uint32 watcher_json_function()
	{
		return watcher_query("JSON_FUNCTION");
	}



	static uint32 watcher_select_index()
	{
		return watcher_query("SELECT_INDEX");
	}


	static uint32 watcher_create_index()
	{
		return watcher_query("CREATE_INDEX");
	}

	static uint32 watcher_drop_index()
	{
		return watcher_query("DROP_INDEX");
	}


	static void initializeWatcher()
	{
		_g_debug = g_kbeSrvConfig.getDBMgr().debugDBMgr;

		WATCH_OBJECT("db_mongo_querys/select", &KBEngine::watcher_select);
		WATCH_OBJECT("db_mongo_querys/delete", &KBEngine::watcher_delete);
		WATCH_OBJECT("db_mongo_querys/insert", &KBEngine::watcher_insert);
		WATCH_OBJECT("db_mongo_querys/update", &KBEngine::watcher_update);
		WATCH_OBJECT("db_mongo_querys/json_function", &KBEngine::watcher_json_function);
		WATCH_OBJECT("db_mongo_querys/select_index", &KBEngine::watcher_select_index);
		WATCH_OBJECT("db_mongo_querys/create_index", &KBEngine::watcher_create_index);
		WATCH_OBJECT("db_mongo_querys/drop_index", &KBEngine::watcher_drop_index);
	}

	DBInterfaceMongodb::DBInterfaceMongodb(const char* name) :
		DBInterface(name),
		_pMongoClient(NULL),
		database(NULL),
		session_(NULL),
		hasLostConnection_(false),
		inTransaction_(false),
		transactionSupported_(false),
		transactionWarningLogged_(false),
		lock_(this, false),
		lastError_(),
		lastErrorDomain_(0),
		lastErrorCode_(0)
	{
	}

	DBInterfaceMongodb::~DBInterfaceMongodb()
	{
		detach();
	}

	bool DBInterfaceMongodb::initInterface(DBInterface* pdbi)
	{
		EntityTables& entityTables = EntityTables::findByInterfaceName(pdbi->name());

		entityTables.addKBETable(new KBEAccountTableMongodb(&entityTables));
		entityTables.addKBETable(new KBEServerLogTableMongodb(&entityTables));
		entityTables.addKBETable(new KBEEntityLogTableMongodb(&entityTables));
		entityTables.addKBETable(new KBEEmailVerificationTableMongodb(&entityTables));
		return true;
	}

	bool DBInterfaceMongodb::attach(const char* databaseName)
	{
		std::call_once(_g_watcherInitFlag, initializeWatcher);

		// MongoDB 默认监听 27017；配置为零时保持驱动和官方部署的共同默认值。
		// MongoDB listens on 27017 by default; a zero configuration keeps the driver and standard deployment default aligned.
		if (db_port_ == 0)
			db_port_ = 27017;

		if (databaseName != NULL && databaseName[0] != '\0')
			kbe_snprintf(db_name_, MAX_BUF, "%s", databaseName);
		else if (db_name_[0] == '\0')
			kbe_snprintf(db_name_, MAX_BUF, "%s", "kbenginelab");

		hasLostConnection_ = false;
		transactionSupported_ = false;
		transactionWarningLogged_ = false;
		lastError_.clear();
		lastErrorDomain_ = 0;
		lastErrorCode_ = 0;

		// libmongoc 的全局初始化必须在所有客户端之前且全进程仅执行一次。
		// libmongoc global initialization must run once per process before any client is created.
		std::call_once(s_mongocInitFlag_, []()
		{
			DEBUG_MSG("DBInterfaceMongodb::attach: mongoc_init\n");
			mongoc_init();
		});

		if (db_port_ > 65535)
		{
			setLastError("MongoDB port is outside the valid range");
			return false;
		}

		// 通过结构化 URI API 设置凭据，用户名或密码中的保留字符不会再破坏 URI 解析。
		// Configure credentials through the structured URI API so reserved characters in usernames or passwords cannot corrupt URI parsing.
		mongoc_uri_t* uri = mongoc_uri_new_for_host_port(db_ip_, static_cast<uint16_t>(db_port_));
		if (!uri)
		{
			setLastError("Failed to create MongoDB URI");
			return false;
		}

		bool uriOK = mongoc_uri_set_database(uri, db_name_) && mongoc_uri_set_appname(uri, "KBEngine-dbmgr");
		if (db_replicaSet_[0] != '\0')
		{
			// 显式副本集名称把 host:port 变为种子地址，使驱动能够发现其他成员并跟随 PRIMARY 切换。
			// An explicit replica-set name turns host:port into a seed address so the driver can discover members and follow PRIMARY changes.
			uriOK = uriOK && mongoc_uri_set_option_as_utf8(uri, MONGOC_URI_REPLICASET, db_replicaSet_);
		}

		if (db_username_[0] != '\0')
		{
			// 显式认证数据库适配集中管理的 root@admin；空值继续认证业务库，兼容原有部署。
			// An explicit authentication database supports centrally managed root@admin users; an empty value preserves application-database authentication.
			const char* authSource = db_authSource_[0] != '\0' ? db_authSource_ : db_name_;
			uriOK = uriOK && mongoc_uri_set_username(uri, db_username_) &&
				mongoc_uri_set_password(uri, db_password_) && mongoc_uri_set_auth_source(uri, authSource);
		}

		if (!uriOK)
		{
			mongoc_uri_destroy(uri);
			setLastError("Failed to configure MongoDB URI");
			return false;
		}

		bson_error_t error = {};
		_pMongoClient = mongoc_client_new_from_uri_with_error(uri, &error);
		mongoc_uri_destroy(uri);
		if (!_pMongoClient)
		{
			setLastError(error);
			return false;
		}

		mongoc_client_set_error_api(_pMongoClient, MONGOC_ERROR_API_VERSION_2);

		database = mongoc_client_get_database(_pMongoClient, db_name_);
		if (!database)
		{
			setLastError("Failed to create MongoDB database handle");
			detach();
			return false;
		}

		bson_t* command = BCON_NEW("hello", BCON_INT32(1));
		bson_t reply;
		bool retval = mongoc_client_command_simple(_pMongoClient, "admin", command, NULL, &reply, &error);
		bson_destroy(command);

		if (!retval)
		{
			setLastError(error);
			bson_destroy(&reply);
			detach();
			return false;
		}

		detectTransactionSupport(&reply);
		bson_destroy(&reply);

		return true;
	}

	bool DBInterfaceMongodb::ping(mongoc_client_t* pMongoClient)
	{
		mongoc_client_t* client = pMongoClient ? pMongoClient : _pMongoClient;
		if (!client)
		{
			setLastError("MongoDB client is not attached");
			return false;
		}

		bson_t* command = BCON_NEW("ping", BCON_INT32(1));
		bson_t reply;
		bson_error_t error = {};
		bool result = mongoc_client_command_simple(client, "admin", command, NULL, &reply, &error);
		bson_destroy(command);
		bson_destroy(&reply);
		if (!result)
		{
			setLastError(error);
			ERROR_MSG(fmt::format("DBInterfaceMongodb::ping: {}\n", error.message));
		}

		return result;
	}

	bool DBInterfaceMongodb::checkEnvironment()
	{
		// 环境检查执行真实往返，防止已创建客户端但服务器不可达时继续同步集合。
		// Perform a real round trip so collection synchronization cannot continue with an unreachable server.
		return ping();
	}

	bool DBInterfaceMongodb::createDatabaseIfNotExist()
	{
		// MongoDB 在首次创建集合或写入文档时惰性创建数据库，无需发送 SQL 风格的建库命令。
		// MongoDB creates a database lazily on the first collection creation or document write, so no SQL-style command is required.
		return true;
	}

	bool DBInterfaceMongodb::checkErrors()
	{
		return true;
	}

	bool DBInterfaceMongodb::reattach()
	{
		detach();

		bool ret = false;

		try
		{
			ret = attach();
		}
		catch (...)
		{
			return false;
		}

		return ret;
	}

	bool DBInterfaceMongodb::detach()
	{
		if (session_)
		{
			abortTransaction();
		}

		if (database)
		{
			mongoc_database_destroy(database);
			database = NULL;
		}

		if (_pMongoClient)
		{
			mongoc_client_destroy(_pMongoClient);
			_pMongoClient = NULL;
		}

		inTransaction_ = false;
		transactionSupported_ = false;

		return true;
	}

	void DBInterfaceMongodb::detectTransactionSupport(const bson_t* helloReply)
	{
		bson_iter_t iter;
		int32 maxWireVersion = 0;
		bool isReplicaSet = false;
		bool isSharded = false;

		if (bson_iter_init_find(&iter, helloReply, "maxWireVersion") && BSON_ITER_HOLDS_NUMBER(&iter))
			maxWireVersion = static_cast<int32>(bson_iter_as_int64(&iter));

		if (bson_iter_init_find(&iter, helloReply, "setName") && BSON_ITER_HOLDS_UTF8(&iter))
			isReplicaSet = bson_iter_utf8(&iter, NULL)[0] != '\0';

		if (bson_iter_init_find(&iter, helloReply, "msg") && BSON_ITER_HOLDS_UTF8(&iter))
			isSharded = strcmp(bson_iter_utf8(&iter, NULL), "isdbgrid") == 0;

		// 多文档事务要求 MongoDB 4.0+ 副本集或分片集群；独立实例继续依赖单文档原子性。
		// Multi-document transactions require MongoDB 4.0+ on a replica set or sharded cluster; standalone servers retain single-document atomicity.
		transactionSupported_ = maxWireVersion >= 7 && (isReplicaSet || isSharded);
	}

	bool DBInterfaceMongodb::appendSession(bson_t* options)
	{
		if (!session_ || !inTransaction_)
			return true;

		bson_error_t error = {};
		if (mongoc_client_session_append(session_, options, &error))
			return true;

		setLastError(error);
		return false;
	}

	bool DBInterfaceMongodb::beginTransaction()
	{
		if (!transactionSupported_)
		{
			if (!transactionWarningLogged_)
			{
				// 独立 MongoDB 不支持多文档事务，只记录一次降级信息以免每个 DB 任务刷屏。
				// Standalone MongoDB does not support multi-document transactions, so log the atomicity fallback only once per connection.
				WARNING_MSG("DBInterfaceMongodb::beginTransaction: server topology does not support multi-document transactions; using single-document atomicity.\n");
				transactionWarningLogged_ = true;
			}
			return true;
		}

		KBE_ASSERT(session_ == NULL && !inTransaction_);
		bson_error_t error = {};
		session_ = mongoc_client_start_session(_pMongoClient, NULL, &error);
		if (!session_)
		{
			setLastError(error);
			return false;
		}

		if (!mongoc_client_session_start_transaction(session_, NULL, &error))
		{
			setLastError(error);
			mongoc_client_session_destroy(session_);
			session_ = NULL;
			return false;
		}

		inTransaction(true);
		return true;
	}

	DBTransactionResult DBInterfaceMongodb::commitTransaction()
	{
		if (!session_)
			return DB_TRANSACTION_COMMITTED;

		bson_t reply;
		bson_error_t error = {};
		if (!mongoc_client_session_commit_transaction(session_, &reply, &error))
		{
			setLastError(error);
			// 官方驱动通过 error label 标记无法确定的提交；网络错误也必须保守归类为 UNKNOWN。
			// The official driver marks indeterminate commits with an error label; network failures are also conservatively UNKNOWN.
			const bool unknown = mongoc_error_has_label(&reply, "UnknownTransactionCommitResult") ||
				mongodb::DBException(this).isLostConnection();
			bson_destroy(&reply);
			// commitTransaction 一旦返回，驱动不允许再对同一事务调用 abort；这里只丢弃本地会话且绝不重放结果不确定的提交。
			// Once commitTransaction returns, the driver forbids aborting that transaction; discard only the local session and never replay an indeterminate commit.
			mongoc_client_session_destroy(session_);
			session_ = NULL;
			inTransaction(false);
			return unknown ? DB_TRANSACTION_UNKNOWN : DB_TRANSACTION_NOT_COMMITTED;
		}

		bson_destroy(&reply);
		mongoc_client_session_destroy(session_);
		session_ = NULL;
		inTransaction(false);
		return DB_TRANSACTION_COMMITTED;
	}

	bool DBInterfaceMongodb::abortTransaction()
	{
		if (!session_)
			return true;

		bson_error_t error = {};
		bool result = mongoc_client_session_abort_transaction(session_, &error);
		if (!result)
		{
			setLastError(error);
			WARNING_MSG(fmt::format("DBInterfaceMongodb::abortTransaction: {}\n", error.message));
		}

		mongoc_client_session_destroy(session_);
		session_ = NULL;
		if (inTransaction_)
			inTransaction(false);
		return result;
	}

	EntityTable* DBInterfaceMongodb::createEntityTable(EntityTables* pEntityTables)
	{
		return new EntityTableMongodb(pEntityTables);
	}

	bool DBInterfaceMongodb::dropEntityTableFromDB(const char* tableName)
	{
		KBE_ASSERT(tableName != NULL);

		mongoc_collection_t* collection = mongoc_database_get_collection(database, tableName);
		bson_t options;
		bson_init(&options);
		bson_error_t error = {};
		bool result = appendSession(&options) && mongoc_collection_drop_with_opts(collection, &options, &error);
		if (!result && error.message[0] != '\0')
		{
			setLastError(error);
			ERROR_MSG(fmt::format("DBInterfaceMongodb::dropEntityTableFromDB({}): {}\n", tableName, error.message));
		}

		bson_destroy(&options);
		mongoc_collection_destroy(collection);
		return result;
	}

	bool DBInterfaceMongodb::dropEntityTableItemFromDB(const char* tableName, const char* tableItemName)
	{
		KBE_ASSERT(tableName != NULL && tableItemName != NULL);

		// MongoDB 没有集合级字段定义，用一次多文档 $unset 删除旧属性以完成实体定义收敛。
		// MongoDB has no collection-level field schema, so a multi-document $unset converges stored entities after a property is removed.
		bson_t selector;
		bson_t unsetFields;
		bson_t update;
		bson_init(&selector);
		bson_init(&unsetFields);
		bson_init(&update);
		BSON_APPEND_UTF8(&unsetFields, tableItemName, "");
		BSON_APPEND_DOCUMENT(&update, "$unset", &unsetFields);

		bool result = updateCollection(tableName, MONGOC_UPDATE_MULTI_UPDATE, &selector, &update, NULL);
		bson_destroy(&update);
		bson_destroy(&unsetFields);
		bson_destroy(&selector);
		return result;
	}

	bool DBInterfaceMongodb::query(const char* cmd, size_t size, bool printlog, MemoryStream* result)
	{
		if (!cmd || size == 0)
		{
			setLastError("MongoDB query command is empty");
			return false;
		}

		lastquery_.assign(cmd, size);
		if (_pMongoClient == NULL)
		{
			if (printlog)
			{
				ERROR_MSG(fmt::format("DBInterfaceMongodb::query: client is not attached, command=({})\n", lastquery_));
			}

			if (result)
			{
				uint32 nfields = 0;
				uint64 affectedRows = 0;
				uint64 lastInsertID = 0;
				(*result) << nfields << affectedRows << lastInsertID;
			}

			return false;
		}

		if (_g_debug)
		{
			DEBUG_MSG(fmt::format("DBInterfaceMongodb::query({:p}): {}\n", (void*)this, lastquery_));
		}

		// 无回调的原始命令仍必须执行，临时流只用于消费 DBInterface 统一结果格式。
		// Raw commands without a callback must still execute; a scratch stream only consumes the common DBInterface result format.
		MemoryStream scratch;
		return write_query_result(result ? result : &scratch, lastquery_.c_str());
	}

	bool DBInterfaceMongodb::write_query_result(MemoryStream* result, const char* cmd)
	{
		if (result == NULL || cmd == NULL)
		{
			setLastError("MongoDB query result or command is null");
			return false;
		}

		std::string strCommand(cmd);
		bool returnsRows = false;
		bool resultFlag = false;

		if (!strCommand.empty() && strCommand[0] == '{')
		{
			returnsRows = true;
			resultFlag = executeFunctionCommand(result, strCommand);
		}
		else
		{
			std::size_t index = strCommand.find('.');
			std::size_t open = strCommand.find('(', index == std::string::npos ? 0 : index + 1);
			if (index == std::string::npos || open == std::string::npos || strCommand.back() != ')')
			{
				setLastError("Invalid MongoDB command syntax");
			}
			else
			{
				std::string tableName = strCommand.substr(0, index);
				std::string operation = strCommand.substr(index + 1, open - index - 1);
				std::string arguments = strCommand.substr(open + 1, strCommand.size() - open - 2);
				std::vector<std::string> commandArguments = splitParameter(arguments);

				if (operation == "find")
				{
					returnsRows = true;
					resultFlag = executeFindCommand(result, commandArguments, tableName.c_str());
				}
				else if (operation == "update")
				{
					resultFlag = executeUpdateCommand(commandArguments, tableName.c_str());
				}
				else if (operation == "remove")
				{
					resultFlag = executeRemoveCommand(commandArguments, tableName.c_str());
				}
				else if (operation == "insert")
				{
					resultFlag = executeInsertCommand(commandArguments, tableName.c_str());
				}
				else
				{
					setLastError("Unsupported MongoDB collection operation");
				}
			}
		}

		// 写操作和失败的读操作使用 DBInterface 约定的空结果头，保持脚本回调解码一致。
		// Writes and failed reads use the empty DBInterface result header so script callbacks decode a consistent payload.
		if (!returnsRows || !resultFlag)
		{
			uint32 nfields = 0;
			uint64 affectedRows = 0;
			uint64 lastInsertID = 0;

			(*result) << nfields;
			(*result) << affectedRows;
			(*result) << lastInsertID;
		}

		return resultFlag;
	}

	bool DBInterfaceMongodb::getTableNames(std::vector<std::string>& tableNames, const char* pattern)
	{
		if (_pMongoClient == NULL)
		{
			setLastError("MongoDB client is not attached");
			ERROR_MSG("DBInterfaceMongodb::getTableNames: client is not attached.\n");
			return false;
		}

		tableNames.clear();
		bson_t options;
		bson_init(&options);
		bson_error_t error = {};
		if (!appendSession(&options))
		{
			bson_destroy(&options);
			return false;
		}

		char** names = mongoc_database_get_collection_names_with_opts(database, &options, &error);
		bson_destroy(&options);
		if (!names)
		{
			setLastError(error);
			ERROR_MSG(fmt::format("DBInterfaceMongodb::getTableNames: {}\n", error.message));
			return false;
		}

		// 当前同步调用传入空模式；非空模式按名称子串过滤，保持接口可用于诊断工具。
		// Synchronization currently passes an empty pattern; non-empty patterns use name substring filtering for diagnostic callers.
		const std::string filter = pattern ? pattern : "";
		for (char** name = names; *name != NULL; ++name)
		{
			if (filter.empty() || std::string(*name).find(filter) != std::string::npos)
				tableNames.push_back(*name);
		}

		bson_strfreev(names);

		return true;
	}

	bool DBInterfaceMongodb::getTableItemNames(const char* tableName, std::vector<std::string>& itemNames)
	{
		KBE_ASSERT(tableName != NULL);
		itemNames.clear();

		bson_t query;
		bson_init(&query);
		std::unique_ptr<MongoCursorGuard> guard = collectionFind(tableName, MONGOC_QUERY_NONE, 0, 1, 0,
			&query, NULL, NULL);
		bson_destroy(&query);
		if (!guard || !guard->cursor())
			return false;

		const bson_t* document = NULL;
		if (!mongoc_cursor_next(guard->cursor(), &document))
		{
			bson_error_t error = {};
			if (mongoc_cursor_error(guard->cursor(), &error))
			{
				setLastError(error);
				return false;
			}
			return true;
		}

		// 文档模型没有固定列定义，返回首个文档的顶层键供通用数据库检查逻辑使用。
		// The document model has no fixed columns, so expose top-level keys from the first document to generic database inspection code.
		bson_iter_t iter;
		if (bson_iter_init(&iter, document))
		{
			while (bson_iter_next(&iter))
				itemNames.push_back(bson_iter_key(&iter));
		}

		return true;
	}

	const char* DBInterfaceMongodb::c_str()
	{
		static char strdescr[MAX_BUF];
		kbe_snprintf(strdescr, MAX_BUF, "interface=%s, dbtype=mongodb, ip=%s, port=%u, currdatabase=%s, username=%s, connected=%s.\n",
			name_, db_ip_, db_port_, db_name_, db_username_, _pMongoClient == NULL ? "no" : "yes");

		return strdescr;
	}

	const char* DBInterfaceMongodb::getstrerror()
	{
		if (!lastError_.empty())
			return lastError_.c_str();

		return _pMongoClient == NULL ? "MongoDB client is not attached" : "";
	}

	int DBInterfaceMongodb::getlasterror()
	{
		return static_cast<int>(lastErrorCode_);
	}

	void DBInterfaceMongodb::setLastError(const bson_error_t& error)
	{
		setLastError(error.message, error.domain, error.code);
	}

	void DBInterfaceMongodb::setLastError(const char* error, uint32 domain, uint32 code)
	{
		// 错误文本由接口拥有，避免保存驱动栈上 bson_error_t.message 的悬空指针。
		// The interface owns the error text so no pointer can outlive a stack-local bson_error_t.message buffer.
		lastError_ = error ? error : "Unknown MongoDB error";
		lastErrorDomain_ = domain;
		lastErrorCode_ = code;
	}

	bool DBInterfaceMongodb::lock()
	{
		return lock_.start();
	}

	//-------------------------------------------------------------------------------------
	DBTransactionResult DBInterfaceMongodb::unlock()
	{
		if (!lock_.active())
			return DB_TRANSACTION_NOT_COMMITTED;

		DBTransactionResult result = lock_.commit();
		lock_.end();
		return result;
	}

	//-------------------------------------------------------------------------------------
	bool DBInterfaceMongodb::rollback()
	{
		lock_.end();
		return true;
	}

	void DBInterfaceMongodb::throwError()
	{
		mongodb::DBException e(this);

		if (e.isLostConnection())
		{
			this->hasLostConnection(true);
		}

		throw e;
	}


	bool DBInterfaceMongodb::isLostConnection(std::exception& e)
	{
		mongodb::DBException* dbe = dynamic_cast<mongodb::DBException*>(&e);
		return dbe && dbe->isLostConnection();
	}

	bool DBInterfaceMongodb::processException(std::exception& e)
	{
		mongodb::DBException* dbe = dynamic_cast<mongodb::DBException*>(&e);
		if (!dbe)
		{
			ERROR_MSG(fmt::format("DBInterfaceMongodb::processException: unexpected exception type: {}\n", e.what()));
			return false;
		}

		bool retry = false;

		if (dbe->isLostConnection())
		{
			ERROR_MSG(fmt::format("DBInterfaceMongodb::processException: "
				"Thread {:p} lost connection to database. Exception: {}. "
				"Attempting to reconnect.\n",
				(void*)this,
				dbe->what()));

			// 单次重连失败时把控制权交还线程池，避免数据库不可用期间永久占住一个工作线程。
			// Return control to the thread pool after one failed reconnect so an unavailable database cannot trap a worker indefinitely.
			retry = this->reattach();
			if (retry)
			{
				INFO_MSG(fmt::format("DBInterfaceMongodb::processException: Thread {:p} reconnected({}).\n",
					(void*)this, db_name_));
			}
			else
			{
				ERROR_MSG(fmt::format("DBInterfaceMongodb::processException: Thread {:p} reconnect({}) failed({}).\n",
					(void*)this, db_name_, getstrerror()));
			}
		}
		else if (dbe->shouldRetry())
		{
			WARNING_MSG(fmt::format("DBInterfaceMongodb::processException: Retrying {:p}\nException:{}\nnlastquery={}\n",
				(void*)this, dbe->what(), lastquery_));

			retry = true;
		}
		else
		{
			WARNING_MSG(fmt::format("DBInterfaceMongodb::processException: "
				"Exception: {}\nlastquery={}\n",
				dbe->what(), lastquery_));
		}

		return retry;
	}

	bool DBInterfaceMongodb::createCollection(const char* tableName)
	{
		querystatistics("CREATE");

		bson_t options;
		bson_error_t error = {};

		// 集合创建是幂等操作，已存在也返回成功，使调用方可在每次启动时安全校验索引。
		// Collection creation is idempotent; an existing collection is also success so callers can verify indexes on every startup.
		if (mongoc_database_has_collection(database, tableName, &error))
			return true;

		bson_init(&options);
		if (!appendSession(&options))
		{
			bson_destroy(&options);
			return false;
		}

		mongoc_collection_t* collection = mongoc_database_create_collection(database, tableName, &options, &error);
		bson_destroy(&options);

		if (!collection)
		{
			setLastError(error);
			ERROR_MSG(fmt::format("DBInterfaceMongodb::createCollection({}): {}\n", tableName, error.message));
			return false;
		}

		mongoc_collection_destroy(collection);
		return true;
	}

	bool DBInterfaceMongodb::insertCollection(const char* tableName, mongoc_insert_flags_t flags, const bson_t* document, const mongoc_write_concern_t* write_concern)
	{
		querystatistics("INSERT");
		bson_error_t error = {};
		mongoc_collection_t* collection = mongoc_database_get_collection(database, tableName);
		bson_t options;
		bson_init(&options);

		if ((flags & MONGOC_INSERT_NO_VALIDATE) != 0)
			BSON_APPEND_BOOL(&options, "bypassDocumentValidation", true);
		if (write_concern)
			mongoc_write_concern_append(const_cast<mongoc_write_concern_t*>(write_concern), &options);

		bool r = appendSession(&options) &&
			mongoc_collection_insert_one(collection, document, &options, NULL, &error);
		if (!r)
		{
			if (error.message[0] != '\0')
				setLastError(error);
			ERROR_MSG(fmt::format("{}\n", error.message));
		}

		bson_destroy(&options);
		mongoc_collection_destroy(collection);

		if (!r)
			throwError();

		return true;
	}

	std::unique_ptr<MongoCursorGuard> DBInterfaceMongodb::collectionFind(const char* tableName, mongoc_query_flags_t flags, uint32_t skip, uint32_t limit, uint32_t  batch_size, const bson_t* query, const bson_t* fields, const mongoc_read_prefs_t* read_prefs)
	{
		querystatistics("SELECT");

		mongoc_collection_t* collection =
			mongoc_database_get_collection(database, tableName);

		bson_t opts;
		bson_init(&opts);

		// 旧查询标志映射到 find 命令选项；SECONDARY_OK 仍由调用方传入的 read preference 控制。
		// Map legacy query flags to find command options; SECONDARY_OK remains controlled by the caller's read preference.
		if ((flags & MONGOC_QUERY_TAILABLE_CURSOR) != 0)
			BSON_APPEND_BOOL(&opts, "tailable", true);
		if ((flags & MONGOC_QUERY_NO_CURSOR_TIMEOUT) != 0)
			BSON_APPEND_BOOL(&opts, "noCursorTimeout", true);
		if ((flags & MONGOC_QUERY_AWAIT_DATA) != 0)
			BSON_APPEND_BOOL(&opts, "awaitData", true);
		if ((flags & MONGOC_QUERY_PARTIAL) != 0)
			BSON_APPEND_BOOL(&opts, "allowPartialResults", true);

		if (skip > 0)
			BSON_APPEND_INT64(&opts, "skip", skip);

		if (limit > 0)
			BSON_APPEND_INT64(&opts, "limit", limit);

		if (batch_size > 0)
			BSON_APPEND_INT64(&opts, "batchSize", batch_size);

		if (fields)
			BSON_APPEND_DOCUMENT(&opts, "projection", fields);

		if (!appendSession(&opts))
		{
			bson_destroy(&opts);
			mongoc_collection_destroy(collection);
			throwError();
		}

		mongoc_cursor_t* cursor =
			mongoc_collection_find_with_opts(
				collection,
				query,
				&opts,
				read_prefs);

		bson_destroy(&opts);

		return std::make_unique<MongoCursorGuard>(collection, cursor);
	}

	bool DBInterfaceMongodb::updateCollection(const char* tableName, mongoc_update_flags_t uflags, const bson_t* selector, const bson_t* update, const mongoc_write_concern_t* write_concern)
	{
		querystatistics("UPDATE");
		bson_error_t error = {};
		mongoc_collection_t* collection = mongoc_database_get_collection(database, tableName);
		bson_t options;
		bson_init(&options);

		if ((uflags & MONGOC_UPDATE_UPSERT) != 0)
			BSON_APPEND_BOOL(&options, "upsert", true);
		if ((uflags & MONGOC_UPDATE_NO_VALIDATE) != 0)
			BSON_APPEND_BOOL(&options, "bypassDocumentValidation", true);
		if (write_concern)
			mongoc_write_concern_append(const_cast<mongoc_write_concern_t*>(write_concern), &options);

		bool r = appendSession(&options);
		if (r && (uflags & MONGOC_UPDATE_MULTI_UPDATE) != 0)
			r = mongoc_collection_update_many(collection, selector, update, &options, NULL, &error);
		else if (r)
			r = mongoc_collection_update_one(collection, selector, update, &options, NULL, &error);

		if (!r)
		{
			if (error.message[0] != '\0')
				setLastError(error);
			ERROR_MSG(fmt::format("{}\n", error.message));
		}

		bson_destroy(&options);
		mongoc_collection_destroy(collection);
		if (!r)
			throwError();

		return true;
	}

	bool DBInterfaceMongodb::collectionRemove(const char* tableName, mongoc_remove_flags_t flags, const bson_t* selector, const mongoc_write_concern_t* write_concern)
	{
		querystatistics("DELETE");
		bson_error_t error = {};
		mongoc_collection_t* collection = mongoc_database_get_collection(database, tableName);
		bson_t options;
		bson_init(&options);
		if (write_concern)
			mongoc_write_concern_append(const_cast<mongoc_write_concern_t*>(write_concern), &options);

		bool r = appendSession(&options);
		if (r && (flags & MONGOC_REMOVE_SINGLE_REMOVE) != 0)
			r = mongoc_collection_delete_one(collection, selector, &options, NULL, &error);
		else if (r)
			r = mongoc_collection_delete_many(collection, selector, &options, NULL, &error);

		if (!r)
		{
			if (error.message[0] != '\0')
				setLastError(error);
			ERROR_MSG(fmt::format("{}\n", error.message));
		}

		bson_destroy(&options);
		mongoc_collection_destroy(collection);
		if (!r)
			throwError();

		return true;
	}

	std::unique_ptr<MongoCursorGuard> DBInterfaceMongodb::collectionFindIndexes(const char* tableName)
	{
		querystatistics("SELECT_INDEX");

		mongoc_collection_t* collection =
			mongoc_database_get_collection(database, tableName);

		bson_t opts;
		bson_init(&opts);
		if (!appendSession(&opts))
		{
			bson_destroy(&opts);
			mongoc_collection_destroy(collection);
			throwError();
		}

		mongoc_cursor_t* cursor =
			mongoc_collection_find_indexes_with_opts(
				collection,
				&opts);

		bson_destroy(&opts);

		return std::make_unique<MongoCursorGuard>(collection, cursor);
	}

	bool DBInterfaceMongodb::collectionCreateIndex(const char* tableName, const bson_t* keys, const bson_t* opt)
	{
		querystatistics("CREATE_INDEX");

		mongoc_collection_t* collection =
			mongoc_database_get_collection(database, tableName);

		bson_error_t error = {};
		bool result = false;

		// 索引模型拥有键和索引级选项的只读引用，并在命令结束后立即释放。
		// The index model borrows key and index-option documents and is released immediately after the command.
		mongoc_index_model_t* model =
			mongoc_index_model_new(keys, opt);

		if (!model)
		{
			ERROR_MSG("Failed to create index model\n");
			mongoc_collection_destroy(collection);
			return false;
		}

		mongoc_index_model_t* models[] = { model };
		bson_t commandOptions;
		bson_init(&commandOptions);

		result = appendSession(&commandOptions) &&
			mongoc_collection_create_indexes_with_opts(collection, models, 1, &commandOptions, NULL, &error);

		if (!result)
		{
			if (error.message[0] != '\0')
				setLastError(error);
			ERROR_MSG(fmt::format("{}\n", error.message));
		}

		bson_destroy(&commandOptions);
		mongoc_index_model_destroy(model);
		mongoc_collection_destroy(collection);

		return result;
	}

	bool DBInterfaceMongodb::collectionDropIndex(const char* tableName, const char* index_name)
	{
		querystatistics("DROP_INDEX");
		mongoc_collection_t* collection = mongoc_database_get_collection(database, tableName);

		bson_error_t error = {};
		bson_t options;
		bson_init(&options);
		bool r = appendSession(&options) &&
			mongoc_collection_drop_index_with_opts(collection, index_name, &options, &error);
		if (!r)
		{
			if (error.message[0] != '\0')
				setLastError(error);
			ERROR_MSG(fmt::format("{}\n", error.message));
		}

		bson_destroy(&options);
		mongoc_collection_destroy(collection);

		return r;
	}

	bool DBInterfaceMongodb::getTopLevelKey(const bson_t* doc, std::string& key)
	{
		bson_iter_t iter;
		if (!bson_iter_init(&iter, doc))
			return false;

		if (!bson_iter_next(&iter))
			return false;

		key = bson_iter_key(&iter);
		return true;
	}

	bool DBInterfaceMongodb::extuteFunction(const bson_t* command, const mongoc_read_prefs_t* read_prefs, bson_t* reply)
	{
		querystatistics("JSON_FUNCTION");
		bson_error_t error = {};
		bson_t options;
		bson_init(&options);
		if (!appendSession(&options))
		{
			bson_destroy(&options);
			bson_init(reply);
			return false;
		}

		bool r = mongoc_database_command_with_opts(database, command, read_prefs, &options, reply, &error);
		if (!r)
		{
			if (error.message[0] != '\0')
				setLastError(error);
			ERROR_MSG(fmt::format("{}\n", error.message));
		}

		bson_destroy(&options);
		return r;
	}

	bool DBInterfaceMongodb::executeFindCommand(MemoryStream* result, std::vector<std::string> strcmd, const char* tableName)
	{
		if (!result || strcmd.empty())
		{
			setLastError("MongoDB find command requires a result stream and query document");
			return false;
		}

		bool flag = true;
		std::string query = "";
		std::string field = "";
		int limit = 0;
		int skip = 0;
		int batchSize = 0;
		int options = 0;
		std::size_t size = strcmd.size();
		query = strcmd[0];

		if (size >= 2)
		{
			field = strcmd[1];
		}

		if (size >= 3)
		{
			limit = atoi(strcmd[2].c_str());
		}

		if (size >= 4)
		{
			skip = atoi(strcmd[3].c_str());
		}

		if (size >= 5)
		{
			batchSize = atoi(strcmd[4].c_str());
		}

		if (size >= 6)
		{
			options = atoi(strcmd[5].c_str());
		}

		bson_error_t error = {};
		bson_t* q = bson_new_from_json(reinterpret_cast<const uint8_t*>(query.c_str()), static_cast<ssize_t>(query.length()), &error);
		if (!q)
		{
			setLastError(error);
			ERROR_MSG(fmt::format(" DBInterfaceMongodb::executeFindCommand:query error: {}\n", error.message));
			return false;
		}

		bson_t* f = NULL;
		if (field != "")
		{
			f = bson_new_from_json(reinterpret_cast<const uint8_t*>(field.c_str()), static_cast<ssize_t>(field.length()), &error);
			if (!f)
			{
				setLastError(error);
				ERROR_MSG(fmt::format(" DBInterfaceMongodb::executeFindCommand:field error: {}\n", error.message));
				bson_destroy(q);
				return false;
			}
		}

		mongoc_query_flags_t queryOptions = (mongoc_query_flags_t)options;
		const bson_t* doc;
		std::unique_ptr<MongoCursorGuard> guard = collectionFind(tableName, queryOptions, skip, limit, batchSize, q, f, NULL);
		if (!guard || !guard->cursor())
		{
			bson_destroy(q);
			if (f)
				bson_destroy(f);
			return false;
		}

		uint32 nrows = 0;
		uint32 nfields = 1;

		std::vector<char*> value;
		while (mongoc_cursor_next(guard->cursor(), &doc))
		{
			nrows++;
			char* str = bson_as_relaxed_extended_json(doc, NULL);
			value.push_back(str);
		}

		(*result) << nfields << nrows;

		std::vector<char*>::iterator it;
		for (it = value.begin(); it != value.end(); it++)
		{
			result->appendBlob(*it, static_cast<KBEngine::ArraySize>(strlen(*it)));
			bson_free(*it);
		}

		if (mongoc_cursor_error(guard->cursor(), &error))
		{
			setLastError(error);
			ERROR_MSG(fmt::format("An error occurred: {}\n", error.message));
			flag = false;
		}

		bson_destroy(q);
		if (f)
		{
			bson_destroy(f);
		}
		return flag;
	}

	bool DBInterfaceMongodb::executeUpdateCommand(std::vector<std::string> strcmd, const char* tableName)
	{
		if (strcmd.size() < 2)
		{
			setLastError("MongoDB update command requires selector and update documents");
			return false;
		}

		bool successFlag = false;
		std::string query = "";
		std::string update = "";
		std::size_t size = strcmd.size();
		query = strcmd[0];

		bool upsert = false;
		bool multi = false;

		if (size >= 2)
		{
			update = strcmd[1];
		}

		if (size >= 3)
		{
			std::istringstream(strcmd[2]) >> std::boolalpha >> upsert;
		}

		if (size >= 4)
		{
			std::istringstream(strcmd[3]) >> std::boolalpha >> multi;
		}

		bson_error_t error = {};
		bson_t* q = bson_new_from_json(reinterpret_cast<const uint8_t*>(query.c_str()), static_cast<ssize_t>(query.length()), &error);
		if (!q)
		{
			setLastError(error);
			ERROR_MSG(fmt::format("{}\n", error.message));
			return false;
		}

		bson_t* u = bson_new_from_json(reinterpret_cast<const uint8_t*>(update.c_str()), static_cast<ssize_t>(update.length()), &error);
		if (!u)
		{
			setLastError(error);
			ERROR_MSG(fmt::format("{}\n", error.message));
			bson_destroy(q);
			return false;
		}


		mongoc_update_flags_t uflags = MONGOC_UPDATE_NONE;
		if (upsert)
			uflags = static_cast<mongoc_update_flags_t>(uflags | MONGOC_UPDATE_UPSERT);
		if (multi)
			uflags = static_cast<mongoc_update_flags_t>(uflags | MONGOC_UPDATE_MULTI_UPDATE);

		successFlag = updateCollection(tableName, uflags, q, u, NULL);

		bson_destroy(q);
		bson_destroy(u);
		return successFlag;
	}

	bool DBInterfaceMongodb::executeRemoveCommand(std::vector<std::string> strcmd, const char* tableName)
	{
		if (strcmd.empty())
		{
			setLastError("MongoDB remove command requires a selector document");
			return false;
		}

		bool successFlag = false;
		std::string query = "";
		bool justOne = false;
		std::size_t size = strcmd.size();
		query = strcmd[0];

		if (size >= 2)
		{
			std::istringstream(strcmd[1]) >> std::boolalpha >> justOne;
		}

		bson_error_t error = {};
		bson_t* q = bson_new_from_json(reinterpret_cast<const uint8_t*>(query.c_str()), static_cast<ssize_t>(query.length()), &error);
		if (!q)
		{
			setLastError(error);
			ERROR_MSG(fmt::format("{}\n", error.message));
			return false;
		}

		mongoc_remove_flags_t flags;
		if (justOne)
		{
			flags = MONGOC_REMOVE_SINGLE_REMOVE;
		}
		else
		{
			flags = MONGOC_REMOVE_NONE;
		}

		successFlag = collectionRemove(tableName, flags, q, NULL);

		bson_destroy(q);
		return successFlag;
	}

	bool DBInterfaceMongodb::executeInsertCommand(std::vector<std::string> strcmd, const char* tableName)
	{
		if (strcmd.empty())
		{
			setLastError("MongoDB insert command requires a document");
			return false;
		}

		bool successFlag = false;
		std::string query = "";
		bool ordered = true;
		std::size_t size = strcmd.size();
		query = strcmd[0];

		if (size >= 2)
		{
			if (strcmd[1].find("false") != std::string::npos)
			{
				ordered = false;
			}
		}

		bson_error_t error = {};
		bson_t* q = bson_new_from_json(reinterpret_cast<const uint8_t*>(query.c_str()), static_cast<ssize_t>(query.length()), &error);
		if (!q)
		{
			setLastError(error);
			ERROR_MSG(fmt::format("{}\n", error.message));
			return false;
		}

		mongoc_insert_flags_t flags;
		if (ordered)
		{
			flags = MONGOC_INSERT_NONE;
		}
		else
		{
			flags = MONGOC_INSERT_CONTINUE_ON_ERROR;
		}

		successFlag = insertCollection(tableName, flags, q, NULL);

		bson_destroy(q);
		return successFlag;
	}

	// 执行 MongoDB 原生命令并把扩展 JSON 响应写入 KBEngine 查询结果流。
	// Execute a native MongoDB command and write its extended-JSON response into the KBEngine query result stream.
	bool DBInterfaceMongodb::executeFunctionCommand(MemoryStream* result, std::string strcmd)
	{
		if (!result)
		{
			setLastError("MongoDB command requires a result stream");
			return false;
		}

		bson_error_t error = {};
		bson_t* q = bson_new_from_json(reinterpret_cast<const uint8_t*>(strcmd.c_str()), static_cast<ssize_t>(strcmd.length()), &error);
		if (!q)
		{
			setLastError(error);
			ERROR_MSG(fmt::format("{}\n", error.message));
			return false;
		}


		std::string topKey;
		if (!getTopLevelKey(q, topKey))
		{
			bson_destroy(q);
			setLastError("Invalid MongoDB command");
			return false;
		}


		bson_t reply;

		bool r = extuteFunction(q, NULL, &reply);
		if (r)
		{
			uint32 nrows = 1;
			uint32 nfields = 1;
			(*result) << nfields << nrows;

			char* str = bson_as_relaxed_extended_json(&reply, NULL);
			result->appendBlob(str, static_cast<KBEngine::ArraySize>(strlen(str)));
			bson_free(str);
		}

		bson_destroy(q);
		bson_destroy(&reply);

		return r;
	}

	std::vector<std::string> DBInterfaceMongodb::splitParameter(std::string value)
	{
		std::vector<std::string> result;
		std::string current;
		int depth = 0;
		bool inString = false;
		bool escaped = false;

		// 只在 JSON 文档之外切分逗号；字符串内的逗号、括号和转义引号必须原样保留。
		// Split commas only outside JSON documents; commas, brackets, and escaped quotes inside strings remain untouched.
		for (std::string::const_iterator iter = value.begin(); iter != value.end(); ++iter)
		{
			const char ch = *iter;
			if (inString)
			{
				current += ch;
				if (escaped)
					escaped = false;
				else if (ch == '\\')
					escaped = true;
				else if (ch == '"')
					inString = false;
				continue;
			}

			if (ch == '"')
			{
				inString = true;
				current += ch;
			}
			else if (ch == '{' || ch == '[')
			{
				++depth;
				current += ch;
			}
			else if (ch == '}' || ch == ']')
			{
				--depth;
				current += ch;
			}
			else if (ch == ',' && depth == 0)
			{
				const std::string::size_type first = current.find_first_not_of(" \t\r\n");
				const std::string::size_type last = current.find_last_not_of(" \t\r\n");
				if (first != std::string::npos)
					result.push_back(current.substr(first, last - first + 1));
				current.clear();
			}
			else
			{
				current += ch;
			}
		}

		const std::string::size_type first = current.find_first_not_of(" \t\r\n");
		const std::string::size_type last = current.find_last_not_of(" \t\r\n");
		if (first != std::string::npos)
			result.push_back(current.substr(first, last - first + 1));

		return result;
	}



}
