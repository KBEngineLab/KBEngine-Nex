// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved.

#include "kbe_table_postgresql.h"
#include "common.h"
#include "db_interface_postgresql.h"
#include "db_interface/db_interface.h"
#include "entitydef/entitydef.h"
#include "openssl/md5.h"
#include "server/serverconfig.h"

#include <algorithm>

namespace KBEngine {
using postgresql::esc;
using postgresql::hexEncode;
using postgresql::pg;

namespace
{
PGresult* execSystemTableQuery(DBInterface* pdbi, const std::string& sql, const char* caller)
{
	return pg(pdbi)->queryResult(sql, caller);
}

// 系统表里还有少量直接读 PGresult 的查询，这里统一把失败 SQL 打清楚。
bool checkSystemTableTuples(PGresult* result, const std::string& sql, const char* caller)
{
	if (PQresultStatus(result) == PGRES_TUPLES_OK)
		return true;

	ERROR_MSG(fmt::format("{}: query failed, error={}, sql={}\n",
		caller, PQresultErrorMessage(result), sql));
	return false;
}

// kbe_entitylog 启动清理会读取 kbe_serverlog；系统表是并行同步的，先把依赖表兜住。
bool ensureServerLogTableForEntityLog(DBInterface* pdbi)
{
	return pdbi->query(
		"CREATE TABLE IF NOT EXISTS kbe_serverlog ("
		"heartbeatTime BIGINT NOT NULL DEFAULT 0, "
		"isShareDB SMALLINT NOT NULL DEFAULT 0, "
		"serverGroupID BIGINT PRIMARY KEY"
		")")
		&& pdbi->query("ALTER TABLE kbe_serverlog DROP COLUMN IF EXISTS componentID")
		&& pdbi->query("ALTER TABLE kbe_serverlog ADD COLUMN IF NOT EXISTS heartbeatTime BIGINT NOT NULL DEFAULT 0")
		&& pdbi->query("ALTER TABLE kbe_serverlog ADD COLUMN IF NOT EXISTS isShareDB SMALLINT NOT NULL DEFAULT 0")
		&& pdbi->query("ALTER TABLE kbe_serverlog ADD COLUMN IF NOT EXISTS serverGroupID BIGINT NOT NULL DEFAULT 0")
		&& pdbi->query("DELETE FROM kbe_serverlog WHERE serverGroupID=0");
}

// 查询邮件验证码表中的单条记录。
bool queryVerification(DBInterface* pdbi, int8 type, const std::string& code,
	std::string& name, std::string& datas, uint64& logtime)
{
	std::string sql = fmt::format(
		"SELECT name, datas, logtime FROM kbe_email_verification WHERE code='{}' AND type={} LIMIT 1",
		esc(pdbi, code), (int)type);

	PGresult* result = execSystemTableQuery(pdbi, sql, "queryVerification");
	if (result == NULL)
		return false;

	bool ok = PQresultStatus(result) == PGRES_TUPLES_OK && PQntuples(result) > 0;
	if (ok)
	{
		name = PQgetvalue(result, 0, 0);
		datas = PQgetvalue(result, 0, 1);
		KBEngine::StringConv::str2value(logtime, PQgetvalue(result, 0, 2));
	}

	PQclear(result);
	return ok;
}

// 判断验证码是否已过期。
bool isVerificationExpired(uint64 logtime, uint64 deadline)
{
	uint64 now = (uint64)time(NULL);
	return logtime > 0 && now > logtime && now - logtime > deadline;
}

std::string byteaSqlValue(const std::string& data)
{
	return fmt::format("decode('{}', 'hex')", hexEncode(data.data(), data.size()));
}

bool assignByteaValue(PGresult* result, int row, int column, std::string& out)
{
	if (PQgetisnull(result, row, column))
	{
		out.clear();
		return true;
	}

	size_t size = 0;
	unsigned char* data = PQunescapeBytea(reinterpret_cast<const unsigned char*>(PQgetvalue(result, row, column)), &size);
	if (data == NULL)
		return false;

	out.assign(reinterpret_cast<const char*>(data), size);
	PQfreemem(data);
	return true;
}
}

// 绑定系统表集合，表结构同步时使用 PostgreSQL SQL。
KBEEntityLogTablePostgresql::KBEEntityLogTablePostgresql(EntityTables* pEntityTables) :
	KBEEntityLogTable(pEntityTables)
{
}

// 同步 kbe_entitylog 表，记录实体当前所在 baseapp。
bool KBEEntityLogTablePostgresql::syncToDB(DBInterface* pdbi)
{
	// 记录实体当前所在的 baseapp。启动时会顺手清掉失效 serverGroup 留下来的在线记录，
	// 避免上次异常退出后，后续登录还被旧记录挡住。
	KBEServerLogTablePostgresql serverLogTable(NULL);
	if (!ensureServerLogTableForEntityLog(pdbi))
		return false;

	int ret = serverLogTable.isShareDB(pdbi);
	if (ret == -1)
		return false;

	// 非 shareDB 模式下沿用 MySQL 后端的行为，启动时先清掉历史在线记录表。
	if (ret == 0 && !pdbi->query("DROP TABLE IF EXISTS kbe_entitylog"))
		return false;

	if (!pdbi->query(
		"CREATE TABLE IF NOT EXISTS kbe_entitylog ("
		"dbid BIGINT NOT NULL, "
		"entityType INTEGER NOT NULL, "
		"entityID BIGINT NOT NULL, "
		"ip VARCHAR(64) NOT NULL, "
		"port INTEGER NOT NULL, "
		"componentID BIGINT NOT NULL, "
		"serverGroupID BIGINT NOT NULL, "
		"PRIMARY KEY(dbid, entityType)"
		")"))
		return false;

	std::vector<COMPONENT_ID> cids = serverLogTable.queryTimeOutServers(pdbi);
	if (!serverLogTable.clearServers(pdbi, cids))
		return false;

	cids.push_back((uint64)getUserUID());
	for (size_t i = 0; i < cids.size(); ++i)
	{
		if (!pdbi->query(fmt::format("DELETE FROM kbe_entitylog WHERE serverGroupID={}", cids[i])))
			return false;
	}

	std::vector<COMPONENT_ID> servers = serverLogTable.queryServers(pdbi);
	PGresult* result = execSystemTableQuery(pdbi, "SELECT DISTINCT serverGroupID FROM kbe_entitylog",
		"KBEEntityLogTablePostgresql::syncToDB");
	if (result == NULL)
		return false;

	if (!checkSystemTableTuples(result, "SELECT DISTINCT serverGroupID FROM kbe_entitylog",
		"KBEEntityLogTablePostgresql::syncToDB"))
	{
		PQclear(result);
		return false;
	}

	for (int i = 0; i < PQntuples(result); ++i)
	{
		COMPONENT_ID cid = 0;
		KBEngine::StringConv::str2value(cid, PQgetvalue(result, i, 0));
		if (std::find(servers.begin(), servers.end(), cid) == servers.end())
		{
			if (!pdbi->query(fmt::format("DELETE FROM kbe_entitylog WHERE serverGroupID={}", cid)))
			{
				PQclear(result);
				return false;
			}
		}
	}

	PQclear(result);
	return true;
}

// 写入或更新实体在线记录。
bool KBEEntityLogTablePostgresql::logEntity(DBInterface* pdbi, const char* ip, uint32 port, DBID dbid,
	COMPONENT_ID componentID, ENTITY_ID entityID, ENTITY_SCRIPT_UID entityType)
{
	// 和 MySQL 一样直接插入；主键冲突表示同一个实体已经有在线记录，调用方会按登录冲突处理。
	std::string sql = fmt::format(
		"INSERT INTO kbe_entitylog(dbid, entityType, entityID, ip, port, componentID, serverGroupID) "
		"VALUES({}, {}, {}, '{}', {}, {}, {})",
		dbid, entityType, entityID, pg(pdbi)->escapeString(ip, strlen(ip)), port, componentID, (uint64)getUserUID());
	return pdbi->query(sql);
}

// 按 dbid 和实体类型查询实体在线记录。
bool KBEEntityLogTablePostgresql::queryEntity(DBInterface* pdbi, DBID dbid, EntityLog& entitylog, ENTITY_SCRIPT_UID entityType)
{
	std::string sql = fmt::format(
		"SELECT entityID, ip, port, componentID, serverGroupID FROM kbe_entitylog WHERE dbid={} AND entityType={}",
		dbid, entityType);
	PGresult* result = execSystemTableQuery(pdbi, sql, "KBEEntityLogTablePostgresql::queryEntity");
	if (result == NULL)
		return false;

	bool ok = PQresultStatus(result) == PGRES_TUPLES_OK && PQntuples(result) > 0;
	if (ok)
	{
		entitylog.dbid = dbid;
		KBEngine::StringConv::str2value(entitylog.entityID, PQgetvalue(result, 0, 0));
		kbe_snprintf(entitylog.ip, MAX_IP, "%s", PQgetvalue(result, 0, 1));
		KBEngine::StringConv::str2value(entitylog.port, PQgetvalue(result, 0, 2));
		KBEngine::StringConv::str2value(entitylog.componentID, PQgetvalue(result, 0, 3));
		KBEngine::StringConv::str2value(entitylog.serverGroupID, PQgetvalue(result, 0, 4));
	}
	PQclear(result);
	return ok;
}

// 删除指定实体的在线记录。
bool KBEEntityLogTablePostgresql::eraseEntityLog(DBInterface* pdbi, DBID dbid, ENTITY_SCRIPT_UID entityType)
{
	return pdbi->query(fmt::format("DELETE FROM kbe_entitylog WHERE dbid={} AND entityType={}", dbid, entityType));
}

// 删除指定 baseapp 组件上的全部实体在线记录。
bool KBEEntityLogTablePostgresql::eraseBaseappEntityLog(DBInterface* pdbi, COMPONENT_ID componentID)
{
	return pdbi->query(fmt::format("DELETE FROM kbe_entitylog WHERE componentID={}", componentID));
}

// 绑定系统表集合，维护服务器心跳表。
KBEServerLogTablePostgresql::KBEServerLogTablePostgresql(EntityTables* pEntityTables) :
	KBEServerLogTable(pEntityTables)
{
}

// 同步 kbe_serverlog 表，保存 dbmgr 看到的服务器心跳。
bool KBEServerLogTablePostgresql::syncToDB(DBInterface* pdbi)
{
	// 这里存的是当前 dbmgr 进程组的心跳。KBE 的清理逻辑按 serverGroupID 归属实体在线记录，
	// PostgreSQL 这边也保持同一套口径。
	if (!pdbi->query(
		"CREATE TABLE IF NOT EXISTS kbe_serverlog ("
		"heartbeatTime BIGINT NOT NULL, "
		"isShareDB SMALLINT NOT NULL DEFAULT 0, "
		"serverGroupID BIGINT PRIMARY KEY"
		")"))
		return false;

	// 早期 PostgreSQL 实现用过 componentID 做主键，本表真正参与清理的是 serverGroupID。
	// 这里把开发库里的旧列移走，并确保主键落在 serverGroupID 上。
	if (!pdbi->query("ALTER TABLE kbe_serverlog DROP COLUMN IF EXISTS componentID"))
		return false;

	if (!pdbi->query("ALTER TABLE kbe_serverlog ADD COLUMN IF NOT EXISTS heartbeatTime BIGINT NOT NULL DEFAULT 0")
		|| !pdbi->query("ALTER TABLE kbe_serverlog ADD COLUMN IF NOT EXISTS isShareDB SMALLINT NOT NULL DEFAULT 0")
		|| !pdbi->query("ALTER TABLE kbe_serverlog ADD COLUMN IF NOT EXISTS serverGroupID BIGINT NOT NULL DEFAULT 0"))
		return false;

	if (!pdbi->query("DELETE FROM kbe_serverlog WHERE serverGroupID=0"))
		return false;

	if (!pdbi->query(
		"DELETE FROM kbe_serverlog a USING kbe_serverlog b "
		"WHERE a.serverGroupID=b.serverGroupID "
		"AND (a.heartbeatTime<b.heartbeatTime OR (a.heartbeatTime=b.heartbeatTime AND a.ctid<b.ctid))"))
		return false;

	if (!pdbi->query(
		"DO $$ BEGIN "
		"IF NOT EXISTS (SELECT 1 FROM pg_constraint WHERE conrelid = 'kbe_serverlog'::regclass AND contype = 'p') THEN "
		"ALTER TABLE kbe_serverlog ADD PRIMARY KEY(serverGroupID); "
		"END IF; "
		"END $$;"))
		return false;

	return updateServer(pdbi);
}

// 刷新当前组件的心跳和 shareDB 状态。
bool KBEServerLogTablePostgresql::updateServer(DBInterface* pdbi)
{
	int ret = isShareDB(pdbi);
	if (ret == -1)
		return false;

	uint64 now = timestamp();
	std::string sql = fmt::format(
		"INSERT INTO kbe_serverlog(heartbeatTime, isShareDB, serverGroupID) "
		"VALUES({}, {}, {}) "
		"ON CONFLICT(serverGroupID) DO UPDATE SET heartbeatTime=EXCLUDED.heartbeatTime, "
		"isShareDB=EXCLUDED.isShareDB",
		now, ret, (uint64)getUserUID());
	return pdbi->query(sql);
}

// 查询当前 serverGroup 的心跳记录。
bool KBEServerLogTablePostgresql::queryServer(DBInterface* pdbi, ServerLog& serverlog)
{
	std::string sql = fmt::format("SELECT heartbeatTime, serverGroupID, isShareDB FROM kbe_serverlog WHERE serverGroupID={}", (uint64)getUserUID());
	PGresult* result = execSystemTableQuery(pdbi, sql, "KBEServerLogTablePostgresql::queryServer");
	if (result == NULL)
		return false;

	bool ok = PQresultStatus(result) == PGRES_TUPLES_OK && PQntuples(result) > 0;
	if (ok)
	{
		KBEngine::StringConv::str2value(serverlog.heartbeatTime, PQgetvalue(result, 0, 0));
		KBEngine::StringConv::str2value(serverlog.serverGroupID, PQgetvalue(result, 0, 1));
		int share = 0;
		KBEngine::StringConv::str2value(share, PQgetvalue(result, 0, 2));
		serverlog.isShareDB = (uint8)share;
	}
	PQclear(result);
	return ok;
}

// 查询所有仍记录在表里的 serverGroupID。
std::vector<COMPONENT_ID> KBEServerLogTablePostgresql::queryServers(DBInterface* pdbi)
{
	std::vector<COMPONENT_ID> cids;
	std::string sql = "SELECT serverGroupID FROM kbe_serverlog";
	PGresult* result = execSystemTableQuery(pdbi, sql,
		"KBEServerLogTablePostgresql::queryServers");
	if (result == NULL)
		return cids;

	if (checkSystemTableTuples(result, sql, "KBEServerLogTablePostgresql::queryServers"))
	{
		for (int i = 0; i < PQntuples(result); ++i)
		{
			COMPONENT_ID cid = 0;
			KBEngine::StringConv::str2value(cid, PQgetvalue(result, i, 0));
			cids.push_back(cid);
		}
	}
	PQclear(result);
	return cids;
}

// 查询心跳超时的 serverGroupID，当前进程组不能被自己清掉。
std::vector<COMPONENT_ID> KBEServerLogTablePostgresql::queryTimeOutServers(DBInterface* pdbi)
{
	std::vector<COMPONENT_ID> cids;
	std::string sql = "SELECT heartbeatTime, serverGroupID FROM kbe_serverlog";
	PGresult* result = execSystemTableQuery(pdbi, sql,
		"KBEServerLogTablePostgresql::queryTimeOutServers");
	if (result == NULL)
		return cids;

	if (checkSystemTableTuples(result, sql, "KBEServerLogTablePostgresql::queryTimeOutServers"))
	{
		for (int i = 0; i < PQntuples(result); ++i)
		{
			uint64 heartbeatTime = 0;
			COMPONENT_ID cid = 0;
			KBEngine::StringConv::str2value(heartbeatTime, PQgetvalue(result, i, 0));
			KBEngine::StringConv::str2value(cid, PQgetvalue(result, i, 1));

			if (cid == (uint64)getUserUID())
				continue;

			if ((uint64)time(NULL) > heartbeatTime + KBEServerLogTable::TIMEOUT * 2)
				cids.push_back(cid);
		}
	}
	PQclear(result);
	return cids;
}

// 清理指定 serverGroup 的心跳记录。
bool KBEServerLogTablePostgresql::clearServers(DBInterface* pdbi, const std::vector<COMPONENT_ID>& cids)
{
	for (size_t i = 0; i < cids.size(); ++i)
	{
		if (cids[i] == (uint64)getUserUID())
			continue;

		if (!pdbi->query(fmt::format("DELETE FROM kbe_serverlog WHERE serverGroupID={}", cids[i])))
			return false;
	}
	return true;
}

// 查询所有 serverGroup 的 shareDB 状态，用来阻止共享和非共享配置混用。
std::map<COMPONENT_ID, bool> KBEServerLogTablePostgresql::queryAllServerShareDBState(DBInterface* pdbi)
{
	std::vector<COMPONENT_ID> cids = queryTimeOutServers(pdbi);
	clearServers(pdbi, cids);

	std::map<COMPONENT_ID, bool> values;
	std::string sql = "SELECT serverGroupID, isShareDB FROM kbe_serverlog";
	PGresult* result = execSystemTableQuery(pdbi, sql,
		"KBEServerLogTablePostgresql::queryAllServerShareDBState");
	if (result == NULL)
		return values;

	if (checkSystemTableTuples(result, sql, "KBEServerLogTablePostgresql::queryAllServerShareDBState"))
	{
		for (int i = 0; i < PQntuples(result); ++i)
		{
			COMPONENT_ID cid = 0;
			int share = 0;
			KBEngine::StringConv::str2value(cid, PQgetvalue(result, i, 0));
			KBEngine::StringConv::str2value(share, PQgetvalue(result, i, 1));
			values[cid] = share != 0;
		}
	}
	PQclear(result);
	return values;
}

// 当前进程组的 shareDB 配置必须和库里其他存活进程组兼容。
int KBEServerLogTablePostgresql::isShareDB(DBInterface* pdbi)
{
	bool isShareDB = g_kbeSrvConfig.getDBMgr().isShareDB;
	uint64 uid = getUserUID();

	try
	{
		std::map<COMPONENT_ID, bool> cidMap = queryAllServerShareDBState(pdbi);
		std::map<COMPONENT_ID, bool>::const_iterator iter = cidMap.begin();
		for (; iter != cidMap.end(); ++iter)
		{
			if (iter->first != uid)
			{
				bool isOtherServerShareDB = iter->second;
				if (!isOtherServerShareDB || (isOtherServerShareDB && !isShareDB))
				{
					ERROR_MSG(fmt::format("KBEServerLogTablePostgresql::isShareDB: database interface({}) is{} shared, uid={}. Check 'kbe_serverlog' and 'kbengine[_defs].xml->dbmgr->shareDB'.\n",
						pdbi->name(), isOtherServerShareDB ? "" : " not", iter->first));

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

// 绑定系统表集合，维护账号信息表。
KBEAccountTablePostgresql::KBEAccountTablePostgresql(EntityTables* pEntityTables) :
	KBEAccountTable(pEntityTables)
{
}

// 同步 kbe_accountinfos 表，保存账号到实体 DBID 的映射。
bool KBEAccountTablePostgresql::syncToDB(DBInterface* pdbi)
{
	/*
		账号表保留 KBE 的现有字段语义：name 对应 MySQL 的 accountName，
		bindata 对应 MySQL 的 bindata/blob，不能用 TEXT 顶替，否则二进制内容会被编码规则影响。
		MySQL 版本对 email 和 entityDBID 都有唯一键，
		PostgreSQL 这里也补上同等约束，避免同一邮箱或同一实体 DBID 被多条账号记录占用。
		dbid=0 是“还没有绑定实体”的占位值，不能建普通唯一索引，否则多个未完成账号会互相冲突。
	*/
	if (!pdbi->query(
		"CREATE TABLE IF NOT EXISTS kbe_accountinfos ("
		"name VARCHAR(255) PRIMARY KEY, "
		"password VARCHAR(255) NOT NULL, "
		"bindata BYTEA, "
		"email VARCHAR(255) NOT NULL DEFAULT '', "
		"dbid BIGINT NOT NULL DEFAULT 0, "
		"flags BIGINT NOT NULL DEFAULT 0, "
		"deadline BIGINT NOT NULL DEFAULT 0, "
		"regtime BIGINT NOT NULL DEFAULT 0, "
		"lasttime BIGINT NOT NULL DEFAULT 0, "
		"numlogin BIGINT NOT NULL DEFAULT 0"
		")"))
		return false;

	return pdbi->query("ALTER TABLE kbe_accountinfos ADD COLUMN IF NOT EXISTS regtime BIGINT NOT NULL DEFAULT 0")
		&& pdbi->query("ALTER TABLE kbe_accountinfos ADD COLUMN IF NOT EXISTS lasttime BIGINT NOT NULL DEFAULT 0")
		&& pdbi->query("ALTER TABLE kbe_accountinfos ADD COLUMN IF NOT EXISTS numlogin BIGINT NOT NULL DEFAULT 0")
		&& pdbi->query("ALTER TABLE kbe_accountinfos ADD COLUMN IF NOT EXISTS bindata BYTEA")
		&& pdbi->query(
			"DO $$ "
			"BEGIN "
			"IF EXISTS (SELECT 1 FROM information_schema.columns WHERE table_schema='public' AND table_name='kbe_accountinfos' AND column_name='datas') THEN "
			"UPDATE kbe_accountinfos SET bindata=convert_to(datas, 'UTF8') WHERE bindata IS NULL; "
			"ALTER TABLE kbe_accountinfos DROP COLUMN datas; "
			"END IF; "
			"END $$")
		&& pdbi->query("CREATE UNIQUE INDEX IF NOT EXISTS uk_kbe_accountinfos_email ON kbe_accountinfos(email) WHERE email<>''")
		&& pdbi->query("CREATE UNIQUE INDEX IF NOT EXISTS uk_kbe_accountinfos_dbid ON kbe_accountinfos(dbid) WHERE dbid<>0");
}

// 查询账号基础信息。
bool KBEAccountTablePostgresql::queryAccount(DBInterface* pdbi, const std::string& name, ACCOUNT_INFOS& info)
{
	return queryAccountAllInfos(pdbi, name, info);
}

// 查询账号完整信息。
bool KBEAccountTablePostgresql::queryAccountAllInfos(DBInterface* pdbi, const std::string& name, ACCOUNT_INFOS& info)
{
	std::string qname = esc(pdbi, name);
	std::string sql = fmt::format(
		"SELECT name, password, bindata, email, dbid, flags, deadline FROM kbe_accountinfos "
		"WHERE name='{}' OR email='{}' LIMIT 1",
		qname, qname);
	PGresult* result = execSystemTableQuery(pdbi, sql, "KBEAccountTablePostgresql::queryAccountAllInfos");
	if (result == NULL)
		return false;

	bool ok = PQresultStatus(result) == PGRES_TUPLES_OK && PQntuples(result) > 0;
	if (ok)
	{
		info.name = PQgetvalue(result, 0, 0);
		info.password = PQgetvalue(result, 0, 1);
		if (!assignByteaValue(result, 0, 2, info.datas))
		{
			ERROR_MSG("KBEAccountTablePostgresql::queryAccountAllInfos: bindata bytea decode failed.\n");
			PQclear(result);
			return false;
		}

		info.email = PQgetvalue(result, 0, 3);
		KBEngine::StringConv::str2value(info.dbid, PQgetvalue(result, 0, 4));
		KBEngine::StringConv::str2value(info.flags, PQgetvalue(result, 0, 5));
		KBEngine::StringConv::str2value(info.deadline, PQgetvalue(result, 0, 6));
	}
	PQclear(result);
	return ok && info.dbid > 0;
}

// 创建或更新账号记录。
bool KBEAccountTablePostgresql::logAccount(DBInterface* pdbi, ACCOUNT_INFOS& info)
{
	uint64 now = (uint64)time(NULL);
	std::string password = KBE_MD5::getDigest(info.password.data(), (int)info.password.length());
	std::string sql = fmt::format(
		"INSERT INTO kbe_accountinfos(name, password, bindata, email, dbid, flags, deadline, regtime, lasttime, numlogin) "
		"VALUES('{}', '{}', {}, '{}', {}, {}, {}, {}, {}, 0) "
		"ON CONFLICT(name) DO UPDATE SET password=EXCLUDED.password, bindata=EXCLUDED.bindata, "
		"email=EXCLUDED.email, dbid=EXCLUDED.dbid, flags=EXCLUDED.flags, deadline=EXCLUDED.deadline, lasttime=EXCLUDED.lasttime",
		esc(pdbi, info.name), esc(pdbi, password), byteaSqlValue(info.datas), esc(pdbi, info.email),
		info.dbid, info.flags, info.deadline, now, now);
	return pdbi->query(sql);
}

// 更新账号标志位和截止时间。
bool KBEAccountTablePostgresql::setFlagsDeadline(DBInterface* pdbi, const std::string& name, uint32 flags, uint64 deadline)
{
	return pdbi->query(fmt::format("UPDATE kbe_accountinfos SET flags={}, deadline={} WHERE name='{}'",
		flags, deadline, esc(pdbi, name)));
}

// 更新账号关联的实体 DBID。
bool KBEAccountTablePostgresql::updateCount(DBInterface* pdbi, const std::string& name, DBID dbid)
{
	return pdbi->query(fmt::format("UPDATE kbe_accountinfos SET dbid={}, lasttime={}, numlogin=numlogin+1 WHERE name='{}' OR dbid={}",
		dbid, (uint64)time(NULL), esc(pdbi, name), dbid));
}

// 更新账号密码摘要。
bool KBEAccountTablePostgresql::updatePassword(DBInterface* pdbi, const std::string& name, const std::string& password)
{
	return pdbi->query(fmt::format("UPDATE kbe_accountinfos SET password='{}' WHERE name='{}'",
		esc(pdbi, password), esc(pdbi, name)));
}

// 绑定系统表集合，维护邮件验证码表。
KBEEmailVerificationTablePostgresql::KBEEmailVerificationTablePostgresql(EntityTables* pEntityTables) :
	KBEEmailVerificationTable(pEntityTables)
{
}

// 同步 kbe_email_verification 表，保存邮件验证码流程状态。
bool KBEEmailVerificationTablePostgresql::syncToDB(DBInterface* pdbi)
{
	// 邮件验证码表按 code 查询和清理。
	if (!pdbi->query(
		"CREATE TABLE IF NOT EXISTS kbe_email_verification ("
		"code VARCHAR(255) PRIMARY KEY, "
		"type SMALLINT NOT NULL, "
		"name VARCHAR(255) NOT NULL, "
		"datas TEXT NOT NULL DEFAULT '', "
		"logtime BIGINT NOT NULL DEFAULT 0"
		")"))
		return false;

	if (!pdbi->query("ALTER TABLE kbe_email_verification ADD COLUMN IF NOT EXISTS logtime BIGINT NOT NULL DEFAULT 0"))
		return false;

	uint64 now = (uint64)time(NULL);
	uint64 createDeadline = g_kbeSrvConfig.emailAtivationInfo_.deadline;
	uint64 resetDeadline = g_kbeSrvConfig.emailResetPasswordInfo_.deadline;
	uint64 bindDeadline = g_kbeSrvConfig.emailBindInfo_.deadline;

	return pdbi->query(fmt::format("DELETE FROM kbe_email_verification WHERE type={} AND logtime<{}",
		(int)V_TYPE_CREATEACCOUNT, now > createDeadline ? now - createDeadline : 0))
		&& pdbi->query(fmt::format("DELETE FROM kbe_email_verification WHERE type={} AND logtime<{}",
			(int)V_TYPE_RESETPASSWORD, now > resetDeadline ? now - resetDeadline : 0))
		&& pdbi->query(fmt::format("DELETE FROM kbe_email_verification WHERE type={} AND logtime<{}",
			(int)V_TYPE_BIND_MAIL, now > bindDeadline ? now - bindDeadline : 0));
}

// 查询邮件验证码对应的账号信息。
bool KBEEmailVerificationTablePostgresql::queryAccount(DBInterface* pdbi, int8 type, const std::string& name, ACCOUNT_INFOS& info)
{
	std::string sql = fmt::format(
		"SELECT code, datas FROM kbe_email_verification WHERE name='{}' AND type={} LIMIT 1",
		esc(pdbi, name), (int)type);

	PGresult* result = execSystemTableQuery(pdbi, sql, "KBEEmailVerificationTablePostgresql::queryAccount");
	if (result == NULL)
		return false;

	bool ok = PQresultStatus(result) == PGRES_TUPLES_OK && PQntuples(result) > 0;
	if (ok)
	{
		info.name = name;
		info.datas = PQgetvalue(result, 0, 0);
		info.password = PQgetvalue(result, 0, 1);
	}

	PQclear(result);
	return ok;
}

// 写入或刷新邮件验证码记录。
bool KBEEmailVerificationTablePostgresql::logAccount(DBInterface* pdbi, int8 type, const std::string& name, const std::string& datas, const std::string& code)
{
	std::string sql = fmt::format(
		"INSERT INTO kbe_email_verification(code, type, name, datas, logtime) "
		"VALUES('{}', {}, '{}', '{}', {}) "
		"ON CONFLICT(code) DO UPDATE SET type=EXCLUDED.type, name=EXCLUDED.name, datas=EXCLUDED.datas, logtime=EXCLUDED.logtime",
		esc(pdbi, code), (int)type, esc(pdbi, name), esc(pdbi, datas), (uint64)time(NULL));
	return pdbi->query(sql);
}

// 删除指定账号和类型的邮件验证码记录。
bool KBEEmailVerificationTablePostgresql::delAccount(DBInterface* pdbi, int8 type, const std::string& name)
{
	return pdbi->query(fmt::format("DELETE FROM kbe_email_verification WHERE type={} AND name='{}'", (int)type, esc(pdbi, name)));
}

// 根据验证码激活账号。
bool KBEEmailVerificationTablePostgresql::activateAccount(DBInterface* pdbi, const std::string& code, ACCOUNT_INFOS& info)
{
	uint64 logtime = 0;
	if (!queryVerification(pdbi, (int8)V_TYPE_CREATEACCOUNT, code, info.name, info.password, logtime))
		return false;

	if (isVerificationExpired(logtime, g_kbeSrvConfig.emailAtivationInfo_.deadline))
		return false;

	KBEAccountTable* pTable = static_cast<KBEAccountTable*>(
		EntityTables::findByInterfaceName(pdbi->name()).findKBETable(KBE_TABLE_PERFIX "_accountinfos"));
	KBE_ASSERT(pTable);

	std::string password = info.password;
	info.flags = 0;
	if (!pTable->queryAccount(pdbi, info.name, info))
		return false;

	if ((info.flags & ACCOUNT_FLAG_NOT_ACTIVATED) <= 0)
		return false;

	info.flags &= ~ACCOUNT_FLAG_NOT_ACTIVATED;
	if (!pTable->setFlagsDeadline(pdbi, info.name, info.flags, info.deadline))
		return false;

	if (!pTable->updatePassword(pdbi, info.name, password))
		return false;

	ScriptDefModule* pModule = EntityDef::findScriptModule(DBUtil::accountScriptName());
	MemoryStream copyAccountDefMemoryStream(pTable->accountDefMemoryStream());
	info.dbid = EntityTables::findByInterfaceName(pdbi->name()).writeEntity(pdbi, 0, -1, &copyAccountDefMemoryStream, pModule);
	if (info.dbid == 0)
		return false;

	if (!pdbi->query(fmt::format("UPDATE kbe_accountinfos SET dbid={} WHERE name='{}'",
		info.dbid, esc(pdbi, info.name))))
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

// 根据验证码绑定邮箱。
bool KBEEmailVerificationTablePostgresql::bindEMail(DBInterface* pdbi, const std::string& name, const std::string& code)
{
	std::string qname, qemail;
	uint64 logtime = 0;
	if (!queryVerification(pdbi, (int8)V_TYPE_BIND_MAIL, code, qname, qemail, logtime))
		return false;

	if (isVerificationExpired(logtime, g_kbeSrvConfig.emailBindInfo_.deadline))
		return false;

	if (qname.empty() || qemail.empty() || qemail != name)
		return false;

	if (!pdbi->query(fmt::format("UPDATE kbe_accountinfos SET email='{}' WHERE name='{}'",
		esc(pdbi, qemail), esc(pdbi, qname))))
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

// 根据验证码重置密码。
bool KBEEmailVerificationTablePostgresql::resetpassword(DBInterface* pdbi, const std::string& name, const std::string& password, const std::string& code)
{
	std::string qname, qemail;
	uint64 logtime = 0;
	if (!queryVerification(pdbi, (int8)V_TYPE_RESETPASSWORD, code, qname, qemail, logtime))
		return false;

	if (isVerificationExpired(logtime, g_kbeSrvConfig.emailResetPasswordInfo_.deadline))
		return false;

	if (qname.empty() || password.empty() || qname != name)
		return false;

	KBEAccountTable* pTable = static_cast<KBEAccountTable*>(
		EntityTables::findByInterfaceName(pdbi->name()).findKBETable(KBE_TABLE_PERFIX "_accountinfos"));
	KBE_ASSERT(pTable);

	if (!pTable->updatePassword(pdbi, qname, KBE_MD5::getDigest(password.data(), (int)password.length())))
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

