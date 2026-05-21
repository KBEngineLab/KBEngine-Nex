#include "entity_table_mongodb.h"
#include "kbe_table_mongodb.h"
#include "db_exception.h"
#include "db_interface_mongodb.h"
#include "mongo_cursor_guard.h"
#include "db_interface/db_interface.h"
#include "db_interface/entity_table.h"
#include "entitydef/entitydef.h"
#include "entitydef/scriptdef_module.h"
#include "server/serverconfig.h"
#include "server/common.h"

namespace KBEngine {
	namespace
	{
		DBInterfaceMongodb* mongoDBI(DBInterface* pdbi)
		{
			return static_cast<DBInterfaceMongodb*>(pdbi);
		}

		bool append_int64_in_query(bson_t* query, const char* field, const std::vector<COMPONENT_ID>& values, bool skipSelf)
		{
			bson_t in;
			bson_t arr;
			bson_append_document_begin(query, field, -1, &in);
			bson_append_array_begin(&in, "$in", -1, &arr);

			uint32 idx = 0;
			char key[16];
			for (std::vector<COMPONENT_ID>::const_iterator iter = values.begin(); iter != values.end(); ++iter)
			{
				if (skipSelf && (*iter) == (uint64)getUserUID())
					continue;

				kbe_snprintf(key, sizeof(key), "%u", idx++);
				BSON_APPEND_INT64(&arr, key, *iter);
			}

			bson_append_array_end(&in, &arr);
			bson_append_document_end(query, &in);
			return idx > 0;
		}

		bool get_int64_field(const bson_t* doc, const char* field, uint64& value)
		{
			bson_iter_t iter;
			if (!bson_iter_init_find(&iter, doc, field))
				return false;

			if (BSON_ITER_HOLDS_INT32(&iter))
			{
				value = bson_iter_int32(&iter);
				return true;
			}

			if (BSON_ITER_HOLDS_INT64(&iter))
			{
				value = bson_iter_int64(&iter);
				return true;
			}

			return false;
		}

		bool get_int32_field(const bson_t* doc, const char* field, int32& value)
		{
			bson_iter_t iter;
			if (!bson_iter_init_find(&iter, doc, field))
				return false;

			if (BSON_ITER_HOLDS_INT32(&iter))
			{
				value = bson_iter_int32(&iter);
				return true;
			}

			if (BSON_ITER_HOLDS_INT64(&iter))
			{
				value = static_cast<int32>(bson_iter_int64(&iter));
				return true;
			}

			return false;
		}

		bool get_utf8_field(const bson_t* doc, const char* field, std::string& value)
		{
			bson_iter_t iter;
			if (!bson_iter_init_find(&iter, doc, field) || !BSON_ITER_HOLDS_UTF8(&iter))
				return false;

			uint32_t len = 0;
			const char* str = bson_iter_utf8(&iter, &len);
			value.assign(str, len);
			return true;
		}

		bool get_bool_field(const bson_t* doc, const char* field, bool& value)
		{
			bson_iter_t iter;
			if (!bson_iter_init_find(&iter, doc, field))
				return false;

			if (BSON_ITER_HOLDS_BOOL(&iter))
			{
				value = bson_iter_bool(&iter);
				return true;
			}

			if (BSON_ITER_HOLDS_INT32(&iter))
			{
				value = bson_iter_int32(&iter) != 0;
				return true;
			}

			if (BSON_ITER_HOLDS_INT64(&iter))
			{
				value = bson_iter_int64(&iter) != 0;
				return true;
			}

			return false;
		}

		bool get_blob_field(const bson_t* doc, const char* field, std::string& value)
		{
			bson_iter_t iter;
			if (!bson_iter_init_find(&iter, doc, field))
				return false;

			if (BSON_ITER_HOLDS_BINARY(&iter))
			{
				bson_subtype_t subtype;
				uint32_t len = 0;
				const uint8_t* data = NULL;
				bson_iter_binary(&iter, &subtype, &len, &data);
				value.assign(reinterpret_cast<const char*>(data), len);
				return true;
			}

			if (BSON_ITER_HOLDS_UTF8(&iter))
				return get_utf8_field(doc, field, value);

			return false;
		}

		bool find_one(DBInterface* pdbi, const char* collection, const bson_t* query, const bson_t** doc, std::unique_ptr<MongoCursorGuard>& guard)
		{
			guard = mongoDBI(pdbi)->collectionFind(collection, MONGOC_QUERY_NONE, 0, 1, 0, query, NULL, NULL);
			if (!mongoc_cursor_next(guard->cursor(), doc))
			{
				bson_error_t error;
				if (mongoc_cursor_error(guard->cursor(), &error))
					ERROR_MSG(fmt::format("find_one({}): {}\n", collection, error.message));

				return false;
			}

			return true;
		}

