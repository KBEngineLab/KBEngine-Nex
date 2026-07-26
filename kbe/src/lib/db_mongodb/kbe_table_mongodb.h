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
#include "common/common.h"
#include "common/singleton.h"
#include "helper/debug_helper.h"
#include "db_interface/entity_table.h"
#include "db_interface/kbe_tables.h"

namespace KBEngine {
	/*
	kbe系统表
	KBEngine system-table adapters backed by MongoDB collections.
	*/
	class KBEEntityLogTableMongodb : public KBEEntityLogTable
	{
	public:
		KBEEntityLogTableMongodb(EntityTables* pEntityTables);
		~KBEEntityLogTableMongodb() override = default;

		/**
		同步表到数据库中
		Synchronize the system collection and its indexes to MongoDB.
		*/
		bool syncToDB(DBInterface* pdbi) override;
		bool syncIndexToDB(DBInterface* pdbi) override { return true; }

		bool logEntity(DBInterface* pdbi, const char* ip, uint32 port, DBID dbid,
		               COMPONENT_ID componentID, ENTITY_ID entityID, ENTITY_SCRIPT_UID entityType) override;

		bool queryEntity(DBInterface* pdbi, DBID dbid, EntityLog& entitylog, ENTITY_SCRIPT_UID entityType) override;

		bool eraseEntityLog(DBInterface* pdbi, DBID dbid, ENTITY_SCRIPT_UID entityType) override;
		bool eraseBaseappEntityLog(DBInterface* pdbi, COMPONENT_ID componentID) override;

	protected:

	};

	class KBEServerLogTableMongodb : public KBEServerLogTable
	{
	public:
		KBEServerLogTableMongodb(EntityTables* pEntityTables);
		~KBEServerLogTableMongodb() override = default;

		/**
		同步表到数据库中
		Synchronize the system collection and its indexes to MongoDB.
		*/
		bool syncToDB(DBInterface* pdbi) override;
		bool syncIndexToDB(DBInterface* pdbi) override { return true; }

		bool updateServer(DBInterface* pdbi) override;

		bool queryServer(DBInterface* pdbi, ServerLog& serverlog) override;
		std::vector<COMPONENT_ID> queryServers(DBInterface* pdbi) override;

		std::vector<COMPONENT_ID> queryTimeOutServers(DBInterface* pdbi) override;

		bool clearServers(DBInterface* pdbi, const std::vector<COMPONENT_ID>& cids) override;

		std::map<COMPONENT_ID, bool> queryAllServerShareDBState(DBInterface* pdbi);
		int isShareDB(DBInterface* pdbi);

	protected:

	};

	class KBEAccountTableMongodb : public KBEAccountTable
	{
	public:
		KBEAccountTableMongodb(EntityTables* pEntityTables);
		~KBEAccountTableMongodb() override = default;

		/**
		同步表到数据库中
		Synchronize the system collection and its indexes to MongoDB.
		*/
		bool syncToDB(DBInterface* pdbi) override;
		bool syncIndexToDB(DBInterface* pdbi) override { return true; }

		bool queryAccount(DBInterface* pdbi, const std::string& name, ACCOUNT_INFOS& info) override;
		bool queryAccountAllInfos(DBInterface* pdbi, const std::string& name, ACCOUNT_INFOS& info) override;
		bool logAccount(DBInterface* pdbi, ACCOUNT_INFOS& info) override;
		bool setFlagsDeadline(DBInterface* pdbi, const std::string& name, uint32 flags, uint64 deadline) override;
		bool updateCount(DBInterface* pdbi, const std::string& name, DBID dbid) override;
		bool updatePassword(DBInterface* pdbi, const std::string& name, const std::string& password) override;
	protected:
	};

	class KBEEmailVerificationTableMongodb : public KBEEmailVerificationTable
	{
	public:

		KBEEmailVerificationTableMongodb(EntityTables* pEntityTables);
		~KBEEmailVerificationTableMongodb() override;

		/**
		同步表到数据库中
		Synchronize the system collection and its indexes to MongoDB.
		*/
		bool syncToDB(DBInterface* pdbi) override;
		bool syncIndexToDB(DBInterface* pdbi) override { return true; }

		bool queryAccount(DBInterface* pdbi, int8 type, const std::string& name, ACCOUNT_INFOS& info) override;
		bool logAccount(DBInterface* pdbi, int8 type, const std::string& name, const std::string& datas, const std::string& code) override;
		bool delAccount(DBInterface* pdbi, int8 type, const std::string& name) override;
		bool activateAccount(DBInterface* pdbi, const std::string& code, ACCOUNT_INFOS& info) override;
		bool bindEMail(DBInterface* pdbi, const std::string& name, const std::string& code) override;
		bool resetpassword(DBInterface* pdbi, const std::string& name,
		                   const std::string& password, const std::string& code) override;

	protected:
	};

}
