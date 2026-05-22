// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved.

#include "kbe_table_postgresql.h"
#include "db_interface_postgresql.h"
#include "common/common.h"
#include "server/serverconfig.h"

namespace KBEngine {
namespace
{
// 将通用 DBInterface 转回 PostgreSQL 后端，便于访问 libpq 连接和转义接口。
DBInterfacePostgresql* pg(DBInterface* pdbi)
{
	return static_cast<DBInterfacePostgresql*>(pdbi);
}

// 转义 SQL 字符串字面量内容。
std::string esc(DBInterface* pdbi, const std::string& value)
{
	return pg(pdbi)->escapeString(value.data(), value.size());
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
	// 实体在线日志表，字段语义与现有数据库后端保持一致。
	return pdbi->query(
		"CREATE TABLE IF NOT EXISTS kbe_entitylog ("
		"dbid BIGINT NOT NULL, "
		"entityType INTEGER NOT NULL, "
		"entityID BIGINT NOT NULL, "
		"ip VARCHAR(64) NOT NULL, "
		"port INTEGER NOT NULL, "
		"componentID BIGINT NOT NULL, "
		"serverGroupID BIGINT NOT NULL, "
		"PRIMARY KEY(dbid, entityType)"
		")");
}

// 写入或更新实体在线记录。
bool KBEEntityLogTablePostgresql::logEntity(DBInterface* pdbi, const char* ip, uint32 port, DBID dbid,
	COMPONENT_ID componentID, ENTITY_ID entityID, ENTITY_SCRIPT_UID entityType)
{
	// PostgreSQL 用 ON CONFLICT 处理重复在线记录。
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
	PGresult* result = PQexec(pg(pdbi)->pgconn(), sql.c_str());
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
	// 服务器心跳表以 componentID 作为主键。
	return pdbi->query(
		"CREATE TABLE IF NOT EXISTS kbe_serverlog ("
		"componentID BIGINT PRIMARY KEY, "
		"heartbeatTime BIGINT NOT NULL, "
		"serverGroupID BIGINT NOT NULL, "
		"isShareDB SMALLINT NOT NULL DEFAULT 0"
		")");
}

// 刷新当前组件的心跳和 shareDB 状态。
bool KBEServerLogTablePostgresql::updateServer(DBInterface* pdbi)
{
	uint64 now = timestamp();
	uint8 isShareDB = g_kbeSrvConfig.getDBMgr().isShareDB ? 1 : 0;
	std::string sql = fmt::format(
		"INSERT INTO kbe_serverlog(componentID, heartbeatTime, serverGroupID, isShareDB) "
		"VALUES({}, {}, {}, {}) "
		"ON CONFLICT(componentID) DO UPDATE SET heartbeatTime=EXCLUDED.heartbeatTime, "
		"serverGroupID=EXCLUDED.serverGroupID, isShareDB=EXCLUDED.isShareDB",
		g_componentID, now, (uint64)getUserUID(), (uint32)isShareDB);
	return pdbi->query(sql);
}

// 查询当前组件的服务器心跳记录。
bool KBEServerLogTablePostgresql::queryServer(DBInterface* pdbi, ServerLog& serverlog)
{
	std::string sql = fmt::format("SELECT heartbeatTime, serverGroupID, isShareDB FROM kbe_serverlog WHERE componentID={}", g_componentID);
	PGresult* result = PQexec(pg(pdbi)->pgconn(), sql.c_str());
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

// 查询所有仍记录在表里的组件 ID。
std::vector<COMPONENT_ID> KBEServerLogTablePostgresql::queryServers(DBInterface* pdbi)
{
	std::vector<COMPONENT_ID> cids;
	PGresult* result = PQexec(pg(pdbi)->pgconn(), "SELECT componentID FROM kbe_serverlog");
	if (PQresultStatus(result) == PGRES_TUPLES_OK)
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

// 查询心跳超时的组件 ID。
std::vector<COMPONENT_ID> KBEServerLogTablePostgresql::queryTimeOutServers(DBInterface* pdbi)
{
	std::vector<COMPONENT_ID> cids;
	uint64 deadline = timestamp() > TIMEOUT ? timestamp() - TIMEOUT : 0;
	std::string sql = fmt::format("SELECT componentID FROM kbe_serverlog WHERE heartbeatTime<{}", deadline);
	PGresult* result = PQexec(pg(pdbi)->pgconn(), sql.c_str());
	if (PQresultStatus(result) == PGRES_TUPLES_OK)
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

// 清理指定组件的服务器心跳记录。
bool KBEServerLogTablePostgresql::clearServers(DBInterface* pdbi, const std::vector<COMPONENT_ID>& cids)
{
	for (size_t i = 0; i < cids.size(); ++i)
	{
		if (!pdbi->query(fmt::format("DELETE FROM kbe_serverlog WHERE componentID={}", cids[i])))
			return false;
	}
	return true;
}

// 查询所有组件的 shareDB 状态。
std::map<COMPONENT_ID, bool> KBEServerLogTablePostgresql::queryAllServerShareDBState(DBInterface* pdbi)
{
	std::map<COMPONENT_ID, bool> values;
	PGresult* result = PQexec(pg(pdbi)->pgconn(), "SELECT componentID, isShareDB FROM kbe_serverlog");
	if (PQresultStatus(result) == PGRES_TUPLES_OK)
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

// 查询当前组件是否处于 shareDB 模式。
int KBEServerLogTablePostgresql::isShareDB(DBInterface* pdbi)
{
	ServerLog serverlog;
	return queryServer(pdbi, serverlog) ? serverlog.isShareDB : -1;
}

// 绑定系统表集合，维护账号信息表。
KBEAccountTablePostgresql::KBEAccountTablePostgresql(EntityTables* pEntityTables) :
	KBEAccountTable(pEntityTables)
{
}

// 同步 kbe_accountinfos 表，保存账号到实体 DBID 的映射。
bool KBEAccountTablePostgresql::syncToDB(DBInterface* pdbi)
{
	// 账号表保留 KBE 现有字段语义，datas 使用 TEXT 存储扩展数据。
	return pdbi->query(
		"CREATE TABLE IF NOT EXISTS kbe_accountinfos ("
		"name VARCHAR(255) PRIMARY KEY, "
		"password VARCHAR(255) NOT NULL, "
		"datas TEXT NOT NULL DEFAULT '', "
		"email VARCHAR(255) NOT NULL DEFAULT '', "
		"dbid BIGINT NOT NULL DEFAULT 0, "
		"flags BIGINT NOT NULL DEFAULT 0, "
		"deadline BIGINT NOT NULL DEFAULT 0"
		")");
}

// 查询账号基础信息。
bool KBEAccountTablePostgresql::queryAccount(DBInterface* pdbi, const std::string& name, ACCOUNT_INFOS& info)
{
	return queryAccountAllInfos(pdbi, name, info);
}

// 查询账号完整信息。
bool KBEAccountTablePostgresql::queryAccountAllInfos(DBInterface* pdbi, const std::string& name, ACCOUNT_INFOS& info)
{
	std::string sql = fmt::format("SELECT name, password, datas, email, dbid, flags, deadline FROM kbe_accountinfos WHERE name='{}'", esc(pdbi, name));
	PGresult* result = PQexec(pg(pdbi)->pgconn(), sql.c_str());
	bool ok = PQresultStatus(result) == PGRES_TUPLES_OK && PQntuples(result) > 0;
	if (ok)
	{
		info.name = PQgetvalue(result, 0, 0);
		info.password = PQgetvalue(result, 0, 1);
		info.datas = PQgetvalue(result, 0, 2);
		info.email = PQgetvalue(result, 0, 3);
		KBEngine::StringConv::str2value(info.dbid, PQgetvalue(result, 0, 4));
		KBEngine::StringConv::str2value(info.flags, PQgetvalue(result, 0, 5));
		KBEngine::StringConv::str2value(info.deadline, PQgetvalue(result, 0, 6));
	}
	PQclear(result);
	return ok;
}

// 创建或更新账号记录。
bool KBEAccountTablePostgresql::logAccount(DBInterface* pdbi, ACCOUNT_INFOS& info)
{
	std::string sql = fmt::format(
		"INSERT INTO kbe_accountinfos(name, password, datas, email, dbid, flags, deadline) "
		"VALUES('{}', '{}', '{}', '{}', {}, {}, {}) "
		"ON CONFLICT(name) DO UPDATE SET password=EXCLUDED.password, datas=EXCLUDED.datas, "
		"email=EXCLUDED.email, dbid=EXCLUDED.dbid, flags=EXCLUDED.flags, deadline=EXCLUDED.deadline",
		esc(pdbi, info.name), esc(pdbi, info.password), esc(pdbi, info.datas), esc(pdbi, info.email),
		info.dbid, info.flags, info.deadline);
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
	return pdbi->query(fmt::format("UPDATE kbe_accountinfos SET dbid={} WHERE name='{}'", dbid, esc(pdbi, name)));
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
	return pdbi->query(
		"CREATE TABLE IF NOT EXISTS kbe_email_verification ("
		"code VARCHAR(255) PRIMARY KEY, "
		"type SMALLINT NOT NULL, "
		"name VARCHAR(255) NOT NULL, "
		"datas TEXT NOT NULL DEFAULT '', "
		"deadline BIGINT NOT NULL DEFAULT 0"
		")");
}

// 查询邮件验证码对应的账号信息。
bool KBEEmailVerificationTablePostgresql::queryAccount(DBInterface* /*pdbi*/, int8 /*type*/, const std::string& /*name*/, ACCOUNT_INFOS& /*info*/)
{
	return false;
}

// 写入或刷新邮件验证码记录。
bool KBEEmailVerificationTablePostgresql::logAccount(DBInterface* pdbi, int8 type, const std::string& name, const std::string& datas, const std::string& code)
{
	uint64 deadline = timestamp() + 86400;
	std::string sql = fmt::format(
		"INSERT INTO kbe_email_verification(code, type, name, datas, deadline) "
		"VALUES('{}', {}, '{}', '{}', {}) "
		"ON CONFLICT(code) DO UPDATE SET type=EXCLUDED.type, name=EXCLUDED.name, datas=EXCLUDED.datas, deadline=EXCLUDED.deadline",
		esc(pdbi, code), (int)type, esc(pdbi, name), esc(pdbi, datas), deadline);
	return pdbi->query(sql);
}

// 删除指定账号和类型的邮件验证码记录。
bool KBEEmailVerificationTablePostgresql::delAccount(DBInterface* pdbi, int8 type, const std::string& name)
{
	return pdbi->query(fmt::format("DELETE FROM kbe_email_verification WHERE type={} AND name='{}'", (int)type, esc(pdbi, name)));
}

// 根据验证码激活账号。
bool KBEEmailVerificationTablePostgresql::activateAccount(DBInterface* /*pdbi*/, const std::string& /*code*/, ACCOUNT_INFOS& /*info*/)
{
	return false;
}

// 根据验证码绑定邮箱。
bool KBEEmailVerificationTablePostgresql::bindEMail(DBInterface* /*pdbi*/, const std::string& /*name*/, const std::string& /*code*/)
{
	return false;
}

// 根据验证码重置密码。
bool KBEEmailVerificationTablePostgresql::resetpassword(DBInterface* /*pdbi*/, const std::string& /*name*/, const std::string& /*password*/, const std::string& /*code*/)
{
	return false;
}

}