		void append_account_name_or_email_query(bson_t* query, const std::string& name)
		{
			bson_t orArray;
			bson_t byName;
			bson_t byEmail;

			bson_append_array_begin(query, "$or", -1, &orArray);

			bson_append_document_begin(&orArray, "0", -1, &byName);
			bson_append_utf8(&byName, "accountName", -1, name.c_str(), static_cast<int>(name.size()));
			bson_append_document_end(&orArray, &byName);

			bson_append_document_begin(&orArray, "1", -1, &byEmail);
			bson_append_utf8(&byEmail, "email", -1, name.c_str(), static_cast<int>(name.size()));
			bson_append_document_end(&orArray, &byEmail);

			bson_append_array_end(query, &orArray);
		}

		bool find_email_verification(DBInterface* pdbi, int8 type, const char* field, const std::string& value,
			std::string& accountName, std::string& datas, uint64& logtime)
		{
			bson_t query;
			bson_init(&query);
			BSON_APPEND_INT32(&query, "type", type);
			bson_append_utf8(&query, field, -1, value.c_str(), static_cast<int>(value.size()));

			const bson_t* doc = NULL;
			std::unique_ptr<MongoCursorGuard> guard;
			bool found = find_one(pdbi, "kbe_email_verification", &query, &doc, guard);
			if (found)
			{
				get_utf8_field(doc, "accountName", accountName);
				get_utf8_field(doc, "datas", datas);
				get_int64_field(doc, "logtime", logtime);
			}

			bson_destroy(&query);
			return found;
		}
	}

	bool KBEEntityLogTableMongodb::syncToDB(DBInterface* pdbi)
	{
		KBEServerLogTableMongodb serverLogTable(NULL);

		int ret = serverLogTable.isShareDB(pdbi);
		if (ret == -1)
			return false;

		if (ret == 0)
		{
			bson_t empty;
			bson_init(&empty);
			mongoDBI(pdbi)->collectionRemove("kbe_entitylog", MONGOC_REMOVE_NONE, &empty, NULL);
			bson_destroy(&empty);
		}

		DBInterfaceMongodb* pdbiMongodb = mongoDBI(pdbi);
		pdbiMongodb->createCollection("kbe_entitylog");

		bson_t keys;
		bson_t opts;
		bson_init(&keys);
		bson_init(&opts);
		BSON_APPEND_INT32(&keys, "entityDBID", 1);
		BSON_APPEND_INT32(&keys, "entityType", 1);
		BSON_APPEND_BOOL(&opts, "unique", true);
		pdbiMongodb->collectionCreateIndex("kbe_entitylog", &keys, &opts);
		bson_destroy(&opts);
		bson_destroy(&keys);

		bson_init(&keys);
		bson_init(&opts);
		BSON_APPEND_INT32(&keys, "serverGroupID", 1);
		pdbiMongodb->collectionCreateIndex("kbe_entitylog", &keys, &opts);
		bson_destroy(&opts);
		bson_destroy(&keys);

		std::vector<COMPONENT_ID> cids = serverLogTable.queryTimeOutServers(pdbi);
		if (!serverLogTable.clearServers(pdbi, cids))
			return false;

		cids.push_back((uint64)getUserUID());
		if (!cids.empty())
		{
			bson_t query;
			bson_init(&query);
			if (append_int64_in_query(&query, "serverGroupID", cids, false))
				pdbiMongodb->collectionRemove("kbe_entitylog", MONGOC_REMOVE_NONE, &query, NULL);
			bson_destroy(&query);
		}

		std::vector<COMPONENT_ID> servers = serverLogTable.queryServers(pdbi);
		std::vector<COMPONENT_ID> erases;

		bson_t query;
		bson_init(&query);
		std::unique_ptr<MongoCursorGuard> guard = pdbiMongodb->collectionFind("kbe_entitylog", MONGOC_QUERY_NONE, 0, 0, 0, &query, NULL, NULL);
		const bson_t* doc = NULL;
		while (mongoc_cursor_next(guard->cursor(), &doc))
		{
			uint64 serverGroupID = 0;
			if (get_int64_field(doc, "serverGroupID", serverGroupID) &&
				std::find(servers.begin(), servers.end(), serverGroupID) == servers.end() &&
				std::find(erases.begin(), erases.end(), serverGroupID) == erases.end())
			{
				erases.push_back(serverGroupID);
			}
		}

		bson_destroy(&query);

		if (!erases.empty())
		{
			bson_init(&query);
			if (append_int64_in_query(&query, "serverGroupID", erases, false))
				pdbiMongodb->collectionRemove("kbe_entitylog", MONGOC_REMOVE_NONE, &query, NULL);
			bson_destroy(&query);
		}

		return true;
	}

	bool KBEEntityLogTableMongodb::logEntity(DBInterface* pdbi, const char* ip, uint32 port, DBID dbid,
		COMPONENT_ID componentID, ENTITY_ID entityID, ENTITY_SCRIPT_UID entityType)
	{
		bson_t options;
		bson_init(&options);
		BSON_APPEND_INT64(&options, "entityDBID", dbid);
		BSON_APPEND_INT32(&options, "entityType", entityType);
		BSON_APPEND_INT32(&options, "entityID", entityID);
		BSON_APPEND_UTF8(&options, "ip", ip);
		BSON_APPEND_INT32(&options, "port", port);
		BSON_APPEND_INT64(&options, "componentID", componentID);
		BSON_APPEND_INT64(&options, "serverGroupID", (uint64)getUserUID());

		DBInterfaceMongodb* pdbiMongodb = static_cast<DBInterfaceMongodb*>(pdbi);
		bool ret = pdbiMongodb->insertCollection("kbe_entitylog", MONGOC_INSERT_NONE, &options, NULL);

		bson_destroy(&options);

		return ret;
	}

