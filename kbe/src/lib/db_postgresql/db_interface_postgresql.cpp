// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved.

#include "db_interface_postgresql.h"
#include "db_exception_postgresql.h"
#include "entity_table_postgresql.h"
#include "kbe_table_postgresql.h"
#include "server/serverconfig.h"
#include "thread/threadguard.h"
#include "helper/watcher.h"

namespace KBEngine {
namespace
{
static KBEngine::thread::ThreadMutex g_pgLogMutex;
static KBEUnordered_map<std::string, uint32> g_pgQueryStatistics;
static bool g_pgInstalledWatcher = false;
static bool g_pgDebug = false;

// 从 SQL 文本中取第一个操作词，用于 watcher 统计。
static void querystatistics(const char* strCommand, uint32 size)
{
	std::string op;
	for (uint32 i = 0; i < size; ++i)
	{
		if (strCommand[i] == ' ')
			break;
		op += strCommand[i];
	}

	if (op.empty())
		return;

	std::transform(op.begin(), op.end(), op.begin(), toupper);
	KBEngine::thread::ThreadGuard tg(&g_pgLogMutex);
	g_pgQueryStatistics[op] += 1;
}

// 读取指定 SQL 操作词的累计执行次数。
static uint32 watcher_query(std::string cmd)
{
	KBEngine::thread::ThreadGuard tg(&g_pgLogMutex);
	KBEUnordered_map<std::string, uint32>::iterator iter = g_pgQueryStatistics.find(cmd);
	return iter == g_pgQueryStatistics.end() ? 0 : iter->second;
}

// 各 SQL 操作的 watcher 入口，保持 watcher 注册接口的函数签名。
static uint32 watcher_select() { return watcher_query("SELECT"); }
static uint32 watcher_delete() { return watcher_query("DELETE"); }
static uint32 watcher_insert() { return watcher_query("INSERT"); }
static uint32 watcher_update() { return watcher_query("UPDATE"); }
static uint32 watcher_create() { return watcher_query("CREATE"); }
static uint32 watcher_drop() { return watcher_query("DROP"); }
static uint32 watcher_alter() { return watcher_query("ALTER"); }

// PostgreSQL 没有 MySQL 的 utf8mb4 名称，完整 UTF-8 编码统一映射成 UTF8。
static std::string normalizeClientEncoding(const std::string& characterSet)
{
	std::string encoding = characterSet;
	std::transform(encoding.begin(), encoding.end(), encoding.begin(), tolower);

	if (encoding == "utf8" || encoding == "utf8mb4")
		return "UTF8";

	return characterSet;
}

// 初始化 PostgreSQL 后端自己的 SQL 统计 watcher。
static void initializeWatcher()
{
	if (g_pgInstalledWatcher)
		return;

	// PostgreSQL 查询统计单独挂 watcher，避免和 MySQL 后端混在一起看。
	g_pgInstalledWatcher = true;
	g_pgDebug = g_kbeSrvConfig.getDBMgr().debugDBMgr;
	WATCH_OBJECT("db_postgresql_querys/select", &watcher_select);
	WATCH_OBJECT("db_postgresql_querys/delete", &watcher_delete);
	WATCH_OBJECT("db_postgresql_querys/insert", &watcher_insert);
	WATCH_OBJECT("db_postgresql_querys/update", &watcher_update);
	WATCH_OBJECT("db_postgresql_querys/create", &watcher_create);
	WATCH_OBJECT("db_postgresql_querys/drop", &watcher_drop);
	WATCH_OBJECT("db_postgresql_querys/alter", &watcher_alter);
}
}

// 保存接口名和字符集配置，连接对象在 attach 时创建。
DBInterfacePostgresql::DBInterfacePostgresql(const char* name, std::string characterSet, std::string collation) :
	DBInterface(name),
	pConn_(NULL),
	characterSet_(characterSet),
	collation_(collation),
	lastError_(),
	lastSqlState_(),
	inTransaction_(false),
	cstr_()
{
	DEBUG_MSG(fmt::format("DBInterfacePostgresql::DBInterfacePostgresql: {}\n", name));
}

// 析构时断开 libpq 连接。
DBInterfacePostgresql::~DBInterfacePostgresql()
{
	detach();
}

// 注册 PostgreSQL 版本的 KBE 系统表。
bool DBInterfacePostgresql::initInterface(DBInterface* pdbi)
{
	EntityTables& entityTables = EntityTables::findByInterfaceName(pdbi->name());

	// 系统表使用 PostgreSQL 方言实现，不复用 MySQL 的 SQL。
	entityTables.addKBETable(new KBEAccountTablePostgresql(&entityTables));
	entityTables.addKBETable(new KBEServerLogTablePostgresql(&entityTables));
	entityTables.addKBETable(new KBEEntityLogTablePostgresql(&entityTables));
	entityTables.addKBETable(new KBEEmailVerificationTablePostgresql(&entityTables));
	return true;
}

// 按当前 DBInterface 配置生成 libpq 连接串。
std::string DBInterfacePostgresql::buildConnInfo(const char* databaseName) const
{
	// libpq 关键字连接串由 PQconnectdb 解析。
	std::string conninfo = fmt::format("host='{}' port='{}' user='{}' password='{}' dbname='{}'",
		db_ip_, db_port_, db_username_, db_password_, databaseName ? databaseName : db_name_);

	// 复用现有 SSL 配置开关，PostgreSQL 侧映射为 sslmode=require。
	if (db_mysql_ssl_)
		conninfo += " sslmode=require";

	return conninfo;
}

// 建立到指定数据库的 PGconn 连接。
bool DBInterfacePostgresql::connectToDatabase(const char* databaseName)
{
	detach();
	pConn_ = PQconnectdb(buildConnInfo(databaseName).c_str());
	if (pConn_ == NULL)
	{
		lastError_ = "PQconnectdb returned NULL";
		ERROR_MSG(fmt::format("DBInterfacePostgresql::connectToDatabase: connect failed, db={}, error={}\n",
			databaseName ? databaseName : db_name_, lastError_));
		return false;
	}

	if (PQstatus(pConn_) != CONNECTION_OK)
	{
		lastError_ = PQerrorMessage(pConn_);
		ERROR_MSG(fmt::format("DBInterfacePostgresql::connectToDatabase: connect failed, db={}, error={}\n",
			databaseName ? databaseName : db_name_, lastError_));
		detach();
		return false;
	}

	return true;
}

// 初始化 PostgreSQL 连接，必要时创建目标数据库。
bool DBInterfacePostgresql::attach(const char* databaseName)
{
	if (!g_pgInstalledWatcher)
		initializeWatcher();

	// 配置未写端口时使用 PostgreSQL 默认端口。
	if (db_port_ == 0)
		db_port_ = 5432;

	if (databaseName != NULL)
		kbe_snprintf(db_name_, MAX_BUF, "%s", databaseName);

	// 目标库不存在时，先连 postgres 管理库创建。
	if (!connectToDatabase(db_name_))
	{
		if (!ensureDatabaseExists())
			return false;

		if (!connectToDatabase(db_name_))
			return false;
	}

	// PostgreSQL 连接编码使用 UTF8 名称，不读取 MySQL collation。
	std::string clientEncoding = normalizeClientEncoding(characterSet_);
	if (PQsetClientEncoding(pConn_, clientEncoding.c_str()) != 0)
	{
		lastError_ = PQerrorMessage(pConn_);
		ERROR_MSG(fmt::format("DBInterfacePostgresql::attach: set client encoding failed, encoding={}, error={}\n",
			clientEncoding, lastError_));
		return false;
	}

	DEBUG_MSG(fmt::format("DBInterfacePostgresql::attach: successfully! addr: {}:{}\n", db_ip_, db_port_));
	return true;
}

// 连接 postgres 管理库并创建配置中的目标数据库。
bool DBInterfacePostgresql::ensureDatabaseExists()
{
	// 目标库还不存在时，只能先连接默认 postgres 数据库。
	if (!connectToDatabase("postgres"))
		return false;

	// 数据库名按 identifier 转义，避免特殊字符破坏 CREATE DATABASE。
	std::string sql = fmt::format("CREATE DATABASE {}", quoteIdentifier(db_name_));
	PGresult* result = PQexec(pConn_, sql.c_str());
	if (result == NULL)
	{
		updateLastError(NULL);
		detach();
		return false;
	}

	ExecStatusType status = PQresultStatus(result);

	// 42P04=duplicate_database，说明并发或上次尝试已经建好。
	const char* state = PQresultErrorField(result, PG_DIAG_SQLSTATE);
	bool ok = status == PGRES_COMMAND_OK || (state != NULL && strcmp(state, "42P04") == 0);
	if (!ok)
		updateLastError(result);

	PQclear(result);
	detach();
	return ok;
}

// 关闭当前 PGconn。
bool DBInterfacePostgresql::detach()
{
	if (pConn_)
	{
		PQfinish(pConn_);
		pConn_ = NULL;
	}

	inTransaction_ = false;
	return true;
}

// 重新建立当前数据库连接。
bool DBInterfacePostgresql::reattach()
{
	detach();
	return attach();
}

// 轻量探测连接是否可用。
bool DBInterfacePostgresql::checkEnvironment()
{
	return query("SELECT 1", static_cast<uint32>(strlen("SELECT 1")), false, NULL);
}

// 保留 DBInterface 的错误检查入口。
bool DBInterfacePostgresql::checkErrors()
{
	// PostgreSQL 暂无 MySQL 那套历史错误修复逻辑，先保留统一入口。
	return true;
}

// 从 PGresult/PGconn 中同步最后一次错误文本和 SQLSTATE。
void DBInterfacePostgresql::updateLastError(PGresult* result)
{
	lastError_.clear();
	lastSqlState_.clear();

	if (result)
	{
		const char* msg = PQresultErrorMessage(result);
		const char* state = PQresultErrorField(result, PG_DIAG_SQLSTATE);
		if (msg)
			lastError_ = msg;
		if (state)
			lastSqlState_ = state;
	}

	if (lastError_.empty() && pConn_)
		lastError_ = PQerrorMessage(pConn_);
}

// 执行上层传入的 SQL，并按需要把结果写入 MemoryStream。
bool DBInterfacePostgresql::query(const char* cmd, uint32 size, bool printlog, MemoryStream* result)
{
	if (pConn_ == NULL)
	{
		lastError_ = "PostgreSQL connection is not attached";
		if (printlog)
			ERROR_MSG(fmt::format("DBInterfacePostgresql::query: {}\nsql:({})\n", lastError_, lastquery_));
		return false;
	}

	// lastquery_ 保持和 MySQL 后端一致，日志排查时能直接看到最后一条 SQL。
	querystatistics(cmd, size);
	lastquery_.assign(cmd, size);

	if (g_pgDebug)
		DEBUG_MSG(fmt::format("DBInterfacePostgresql::query({:p}): {}\n", (void*)this, lastquery_));

	// 上层仍负责生成 SQL，这里只通过 libpq 执行。
	PGresult* pgResult = PQexec(pConn_, lastquery_.c_str());
	if (pgResult == NULL)
	{
		updateLastError(NULL);
		if (printlog)
		{
			ERROR_MSG(fmt::format("DBInterfacePostgresql::query: PQexec returned NULL, error({})!\nsql:({})\n",
				lastError_, lastquery_));
		}

		throw DBExceptionPostgresql(this, lastError_, lastSqlState_);
	}

	ExecStatusType status = PQresultStatus(pgResult);
	bool ok = status == PGRES_TUPLES_OK || status == PGRES_COMMAND_OK;

	if (!ok)
	{
		updateLastError(pgResult);
		if (printlog)
		{
			ERROR_MSG(fmt::format("DBInterfacePostgresql::query: error({})!\nsql:({})\n",
				lastError_, lastquery_));
		}

		DBExceptionPostgresql e(this, lastError_, lastSqlState_);
		PQclear(pgResult);
		throw e;
	}

	bool ret = result == NULL || write_query_result(pgResult, result);
	PQclear(pgResult);
	return ret;
}

// 少数调用方需要直接读取 PGresult，例如系统表查询和 RETURNING id。
// 这里把这类 SQL 也收回 DBInterfacePostgresql，避免绕过 lastquery、watcher 和断线重试判断。
PGresult* DBInterfacePostgresql::queryResult(const std::string& sql, const char* caller,
	ExecStatusType expectedStatus, bool traceQuery)
{
	if (pConn_ == NULL)
	{
		lastError_ = "PostgreSQL connection is not attached";
		ERROR_MSG(fmt::format("{}: {}\nsql:({})\n", caller, lastError_, sql));
		throw DBExceptionPostgresql(this, lastError_, lastSqlState_);
	}

	if (traceQuery)
	{
		querystatistics(sql.c_str(), static_cast<uint32>(sql.size()));
		lastquery_ = sql;
	}

	if (g_pgDebug && traceQuery)
		DEBUG_MSG(fmt::format("DBInterfacePostgresql::queryResult({:p}, {}): {}\n", (void*)this, caller, sql));

	PGresult* result = PQexec(pConn_, sql.c_str());
	if (result == NULL)
	{
		updateLastError(NULL);
		ERROR_MSG(fmt::format("{}: PQexec returned NULL, error={}!\nsql:({})\n", caller, lastError_, lastquery_));
		throw DBExceptionPostgresql(this, lastError_, lastSqlState_);
	}

	if (PQresultStatus(result) != expectedStatus)
	{
		updateLastError(result);
		ERROR_MSG(fmt::format("{}: error({})!\nsql:({})\n", caller, lastError_, lastquery_));

		DBExceptionPostgresql e(this, lastError_, lastSqlState_);
		PQclear(result);
		throw e;
	}

	return result;
}

// 将 PGresult 转成 DBInterface 约定的查询返回格式。
bool DBInterfacePostgresql::write_query_result(PGresult* pgResult, MemoryStream* result)
{
	if (!result)
		return true;

	size_t wpos = result->wpos();
	try
	{
		// 查询结果按 DBInterface 既有协议写回，脚本层不区分具体数据库。
		if (PQresultStatus(pgResult) == PGRES_TUPLES_OK)
		{
			uint32 nfields = static_cast<uint32>(PQnfields(pgResult));
			uint32 nrows = static_cast<uint32>(PQntuples(pgResult));
			(*result) << nfields << nrows;

			for (uint32 r = 0; r < nrows; ++r)
			{
				for (uint32 c = 0; c < nfields; ++c)
				{
					if (PQgetisnull(pgResult, r, c))
						result->appendBlob("KBE_QUERY_DB_NULL", static_cast<ArraySize>(strlen("KBE_QUERY_DB_NULL")));
					else
						result->appendBlob(PQgetvalue(pgResult, r, c), static_cast<ArraySize>(PQgetlength(pgResult, r, c)));
				}
			}
		}
		else
		{
			// PostgreSQL 的自增值需要 RETURNING 显式返回，这里不伪造 lastInsertID。
			uint32 nfields = 0;
			uint64 affectedRows = 0;
			uint64 lastInsertID = 0;
			const char* affected = PQcmdTuples(pgResult);
			if (affected && affected[0] != '\0')
				KBEngine::StringConv::str2value(affectedRows, affected);

			(*result) << nfields;
			(*result) << affectedRows;
			(*result) << lastInsertID;
		}
	}
	catch (MemoryStreamWriteOverflow& e)
	{
		KBE_ASSERT(wpos <= static_cast<size_t>(std::numeric_limits<int>::max()));
		result->wpos(static_cast<int>(wpos));
		lastError_ = fmt::format("DBInterfacePostgresql::write_query_result: {}, SQL({})", e.what(), lastquery_);
		return false;
	}

	return true;
}

// 读取当前 schema 下的表名列表。
bool DBInterfacePostgresql::getTableNames(std::vector<std::string>& tableNames, const char* pattern)
{
	// 只读取 public schema 下的基础表，和当前建表逻辑保持一致。
	std::string like = pattern && pattern[0] ? escapeString(pattern, strlen(pattern)) : "%";
	std::string sql = fmt::format(
		"SELECT table_name FROM information_schema.tables "
		"WHERE table_schema='public' AND table_type='BASE TABLE' AND table_name LIKE '{}' "
		"ORDER BY table_name",
		like);

	PGresult* result = PQexec(pConn_, sql.c_str());
	if (result == NULL)
	{
		updateLastError(NULL);
		return false;
	}

	if (PQresultStatus(result) != PGRES_TUPLES_OK)
	{
		updateLastError(result);
		PQclear(result);
		return false;
	}

	int rows = PQntuples(result);
	for (int i = 0; i < rows; ++i)
		tableNames.push_back(PQgetvalue(result, i, 0));

	PQclear(result);
	return true;
}

// 读取指定表的字段名列表。
bool DBInterfacePostgresql::getTableItemNames(const char* tableName, std::vector<std::string>& itemNames)
{
	// 字段信息直接查 information_schema，不依赖 psql 元命令。
	std::string sql = fmt::format(
		"SELECT column_name FROM information_schema.columns "
		"WHERE table_schema='public' AND table_name='{}' ORDER BY ordinal_position",
		escapeString(tableName, strlen(tableName)));

	PGresult* result = PQexec(pConn_, sql.c_str());
	if (result == NULL)
	{
		updateLastError(NULL);
		return false;
	}

	if (PQresultStatus(result) != PGRES_TUPLES_OK)
	{
		updateLastError(result);
		PQclear(result);
		return false;
	}

	int rows = PQntuples(result);
	for (int i = 0; i < rows; ++i)
		itemNames.push_back(PQgetvalue(result, i, 0));

	PQclear(result);
	return true;
}

// 创建 PostgreSQL 实体表对象。
EntityTable* DBInterfacePostgresql::createEntityTable(EntityTables* pEntityTables)
{
	return new EntityTablePostgresql(pEntityTables);
}

// 删除实体表。
bool DBInterfacePostgresql::dropEntityTableFromDB(const char* tableName)
{
	KBE_ASSERT(tableName != NULL);

	DEBUG_MSG(fmt::format("DBInterfacePostgresql::dropEntityTableFromDB: {}.\n", tableName));

	std::string sql = fmt::format("DROP TABLE IF EXISTS {}", quoteIdentifier(tableName));
	return query(sql);
}

// 删除实体表字段。
bool DBInterfacePostgresql::dropEntityTableItemFromDB(const char* tableName, const char* tableItemName)
{
	KBE_ASSERT(tableName != NULL && tableItemName != NULL);

	DEBUG_MSG(fmt::format("DBInterfacePostgresql::dropEntityTableItemFromDB: {} {}.\n",
		tableName, tableItemName));

	std::string sql = fmt::format("ALTER TABLE {} DROP COLUMN IF EXISTS {}",
		quoteIdentifier(tableName), quoteIdentifier(tableItemName));
	return query(sql);
}

// 返回当前数据库接口状态字符串。
const char* DBInterfacePostgresql::c_str()
{
	kbe_snprintf(cstr_, MAX_BUF * 2, "dbinterface(%s): type=postgresql, addr=%s:%u, db=%s, username=%s, connected=%s",
		name_, db_ip_, db_port_, db_name_, db_username_, (pConn_ == NULL ? "no" : "yes"));
	return cstr_;
}

// 返回最后一次 PostgreSQL 错误文本。
const char* DBInterfacePostgresql::getstrerror()
{
	return lastError_.c_str();
}

void DBInterfacePostgresql::inTransaction(bool value)
{
	KBE_ASSERT(inTransaction_ != value);
	inTransaction_ = value;
}

// DBInterface 的整型错误码入口，PostgreSQL 错误细节以 SQLSTATE 字符串保存。
int DBInterfacePostgresql::getlasterror()
{
	// PostgreSQL 的错误码是 SQLSTATE 字符串，这里返回 0 并通过 getstrerror 输出完整信息。
	return 0;
}

// 开启事务。
bool DBInterfacePostgresql::lock()
{
	if (query("BEGIN"))
	{
		inTransaction(true);
		return true;
	}

	return false;
}

// 提交事务。
bool DBInterfacePostgresql::unlock()
{
	if (query("COMMIT"))
	{
		inTransaction(false);
		return true;
	}

	return false;
}

// 处理 PostgreSQL 异常，断线时重连，可重试错误交回 DB 任务队列重跑。
bool DBInterfacePostgresql::processException(std::exception& e)
{
	DBExceptionPostgresql* pException = dynamic_cast<DBExceptionPostgresql*>(&e);
	if (!pException)
		return false;

	if (pException->isLostConnection())
	{
		WARNING_MSG(fmt::format("DBInterfacePostgresql::processException: lost connection, retry attach. lastquery={}\n", lastquery_));
		return reattach();
	}

	return pException->shouldRetry();
}

// 返回 dbmgr 配置里的自增初始值。
const char* DBInterfacePostgresql::getAutoIncrementInit()
{
	DBInterfaceInfo* pDBInfo = g_kbeSrvConfig.dbInterface(name());
	return pDBInfo ? pDBInfo->db_autoIncrementInit : NULL;
}

// 转义 SQL 字符串字面量内容。
std::string DBInterfacePostgresql::escapeString(const char* data, size_t size)
{
	std::string out;
	out.resize(size * 2 + 1);

	int error = 0;
	size_t len = PQescapeStringConn(pConn_, &out[0], data, size, &error);
	out.resize(len);

	if (error)
		lastError_ = PQerrorMessage(pConn_);

	return out;
}

// 转义 PostgreSQL identifier，例如库名、表名、字段名。
std::string DBInterfacePostgresql::quoteIdentifier(const char* identifier)
{
	char* quoted = PQescapeIdentifier(pConn_, identifier, strlen(identifier));
	if (!quoted)
		return identifier;

	std::string out = quoted;
	PQfreemem(quoted);
	return out;
}

}

