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

#ifndef KBE_POSTGRESQL_KBE_TABLE_H
#define KBE_POSTGRESQL_KBE_TABLE_H

#include "db_interface/kbe_tables.h"

namespace KBEngine {

class KBEEntityLogTablePostgresql : public KBEEntityLogTable
{
public:
	KBEEntityLogTablePostgresql(EntityTables* pEntityTables);
	virtual bool syncToDB(DBInterface* pdbi);
	virtual bool syncIndexToDB(DBInterface* pdbi) { return true; }
	virtual bool logEntity(DBInterface* pdbi, const char* ip, uint32 port, DBID dbid,
		COMPONENT_ID componentID, ENTITY_ID entityID, ENTITY_SCRIPT_UID entityType);
	virtual bool queryEntity(DBInterface* pdbi, DBID dbid, EntityLog& entitylog, ENTITY_SCRIPT_UID entityType);
	virtual bool eraseEntityLog(DBInterface* pdbi, DBID dbid, ENTITY_SCRIPT_UID entityType);
	virtual bool eraseBaseappEntityLog(DBInterface* pdbi, COMPONENT_ID componentID);
};

class KBEServerLogTablePostgresql : public KBEServerLogTable
{
public:
	KBEServerLogTablePostgresql(EntityTables* pEntityTables);
	virtual bool syncToDB(DBInterface* pdbi);
	virtual bool syncIndexToDB(DBInterface* pdbi) { return true; }
	virtual bool updateServer(DBInterface* pdbi);
	virtual bool queryServer(DBInterface* pdbi, ServerLog& serverlog);
	virtual std::vector<COMPONENT_ID> queryServers(DBInterface* pdbi);
	virtual std::vector<COMPONENT_ID> queryTimeOutServers(DBInterface* pdbi);
	virtual bool clearServers(DBInterface* pdbi, const std::vector<COMPONENT_ID>& cids);
	virtual std::map<COMPONENT_ID, bool> queryAllServerShareDBState(DBInterface* pdbi);
	virtual int isShareDB(DBInterface* pdbi);
};

class KBEAccountTablePostgresql : public KBEAccountTable
{
public:
	KBEAccountTablePostgresql(EntityTables* pEntityTables);
	virtual bool syncToDB(DBInterface* pdbi);
	virtual bool syncIndexToDB(DBInterface* pdbi) { return true; }
	virtual bool queryAccount(DBInterface* pdbi, const std::string& name, ACCOUNT_INFOS& info);
	virtual bool queryAccountAllInfos(DBInterface* pdbi, const std::string& name, ACCOUNT_INFOS& info);
	virtual bool logAccount(DBInterface* pdbi, ACCOUNT_INFOS& info);
	virtual bool setFlagsDeadline(DBInterface* pdbi, const std::string& name, uint32 flags, uint64 deadline);
	virtual bool updateCount(DBInterface* pdbi, const std::string& name, DBID dbid);
	virtual bool updatePassword(DBInterface* pdbi, const std::string& name, const std::string& password);
};

class KBEEmailVerificationTablePostgresql : public KBEEmailVerificationTable
{
public:
	KBEEmailVerificationTablePostgresql(EntityTables* pEntityTables);
	virtual bool syncToDB(DBInterface* pdbi);
	virtual bool syncIndexToDB(DBInterface* pdbi) { return true; }
	virtual bool queryAccount(DBInterface* pdbi, int8 type, const std::string& name, ACCOUNT_INFOS& info);
	virtual bool logAccount(DBInterface* pdbi, int8 type, const std::string& name, const std::string& datas, const std::string& code);
	virtual bool delAccount(DBInterface* pdbi, int8 type, const std::string& name);
	virtual bool activateAccount(DBInterface* pdbi, const std::string& code, ACCOUNT_INFOS& info);
	virtual bool bindEMail(DBInterface* pdbi, const std::string& name, const std::string& code);
	virtual bool resetpassword(DBInterface* pdbi, const std::string& name, const std::string& password, const std::string& code);
};

}

#endif // KBE_POSTGRESQL_KBE_TABLE_H