	bool KBEEntityLogTableMongodb::queryEntity(DBInterface* pdbi, DBID dbid, EntityLog& entitylog, ENTITY_SCRIPT_UID entityType)
	{
		bson_t query;
		bson_init(&query);
		BSON_APPEND_INT64(&query, "entityDBID", dbid);
		BSON_APPEND_INT32(&query, "entityType", entityType);

		DBInterfaceMongodb* pdbiMongodb = static_cast<DBInterfaceMongodb*>(pdbi);
		//const std::list<const bson_t *> value = pdbiMongodb->collectionFind("kbe_entitylog", &query);

		// mongoc_cursor_t* cursor = pdbiMongodb->collectionFind("kbe_entitylog", MONGOC_QUERY_NONE, 0, 0, 0, &query, NULL, NULL);
		std::unique_ptr<MongoCursorGuard> guard = pdbiMongodb->collectionFind("kbe_entitylog", MONGOC_QUERY_NONE, 0, 0, 0, &query, NULL, NULL);

		entitylog.dbid = dbid;
		entitylog.componentID = 0;
		entitylog.serverGroupID = 0;
		entitylog.entityID = 0;
		entitylog.ip[0] = '\0';
		entitylog.port = 0;


		const bson_t* doc = NULL;
		bson_error_t  error;
		while (mongoc_cursor_more(guard->cursor()) && mongoc_cursor_next(guard->cursor(), &doc)) {
			break;
		}

		if (mongoc_cursor_error(guard->cursor(), &error)) {
			ERROR_MSG(fmt::format("An error occurred: {}\n", error.message));
		}

		if (doc == NULL)
		{
			// mongoc_cursor_destroy(cursor);
			bson_destroy(&query);
			return false;
		}

		int32 value32 = 0;
		uint64 value64 = 0;
		std::string value;

		if (get_int32_field(doc, "entityID", value32))
			entitylog.entityID = value32;

		if (get_utf8_field(doc, "ip", value))
			kbe_snprintf(entitylog.ip, MAX_IP, "%s", value.c_str());

		if (get_int32_field(doc, "port", value32))
			entitylog.port = static_cast<uint16>(value32);

		if (get_int64_field(doc, "componentID", value64))
			entitylog.componentID = value64;

		if (get_int64_field(doc, "serverGroupID", value64))
			entitylog.serverGroupID = value64;

		bson_destroy(&query);
		return entitylog.componentID > 0;
	}

	bool KBEEntityLogTableMongodb::eraseEntityLog(DBInterface* pdbi, DBID dbid, ENTITY_SCRIPT_UID entityType)
	{
		bool r = true;
		bson_t doc;
		bson_init(&doc);
		BSON_APPEND_INT64(&doc, "entityDBID", dbid);
		BSON_APPEND_INT32(&doc, "entityType", entityType);


		DBInterfaceMongodb* pdbiMongodb = static_cast<DBInterfaceMongodb*>(pdbi);
		r = pdbiMongodb->collectionRemove("kbe_entitylog", MONGOC_REMOVE_SINGLE_REMOVE, &doc, NULL);

		bson_destroy(&doc);

		return r;
	}

	bool KBEEntityLogTableMongodb::eraseBaseappEntityLog(DBInterface* pdbi, COMPONENT_ID componentID)
	{
		bson_t query;
		bson_init(&query);
		BSON_APPEND_INT64(&query, "componentID", componentID);

		bool ret = mongoDBI(pdbi)->collectionRemove("kbe_entitylog", MONGOC_REMOVE_NONE, &query, NULL);
		bson_destroy(&query);
		return ret;
	}

	KBEEntityLogTableMongodb::KBEEntityLogTableMongodb(EntityTables* pEntityTables) :
		KBEEntityLogTable(pEntityTables)
	{
	}

	//-------------------------------------------------------------------------------------
	bool KBEServerLogTableMongodb::syncToDB(DBInterface* pdbi)
	{
		DBInterfaceMongodb* pdbiMongodb = mongoDBI(pdbi);
		pdbiMongodb->createCollection("kbe_serverlog");

		bson_t keys;
		bson_t opts;
		bson_init(&keys);
		bson_init(&opts);
		BSON_APPEND_INT32(&keys, "serverGroupID", 1);
		BSON_APPEND_BOOL(&opts, "unique", true);
		pdbiMongodb->collectionCreateIndex("kbe_serverlog", &keys, &opts);
		bson_destroy(&opts);
		bson_destroy(&keys);

		return updateServer(pdbi);
	}

