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

#ifndef KBE_DB_INTERFACE_H
#define KBE_DB_INTERFACE_H

#include "common/common.h"
#include "common/singleton.h"
#include "helper/debug_helper.h"
#include "db_interface/entity_table.h"
#include "server/serverconfig.h"

namespace KBEngine { 

namespace thread
{
class ThreadPool;
}

class DBUtil;

/*
	���ݿ�ӿ�
*/
class DBInterface
{
public:
	enum DB_OP_STATE
	{
		DB_OP_READ,
		DB_OP_WRITE,
	};

	friend class DBUtil;

	DBInterface(const char* name) :
	db_port_(3306),
	db_numConnections_(1),
	lastquery_()
	{
		strncpy(name_, name, MAX_NAME - 1);
		int dbIndex = g_kbeSrvConfig.dbInterfaceName2dbInterfaceIndex(this->name());
		KBE_ASSERT(dbIndex >= 0);
		dbIndex_ = dbIndex;
	};

	virtual ~DBInterface()
	{
	};

	/**
		��黷��
	*/
	virtual bool checkEnvironment() = 0;
	
	/**
		������ �Դ�������ݽ��о���
		����������ɹ�����ʧ��
	*/
	virtual bool checkErrors() = 0;

	/**
		��ĳ�����ݿ����
	*/
	virtual bool attach(const char* databaseName = NULL) = 0;
	virtual bool detach() = 0;

	/**
		��ȡ���ݿ����еı���
	*/
	virtual bool getTableNames( std::vector<std::string>& tableNames, const char * pattern) = 0;

	/**
		��ȡ���ݿ�ĳ�������е��ֶ�����
	*/
	virtual bool getTableItemNames(const char* tableName, std::vector<std::string>& itemNames) = 0;

	/**
		��ѯ��
	*/
	virtual bool query(const char* cmd, uint32 size, bool printlog = true, MemoryStream * result = NULL) = 0;
	virtual bool query(const std::string& cmd, bool printlog = true, MemoryStream * result = NULL)
	{
		return query(cmd.c_str(), (uint32)cmd.size(), printlog, result);
	}

	/**
		��������ӿڵ�����
	*/
	const char* name() const { return name_; }

	/**
		��������ӿڵ�����
	*/
	uint16 dbIndex() const { return dbIndex_; }

	/**
		��������ӿڵ�����
	*/
	virtual const char* c_str() = 0;

	/** 
		��ȡ����
	*/
	virtual const char* getstrerror() = 0;

	/** 
		��ȡ������
	*/
	virtual int getlasterror() = 0;

	/**
		����һ��entity�洢��
	*/
	virtual EntityTable* createEntityTable(EntityTables* pEntityTables) = 0;

	/** 
		�����ݿ�ɾ��entity��
	*/
	virtual bool dropEntityTableFromDB(const char* tableName) = 0;

	/** 
		�����ݿ�ɾ��entity���ֶ�
	*/
	virtual bool dropEntityTableItemFromDB(const char* tableName, const char* tableItemName) = 0;

	/**
		��ס�ӿڲ���
	*/
	virtual bool lock() = 0;
	virtual bool unlock() = 0;

	/**
		�����쳣
	*/
	virtual bool processException(std::exception & e) = 0;

	/**
		��ȡ���һ�β�ѯ��sql���
	*/
	virtual const std::string& lastquery() const{ return lastquery_; }

protected:
	char name_[MAX_BUF];									// ���ݿ�ӿڵ�����
	char db_type_[MAX_BUF];									// ���ݿ�����
	uint32 db_port_;										// ���ݿ�Ķ˿�
	char db_ip_[MAX_IP];									// ���ݿ��ip��ַ
	char db_username_[MAX_BUF];								// ���ݿ���û���
	char db_password_[MAX_BUF * 10];						// ���ݿ������
	char db_name_[MAX_BUF];									// ���ݿ���
	uint16 db_numConnections_;								// ���ݿ��������
	std::string lastquery_;									// ���һ�β�ѯ����
	uint16 dbIndex_;										// ��Ӧ�����ݿ�ӿ�����
};

/*
	���ݿ������Ԫ
*/
class DBUtil : public Singleton<DBUtil>
{
public:
	DBUtil();
	~DBUtil();
	
	static bool initialize();
	static void finalise();
	static bool initializeWatcher();

	static bool initThread(const std::string& dbinterfaceName);
	static bool finiThread(const std::string& dbinterfaceName);

	static DBInterface* createInterface(const std::string& name, bool showinfo = true);
	static const char* accountScriptName();
	static bool initInterface(DBInterface* pdbi);

	static void handleMainTick();

	typedef KBEUnordered_map<std::string, thread::ThreadPool*> DBThreadPoolMap;
	static thread::ThreadPool* pThreadPool(const std::string& name)
	{ 
		DBThreadPoolMap::iterator iter = pThreadPoolMaps_.find(name);
		if (iter != pThreadPoolMaps_.end())
			return iter->second;

		return NULL;
	}

private:
	static DBThreadPoolMap pThreadPoolMaps_;
};

}

#endif // KBE_DB_INTERFACE_H
