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


#include "db_interface.h"
#include "db_threadpool.h"
#include "entity_table.h"
#include "common/kbekey.h"
#include "db_redis/db_interface_redis.h"
#include "db_mongodb/db_interface_mongodb.h"
#include "db_mysql/db_interface_mysql.h"
#include "db_postgresql/db_interface_postgresql.h"
#include "server/serverconfig.h"
#include "thread/threadpool.h"

#include <cctype>

namespace KBEngine { 
KBE_SINGLETON_INIT(DBUtil);

DBUtil g_DBUtil;

DBUtil::DBThreadPoolMap DBUtil::pThreadPoolMaps_;

namespace
{
bool isRawCommandBoundary(char value)
{
	return !std::isalnum(static_cast<unsigned char>(value)) && value != '_';
}

bool findBlacklistedRawCommand(const std::string& command,
	const std::vector<std::string>& blacklist, std::string& hitWord)
{
	if (command.empty() || blacklist.empty())
		return false;

	const std::string normalizedCommand = strutil::toLower(command);
	for (std::vector<std::string>::const_iterator iter = blacklist.begin(); iter != blacklist.end(); ++iter)
	{
		const std::string& word = *iter;
		if (word.empty())
			continue;

		std::string::size_type position = 0;
		while ((position = normalizedCommand.find(word, position)) != std::string::npos)
		{
			const std::string::size_type end = position + word.size();
			const bool leftBoundary = position == 0 || isRawCommandBoundary(normalizedCommand[position - 1]);
			const bool rightBoundary = end == normalizedCommand.size() || isRawCommandBoundary(normalizedCommand[end]);
			if (leftBoundary && rightBoundary)
			{
				hitWord = word;
				return true;
			}

			++position;
		}
	}

	return false;
}
}

//-------------------------------------------------------------------------------------
bool DBInterface::checkRawDatabaseCommandAllowed(const std::string& command, std::string& error) const
{
	if (!g_kbeSrvConfig.enableRawDatabaseCommandBlacklist())
		return true;

	std::string hitWord;
	if (!findBlacklistedRawCommand(command, g_kbeSrvConfig.rawDatabaseCommandBlacklist(dbType()), hitWord))
		return true;

	error = fmt::format("executeRawDatabaseCommand blocked by blacklist: dbInterface={}, dbType={}, hit={}",
		name(), dbType(), hitWord);
	// 不记录完整命令，避免账号、令牌或业务数据通过安全日志再次泄露。
	// Do not log the full command because credentials, tokens, or business data could be leaked again through the security log.
	WARNING_MSG(fmt::format("{}.\n", error));
	return false;
}

//-------------------------------------------------------------------------------------
DBUtil::DBUtil()
{
}

//-------------------------------------------------------------------------------------
DBUtil::~DBUtil()
{
}

//-------------------------------------------------------------------------------------
bool DBUtil::initializeWatcher()
{
	DBUtil::DBThreadPoolMap::iterator iter = pThreadPoolMaps_.begin();
	for (; iter != pThreadPoolMaps_.end(); ++iter)
	{
		iter->second->initializeWatcher();
	}

	return true;
}

//-------------------------------------------------------------------------------------
bool DBUtil::initThread(const std::string& dbinterfaceName)
{
	DBInterfaceInfo* pDBInfo = g_kbeSrvConfig.dbInterface(dbinterfaceName);
	if (strcmp(pDBInfo->db_type, "mysql") == 0)
	{
		if (!mysql_thread_safe()) 
		{
			KBE_ASSERT(false);
		}
		else
		{
			mysql_thread_init();
		}
	}
	
	return true;
}

//-------------------------------------------------------------------------------------
bool DBUtil::finiThread(const std::string& dbinterfaceName)
{
	DBInterfaceInfo* pDBInfo = g_kbeSrvConfig.dbInterface(dbinterfaceName);
	if (strcmp(pDBInfo->db_type, "mysql") == 0)
	{
		mysql_thread_end();
	}

	return true;
}

//-------------------------------------------------------------------------------------
bool DBUtil::initialize()
{
	ENGINE_COMPONENT_INFO& dbcfg = g_kbeSrvConfig.getDBMgr();
	if (dbcfg.dbInterfaceInfos.size() == 0)
	{
		ERROR_MSG(fmt::format("DBUtil::initialize: not found database interface! (kbengine[_defs].xml->dbmgr->databaseInterfaces)\n"));
		return false;
	}

	std::vector<DBInterfaceInfo>::iterator dbinfo_iter = dbcfg.dbInterfaceInfos.begin();
	for (; dbinfo_iter != dbcfg.dbInterfaceInfos.end(); ++dbinfo_iter)
	{
		pThreadPoolMaps_[(*dbinfo_iter).name] = new DBThreadPool((*dbinfo_iter).name);

		if ((*dbinfo_iter).db_passwordEncrypt)
		{
			// 如果小于64则表明当前是明文密码配置
			if (strlen((*dbinfo_iter).db_password) < 64)
			{
				WARNING_MSG(fmt::format("DBUtil::initialize: db({}) password is not encrypted!\nplease use password(rsa):\n{}\n",
					(*dbinfo_iter).name, KBEKey::getSingleton().encrypt((*dbinfo_iter).db_password)));
			}
			else
			{
				std::string out = KBEKey::getSingleton().decrypt((*dbinfo_iter).db_password);

				if (out.size() == 0)
					return false;

				kbe_snprintf((*dbinfo_iter).db_password, MAX_BUF, "%s", out.c_str());
			}
		}
	}

	return true;
}

//-------------------------------------------------------------------------------------
void DBUtil::finalise()
{
	DBUtil::DBThreadPoolMap::iterator iter = pThreadPoolMaps_.begin();
	for (; iter != pThreadPoolMaps_.end(); ++iter)
	{
		iter->second->finalise();
		SAFE_RELEASE(iter->second);
	}

	pThreadPoolMaps_.clear();
}

//-------------------------------------------------------------------------------------
void DBUtil::handleMainTick()
{
	DBUtil::DBThreadPoolMap::iterator iter = pThreadPoolMaps_.begin();
	for (; iter != pThreadPoolMaps_.end(); ++iter)
		iter->second->onMainThreadTick();
}

//-------------------------------------------------------------------------------------
DBInterface* DBUtil::createInterface(const std::string& name, bool showinfo)
{
	DBInterfaceInfo* pDBInfo = g_kbeSrvConfig.dbInterface(name);
	if (!pDBInfo)
	{
		ERROR_MSG(fmt::format("DBUtil::createInterface: not found dbInterface({})\n",
			name));

		return NULL;
	}

	DBInterface* dbinterface = NULL;

	if (strcmp(pDBInfo->db_type, "mysql") == 0)
	{
		dbinterface = new DBInterfaceMysql(name.c_str(), pDBInfo->db_unicodeString_characterSet,
			pDBInfo->db_unicodeString_collation, pDBInfo->db_mysqlTLS);
	}
	else if (strcmp(pDBInfo->db_type, "redis") == 0)
	{
		dbinterface = new DBInterfaceRedis(name.c_str());
	}
	else if (strcmp(pDBInfo->db_type, "mongodb") == 0)
	{
		dbinterface = new DBInterfaceMongodb(name.c_str());
	}
	else if (strcmp(pDBInfo->db_type, "postgresql") == 0)
	{
		dbinterface = new DBInterfacePostgresql(name.c_str(), pDBInfo->db_unicodeString_characterSet,
			pDBInfo->db_unicodeString_collation);
	}

	if(dbinterface == NULL)
	{
		ERROR_MSG(fmt::format("DBUtil::createInterface: create db_interface error! type={}\n",
			pDBInfo->db_type));

		return NULL;
	}
	
	kbe_snprintf(dbinterface->db_type_, MAX_BUF, "%s", pDBInfo->db_type);
	dbinterface->db_port_ = pDBInfo->db_port;
	kbe_snprintf(dbinterface->db_ip_, MAX_IP, "%s", pDBInfo->db_ip);
	kbe_snprintf(dbinterface->db_username_, MAX_BUF, "%s", pDBInfo->db_username);
	dbinterface->db_numConnections_ = pDBInfo->db_numConnections;
	kbe_snprintf(dbinterface->db_password_, MAX_BUF * 10, "%s", pDBInfo->db_password);
	kbe_snprintf(dbinterface->db_authSource_, MAX_BUF, "%s", pDBInfo->db_authSource);
	kbe_snprintf(dbinterface->db_replicaSet_, MAX_BUF, "%s", pDBInfo->db_replicaSet);

	if (!dbinterface->attach(pDBInfo->db_name))
	{
		ERROR_MSG(fmt::format("DBUtil::createInterface: attach to database failed!\n\tdbinterface={0:p}\n\targs={1}\n",
			(void*)&dbinterface, dbinterface->c_str()));

		delete dbinterface;
		return NULL;
	}
	else
	{
		if(showinfo)
		{
			INFO_MSG(fmt::format("DBUtil::createInterface[{0:p}]: {1}\n", (void*)&dbinterface, 
				dbinterface->c_str()));
		}
	}

	return dbinterface;
}

//-------------------------------------------------------------------------------------
const char* DBUtil::accountScriptName()
{
	ENGINE_COMPONENT_INFO& dbcfg = g_kbeSrvConfig.getDBMgr();
	return dbcfg.dbAccountEntityScriptType;
}

//-------------------------------------------------------------------------------------
bool DBUtil::initInterface(DBInterface* pdbi)
{
	DBInterfaceInfo* pDBInfo = g_kbeSrvConfig.dbInterface(pdbi->name());
	if (!pDBInfo)
	{
		ERROR_MSG(fmt::format("DBUtil::initInterface: not found dbInterface({})\n",
			pdbi->name()));

		return false;
	}

	if (strcmp(pDBInfo->db_type, "mysql") == 0)
	{
		DBInterfaceMysql::initInterface(pdbi);
	}
	else if (strcmp(pDBInfo->db_type, "redis") == 0)
	{
		DBInterfaceRedis::initInterface(pdbi);
	}
	else if (strcmp(pDBInfo->db_type, "mongodb") == 0)
	{
		DBInterfaceMongodb::initInterface(pdbi);
	}
	else if (strcmp(pDBInfo->db_type, "postgresql") == 0)
	{
		DBInterfacePostgresql::initInterface(pdbi);
	}
	
	thread::ThreadPool* pThreadPool = pThreadPoolMaps_[pdbi->name()];
	KBE_ASSERT(pThreadPool);

	if (!pThreadPool->isInitialize())
	{
		if (!pThreadPool->createThreadPool(pDBInfo->db_numConnections,
			pDBInfo->db_numConnections, pDBInfo->db_numConnections))
			return false;
	}

	EntityTables& entityTables = EntityTables::findByInterfaceName(pdbi->name());
	bool ret = entityTables.load(pdbi);

	if(ret)
	{
		ret = pdbi->checkEnvironment();
	}
	
	if(ret)
	{
		ret = pdbi->checkErrors();
	}

	if (ret)
	{
		if (!pDBInfo->isPure)
			ret = entityTables.syncToDB(pdbi);
	}

	return ret;
}

//-------------------------------------------------------------------------------------
}