	//-------------------------------------------------------------------------------------
	bool KBEServerLogTableMongodb::updateServer(DBInterface* pdbi)
	{
		int shareDB = isShareDB(pdbi);
		if (shareDB == -1)
			return false;

		bson_t query;
		bson_init(&query);
		BSON_APPEND_INT64(&query, "serverGroupID", (uint64)getUserUID());

		bson_t values;
		bson_init(&values);
		BSON_APPEND_INT64(&values, "heartbeatTime", time(NULL));
		BSON_APPEND_BOOL(&values, "isShareDB", shareDB > 0);
		BSON_APPEND_INT64(&values, "serverGroupID", (uint64)getUserUID());

		bson_t update;
		bson_init(&update);
		BSON_APPEND_DOCUMENT(&update, "$set", &values);

		bool ret = mongoDBI(pdbi)->updateCollection("kbe_serverlog", MONGOC_UPDATE_UPSERT, &query, &update, NULL);

		bson_destroy(&update);
		bson_destroy(&values);
		bson_destroy(&query);
		return ret;
	}

	//-------------------------------------------------------------------------------------
	bool KBEServerLogTableMongodb::queryServer(DBInterface* pdbi, ServerLog& serverlog)
	{
		bson_t query;
		bson_init(&query);
		BSON_APPEND_INT64(&query, "serverGroupID", (uint64)getUserUID());

		serverlog.heartbeatTime = 0;
		serverlog.serverGroupID = (uint64)getUserUID();
		serverlog.isShareDB = 0;

		const bson_t* doc = NULL;
		std::unique_ptr<MongoCursorGuard> guard;
		bool found = find_one(pdbi, "kbe_serverlog", &query, &doc, guard);
		if (found)
		{
			uint64 v = 0;
			if (get_int64_field(doc, "heartbeatTime", v))
				serverlog.heartbeatTime = v;

			if (get_int64_field(doc, "serverGroupID", v))
				serverlog.serverGroupID = v;

			bool isShareDB = false;
			if (get_bool_field(doc, "isShareDB", isShareDB))
				serverlog.isShareDB = isShareDB ? 1 : 0;
		}

		bson_destroy(&query);
		return found;
	}

	//-------------------------------------------------------------------------------------
	std::vector<COMPONENT_ID> KBEServerLogTableMongodb::queryServers(DBInterface* pdbi)
	{
		std::vector<COMPONENT_ID> cids;

		bson_t query;
		bson_init(&query);
		std::unique_ptr<MongoCursorGuard> guard = mongoDBI(pdbi)->collectionFind("kbe_serverlog", MONGOC_QUERY_NONE, 0, 0, 0, &query, NULL, NULL);
		const bson_t* doc = NULL;
		while (mongoc_cursor_next(guard->cursor(), &doc))
		{
			uint64 serverGroupID = 0;
			if (get_int64_field(doc, "serverGroupID", serverGroupID))
				cids.push_back(serverGroupID);
		}

		bson_destroy(&query);
		return cids;
	}

	//-------------------------------------------------------------------------------------
	std::vector<COMPONENT_ID> KBEServerLogTableMongodb::queryTimeOutServers(DBInterface* pdbi)
	{
		std::vector<COMPONENT_ID> cids;

		bson_t query;
		bson_init(&query);
		std::unique_ptr<MongoCursorGuard> guard = mongoDBI(pdbi)->collectionFind("kbe_serverlog", MONGOC_QUERY_NONE, 0, 0, 0, &query, NULL, NULL);
		const bson_t* doc = NULL;
		while (mongoc_cursor_next(guard->cursor(), &doc))
		{
			uint64 heartbeatTime = 0;
			uint64 serverGroupID = 0;
			if (!get_int64_field(doc, "heartbeatTime", heartbeatTime) ||
				!get_int64_field(doc, "serverGroupID", serverGroupID))
				continue;

			if (serverGroupID == (uint64)getUserUID())
				continue;

			if ((uint64)time(NULL) > heartbeatTime + KBEServerLogTable::TIMEOUT * 2)
				cids.push_back(serverGroupID);
		}

		bson_destroy(&query);
		return cids;
	}

	//-------------------------------------------------------------------------------------
	bool KBEServerLogTableMongodb::clearServers(DBInterface* pdbi, const std::vector<COMPONENT_ID>& cids)
	{
		if (cids.empty())
			return true;

		bson_t query;
		bson_init(&query);
		if (!append_int64_in_query(&query, "serverGroupID", cids, true))
		{
			bson_destroy(&query);
			return true;
		}

		bool ret = mongoDBI(pdbi)->collectionRemove("kbe_serverlog", MONGOC_REMOVE_NONE, &query, NULL);
		bson_destroy(&query);
		return ret;
	}

	std::map<COMPONENT_ID, bool> KBEServerLogTableMongodb::queryAllServerShareDBState(DBInterface* pdbi)
	{
		std::vector<COMPONENT_ID> cids = queryTimeOutServers(pdbi);
		clearServers(pdbi, cids);

		std::map<COMPONENT_ID, bool> cidMap;

		bson_t query;
		bson_init(&query);
		std::unique_ptr<MongoCursorGuard> guard = mongoDBI(pdbi)->collectionFind("kbe_serverlog", MONGOC_QUERY_NONE, 0, 0, 0, &query, NULL, NULL);
		const bson_t* doc = NULL;
		while (mongoc_cursor_next(guard->cursor(), &doc))
		{
			uint64 serverGroupID = 0;
			if (!get_int64_field(doc, "serverGroupID", serverGroupID))
				continue;

			bool isShareDB = false;
			get_bool_field(doc, "isShareDB", isShareDB);

			cidMap.insert(std::make_pair(serverGroupID, isShareDB));
		}

		bson_destroy(&query);
		return cidMap;
	}

	int KBEServerLogTableMongodb::isShareDB(DBInterface* pdbi)
	{
		bool isShareDB = g_kbeSrvConfig.getDBMgr().isShareDB;
		uint64 uid = getUserUID();

		try
		{
			std::map<COMPONENT_ID, bool> cidMap = queryAllServerShareDBState(pdbi);
			std::map<COMPONENT_ID, bool>::const_iterator citer = cidMap.begin();
			for (; citer != cidMap.end(); ++citer)
			{
				if (citer->first != uid)
				{
					bool isOtherServerShareDB = citer->second;
					if (!isOtherServerShareDB || (isOtherServerShareDB && !isShareDB))
					{
						ERROR_MSG(fmt::format("KBEServerLogTableMongodb::isShareDB: The database interface({}) is{} shared, uid={}! Check 'kbe_serverlog' table and 'kbengine[_defs].xml->dbmgr->shareDB'.\n",
							pdbi->name(), isOtherServerShareDB ? "" : " not", citer->first));

						return -1;
					}
				}
			}
		}
		catch (...)
		{
		}

		return isShareDB;
	}

	//-------------------------------------------------------------------------------------
	KBEServerLogTableMongodb::KBEServerLogTableMongodb(EntityTables* pEntityTables) :
		KBEServerLogTable(pEntityTables)
	{
	}

	bool KBEAccountTableMongodb::syncToDB(DBInterface* pdbi)
	{
		DBInterfaceMongodb* pdbiMongodb = mongoDBI(pdbi);
		pdbiMongodb->createCollection("kbe_accountinfos");

		const char* uniqueFields[] = { "accountName", "email", "entityDBID" };
		for (size_t i = 0; i < sizeof(uniqueFields) / sizeof(uniqueFields[0]); ++i)
		{
			bson_t keys;
			bson_t opts;
			bson_init(&keys);
			bson_init(&opts);
			BSON_APPEND_INT32(&keys, uniqueFields[i], 1);
			BSON_APPEND_BOOL(&opts, "unique", true);
			pdbiMongodb->collectionCreateIndex("kbe_accountinfos", &keys, &opts);
			bson_destroy(&opts);
			bson_destroy(&keys);
		}

		return true;
	}

	KBEAccountTableMongodb::KBEAccountTableMongodb(EntityTables* pEntityTables) :
		KBEAccountTable(pEntityTables)
	{
	}

	bool KBEAccountTableMongodb::setFlagsDeadline(DBInterface* pdbi, const std::string& name, uint32 flags, uint64 deadline)
	{
		bson_t query;
		bson_init(&query);
		bson_append_utf8(&query, "accountName", -1, name.c_str(), static_cast<int>(name.size()));

		bson_t values;
		bson_init(&values);
		BSON_APPEND_INT32(&values, "flags", flags);
		BSON_APPEND_INT64(&values, "deadline", deadline);

		bson_t update;
		bson_init(&update);
		BSON_APPEND_DOCUMENT(&update, "$set", &values);

		bool ret = mongoDBI(pdbi)->updateCollection("kbe_accountinfos", MONGOC_UPDATE_NONE, &query, &update, NULL);

		bson_destroy(&update);
		bson_destroy(&values);
		bson_destroy(&query);

		return ret;
	}

	bool KBEAccountTableMongodb::queryAccount(DBInterface* pdbi, const std::string& name, ACCOUNT_INFOS& info)
	{
		bson_t query;
		bson_init(&query);
		append_account_name_or_email_query(&query, name);

		const bson_t* doc = NULL;
		std::unique_ptr<MongoCursorGuard> guard;
		bool found = find_one(pdbi, "kbe_accountinfos", &query, &doc, guard);

		if (!found)
		{
			bson_destroy(&query);
			return false;
		}

		info.name = name;
		uint64 value64 = 0;
		int32 value32 = 0;
		get_utf8_field(doc, "password", info.password);
		if (get_int64_field(doc, "entityDBID", value64))
			info.dbid = value64;
		if (get_int32_field(doc, "flags", value32))
			info.flags = value32;
		if (get_int64_field(doc, "deadline", value64))
			info.deadline = value64;
		get_blob_field(doc, "bindata", info.datas);

		bson_destroy(&query);
		return info.dbid > 0;
	}

	bool KBEAccountTableMongodb::queryAccountAllInfos(DBInterface* pdbi, const std::string& name, ACCOUNT_INFOS& info)
	{
		bson_t query;
		bson_init(&query);
		append_account_name_or_email_query(&query, name);


		const bson_t* doc = NULL;
		std::unique_ptr<MongoCursorGuard> guard;
		bool found = find_one(pdbi, "kbe_accountinfos", &query, &doc, guard);

		if (!found)
		{
			bson_destroy(&query);
			return false;
		}

		info.name = name;
		uint64 value64 = 0;
		int32 value32 = 0;
		get_utf8_field(doc, "password", info.password);
		get_utf8_field(doc, "email", info.email);
		if (get_int64_field(doc, "entityDBID", value64))
			info.dbid = value64;
		if (get_int32_field(doc, "flags", value32))
			info.flags = value32;
		if (get_int64_field(doc, "deadline", value64))
			info.deadline = value64;

		bson_destroy(&query);
		return info.dbid > 0;
	}

	bool KBEAccountTableMongodb::updateCount(DBInterface* pdbi, const std::string& name, DBID dbid)
	{
		bson_t query;
		bson_init(&query);
		BSON_APPEND_INT64(&query, "entityDBID", dbid);

		bson_t setValues;
		bson_init(&setValues);
		BSON_APPEND_INT64(&setValues, "lasttime", time(NULL));

		bson_t incValues;
		bson_init(&incValues);
		BSON_APPEND_INT32(&incValues, "numlogin", 1);

		bson_t update;
		bson_init(&update);
		BSON_APPEND_DOCUMENT(&update, "$set", &setValues);
		BSON_APPEND_DOCUMENT(&update, "$inc", &incValues);

		bool ret = mongoDBI(pdbi)->updateCollection("kbe_accountinfos", MONGOC_UPDATE_NONE, &query, &update, NULL);

		bson_destroy(&update);
		bson_destroy(&incValues);
		bson_destroy(&setValues);
		bson_destroy(&query);

		return ret;
	}

	bool KBEAccountTableMongodb::updatePassword(DBInterface* pdbi, const std::string& name, const std::string& password)
	{
		bson_t doc;
		bson_init(&doc);
		bson_t child;
		bson_append_document_begin(&doc, "$set", -1, &child);
		bson_append_utf8(&child, "password", (int)strlen("password"), password.c_str(), static_cast<int>(password.size()));
		bson_append_document_end(&doc, &child);

		bson_t query;
		bson_init(&query);
		bson_append_utf8(&query, "accountName", -1, name.c_str(), static_cast<int>(name.size()));

		bool ret = mongoDBI(pdbi)->updateCollection("kbe_accountinfos", MONGOC_UPDATE_NONE, &query, &doc, NULL);

		bson_destroy(&query);
		bson_destroy(&doc);

		return ret;
	}


	bool KBEAccountTableMongodb::logAccount(DBInterface* pdbi, ACCOUNT_INFOS& info)
	{
		bson_t options;
		bson_init(&options);
		bson_append_utf8(&options, "accountName", (int)strlen("accountName"), info.name.c_str(), static_cast<int>(info.name.size()));
		std::string password = KBE_MD5::getDigest(info.password.data(), static_cast<int>(info.password.length()));
		bson_append_utf8(&options, "password", (int)strlen("password"), password.c_str(), static_cast<int>(password.size()));
		BSON_APPEND_BINARY(&options, "bindata", BSON_SUBTYPE_BINARY, reinterpret_cast<const uint8_t*>(info.datas.data()), static_cast<uint32_t>(info.datas.size()));
		bson_append_utf8(&options, "email", (int)strlen("email"), info.email.c_str(), static_cast<int>(info.email.size()));
		BSON_APPEND_INT64(&options, "entityDBID", info.dbid);
		BSON_APPEND_INT32(&options, "flags", info.flags);
		BSON_APPEND_INT64(&options, "deadline", info.deadline);
		BSON_APPEND_INT64(&options, "regtime", time(NULL));
		BSON_APPEND_INT64(&options, "lasttime", time(NULL));
		BSON_APPEND_INT32(&options, "numlogin", 0);

		bool ret = mongoDBI(pdbi)->insertCollection("kbe_accountinfos", MONGOC_INSERT_NONE, &options, NULL);

		bson_destroy(&options);

		return ret;
	}

	KBEEmailVerificationTableMongodb::KBEEmailVerificationTableMongodb(EntityTables* pEntityTables) :
		KBEEmailVerificationTable(pEntityTables)
	{

	}

	KBEEmailVerificationTableMongodb::~KBEEmailVerificationTableMongodb()
	{

	}

	bool KBEEmailVerificationTableMongodb::syncToDB(DBInterface* pdbi)
	{
		DBInterfaceMongodb* pdbiMongodb = mongoDBI(pdbi);
		pdbiMongodb->createCollection("kbe_email_verification");

		bson_t keys;
		bson_t opts;
		bson_init(&keys);
		bson_init(&opts);
		BSON_APPEND_INT32(&keys, "code", 1);
		BSON_APPEND_BOOL(&opts, "unique", true);
		pdbiMongodb->collectionCreateIndex("kbe_email_verification", &keys, &opts);
		bson_destroy(&opts);
		bson_destroy(&keys);

		struct ExpireRule
		{
			int type;
			uint64 deadline;
		};

		const ExpireRule rules[] = {
			{ KBEEmailVerificationTable::V_TYPE_CREATEACCOUNT, g_kbeSrvConfig.emailAtivationInfo_.deadline },
			{ KBEEmailVerificationTable::V_TYPE_RESETPASSWORD, g_kbeSrvConfig.emailResetPasswordInfo_.deadline },
			{ KBEEmailVerificationTable::V_TYPE_BIND_MAIL, g_kbeSrvConfig.emailBindInfo_.deadline }
		};

		for (size_t i = 0; i < sizeof(rules) / sizeof(rules[0]); ++i)
		{
			bson_t query;
			bson_t logtime;
			bson_init(&query);
			BSON_APPEND_INT32(&query, "type", rules[i].type);
			bson_append_document_begin(&query, "logtime", -1, &logtime);
			BSON_APPEND_INT64(&logtime, "$lt", time(NULL) - rules[i].deadline);
			bson_append_document_end(&query, &logtime);
			pdbiMongodb->collectionRemove("kbe_email_verification", MONGOC_REMOVE_NONE, &query, NULL);
			bson_destroy(&query);
		}

		return true;
	}

	bool KBEEmailVerificationTableMongodb::queryAccount(DBInterface* pdbi, int8 type, const std::string& name, ACCOUNT_INFOS& info)
	{
		std::string accountName;
		uint64 logtime = 0;
		if (!find_email_verification(pdbi, type, "accountName", name, accountName, info.password, logtime))
			return false;

		info.name = name;

		bson_t query;
		bson_init(&query);
		BSON_APPEND_INT32(&query, "type", type);
		bson_append_utf8(&query, "accountName", -1, name.c_str(), static_cast<int>(name.size()));

		const bson_t* doc = NULL;
		std::unique_ptr<MongoCursorGuard> guard;
		if (find_one(pdbi, "kbe_email_verification", &query, &doc, guard))
			get_utf8_field(doc, "code", info.datas);

		bson_destroy(&query);
		return info.datas.size() > 0;
	}

	bool KBEEmailVerificationTableMongodb::logAccount(DBInterface* pdbi, int8 type, const std::string& name, const std::string& datas, const std::string& code)
	{
		bson_t doc;
		bson_init(&doc);
		bson_append_utf8(&doc, "accountName", -1, name.c_str(), static_cast<int>(name.size()));
		BSON_APPEND_INT32(&doc, "type", type);
		bson_append_utf8(&doc, "datas", -1, datas.c_str(), static_cast<int>(datas.size()));
		bson_append_utf8(&doc, "code", -1, code.c_str(), static_cast<int>(code.size()));
		BSON_APPEND_INT64(&doc, "logtime", time(NULL));

		bool ret = mongoDBI(pdbi)->insertCollection("kbe_email_verification", MONGOC_INSERT_NONE, &doc, NULL);

		bson_destroy(&doc);
		return ret;
	}

	bool KBEEmailVerificationTableMongodb::delAccount(DBInterface* pdbi, int8 type, const std::string& name)
	{
		bson_t query;
		bson_init(&query);
		BSON_APPEND_INT32(&query, "type", type);
		bson_append_utf8(&query, "accountName", -1, name.c_str(), static_cast<int>(name.size()));

		bool ret = mongoDBI(pdbi)->collectionRemove("kbe_email_verification", MONGOC_REMOVE_NONE, &query, NULL);
		bson_destroy(&query);
		return ret;
	}

	bool KBEEmailVerificationTableMongodb::activateAccount(DBInterface* pdbi, const std::string& code, ACCOUNT_INFOS& info)
	{
		std::string datas;
		uint64 logtime = 1;
		if (!find_email_verification(pdbi, (int8)KBEEmailVerificationTable::V_TYPE_CREATEACCOUNT, "code", code, info.name, datas, logtime))
		{
			ERROR_MSG(fmt::format("KBEEmailVerificationTableMongodb::activateAccount({}): code is invalid.\n", code));
			return false;
		}

		info.password = datas;

		if (logtime > 0 && time(NULL) - logtime > g_kbeSrvConfig.emailAtivationInfo_.deadline)
		{
			ERROR_MSG(fmt::format("KBEEmailVerificationTableMongodb::activateAccount({}): is expired! {} > {}.\n",
				code, (time(NULL) - logtime), g_kbeSrvConfig.emailAtivationInfo_.deadline));
			return false;
		}

		if (info.name.empty())
		{
			ERROR_MSG(fmt::format("KBEEmailVerificationTableMongodb::activateAccount({}): name is NULL.\n", code));
			return false;
		}

		std::string password = info.password;

		KBEAccountTable* pTable = static_cast<KBEAccountTable*>(EntityTables::findByInterfaceName(pdbi->name()).findKBETable(KBE_TABLE_PERFIX "_accountinfos"));
		KBE_ASSERT(pTable);

		info.flags = 0;
		if (!pTable->queryAccount(pdbi, info.name, info))
			return false;

		if ((info.flags & ACCOUNT_FLAG_NOT_ACTIVATED) <= 0)
		{
			ERROR_MSG(fmt::format("KBEEmailVerificationTableMongodb::activateAccount({}): Has been activated, flags={}.\n", code, info.flags));
			return false;
		}

		info.flags &= ~ACCOUNT_FLAG_NOT_ACTIVATED;

		if (!pTable->setFlagsDeadline(pdbi, info.name, info.flags, info.deadline))
			return false;

		if (!pTable->updatePassword(pdbi, info.name, password))
			return false;

		info.dbid = 0;

		ScriptDefModule* pModule = EntityDef::findScriptModule(DBUtil::accountScriptName());
		MemoryStream copyAccountDefMemoryStream(pTable->accountDefMemoryStream());

		info.dbid = EntityTables::findByInterfaceName(pdbi->name()).writeEntity(pdbi, 0, -1,
			&copyAccountDefMemoryStream, pModule);

		KBE_ASSERT(info.dbid > 0);

		bson_t query;
		bson_init(&query);
		bson_append_utf8(&query, "accountName", -1, info.name.c_str(), static_cast<int>(info.name.size()));

		bson_t values;
		bson_init(&values);
		BSON_APPEND_INT64(&values, "entityDBID", info.dbid);

		bson_t update;
		bson_init(&update);
		BSON_APPEND_DOCUMENT(&update, "$set", &values);

		bool ret = mongoDBI(pdbi)->updateCollection("kbe_accountinfos", MONGOC_UPDATE_NONE, &query, &update, NULL);

		bson_destroy(&update);
		bson_destroy(&values);
		bson_destroy(&query);

		if (!ret)
			return false;

		try
		{
			delAccount(pdbi, (int8)V_TYPE_CREATEACCOUNT, info.name);
		}
		catch (...)
		{
		}

		return true;
	}

	bool KBEEmailVerificationTableMongodb::bindEMail(DBInterface* pdbi, const std::string& name, const std::string& code)
	{
		std::string qname;
		std::string qemail;
		uint64 logtime = 1;
		if (!find_email_verification(pdbi, (int8)KBEEmailVerificationTable::V_TYPE_BIND_MAIL, "code", code, qname, qemail, logtime))
			return false;

		if (logtime > 0 && time(NULL) - logtime > g_kbeSrvConfig.emailBindInfo_.deadline)
		{
			ERROR_MSG(fmt::format("KBEEmailVerificationTableMongodb::bindEMail({}): is expired! {} > {}.\n",
				code, (time(NULL) - logtime), g_kbeSrvConfig.emailBindInfo_.deadline));
			return false;
		}

		if (qname.empty() || qemail.empty())
			return false;

		if (qemail != name)
		{
			WARNING_MSG(fmt::format("KBEEmailVerificationTableMongodb::bindEMail: code({}) username({}:{}, {}) not match.\n",
				code, name, qname, qemail));
			return false;
		}

		bson_t query;
		bson_init(&query);
		bson_append_utf8(&query, "accountName", -1, qname.c_str(), static_cast<int>(qname.size()));

		bson_t values;
		bson_init(&values);
		bson_append_utf8(&values, "email", -1, qemail.c_str(), static_cast<int>(qemail.size()));

		bson_t update;
		bson_init(&update);
		BSON_APPEND_DOCUMENT(&update, "$set", &values);

		bool ret = mongoDBI(pdbi)->updateCollection("kbe_accountinfos", MONGOC_UPDATE_NONE, &query, &update, NULL);

		bson_destroy(&update);
		bson_destroy(&values);
		bson_destroy(&query);

		if (!ret)
			return false;

		try
		{
			delAccount(pdbi, (int8)V_TYPE_BIND_MAIL, name);
		}
		catch (...)
		{
		}

		return true;
	}

	bool KBEEmailVerificationTableMongodb::resetpassword(DBInterface* pdbi, const std::string& name, const std::string& password, const std::string& code)
	{
		std::string qname;
		std::string qemail;
		uint64 logtime = 1;
		if (!find_email_verification(pdbi, (int8)KBEEmailVerificationTable::V_TYPE_RESETPASSWORD, "code", code, qname, qemail, logtime))
			return false;

		if (logtime > 0 && time(NULL) - logtime > g_kbeSrvConfig.emailResetPasswordInfo_.deadline)
		{
			ERROR_MSG(fmt::format("KBEEmailVerificationTableMongodb::resetpassword({}): is expired! {} > {}.\n",
				code, (time(NULL) - logtime), g_kbeSrvConfig.emailResetPasswordInfo_.deadline));
			return false;
		}

		if (qname.empty() || password.empty())
			return false;

		if (qname != name)
		{
			WARNING_MSG(fmt::format("KBEEmailVerificationTableMongodb::resetpassword: code({}) username({} != {}) not match.\n",
				code, name, qname));
			return false;
		}

		KBEAccountTable* pTable = static_cast<KBEAccountTable*>(EntityTables::findByInterfaceName(pdbi->name()).findKBETable(KBE_TABLE_PERFIX "_accountinfos"));
		KBE_ASSERT(pTable);

		if (!pTable->updatePassword(pdbi, qname, KBE_MD5::getDigest(password.data(), password.length())))
			return false;

		try
		{
			delAccount(pdbi, (int8)V_TYPE_RESETPASSWORD, qname);
		}
		catch (...)
		{
		}

		return true;
	}
}
