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

#ifndef KBE_DB_INTERFACE_REDIS_H
#define KBE_DB_INTERFACE_REDIS_H

#include "common.h"
#include "common/common.h"
#include "common/singleton.h"
#include "common/memorystream.h"
#include "helper/debug_helper.h"
#include "db_interface/db_interface.h"

#include "hiredis/hiredis.h"
// Windows 最终链接由集中式 vcpkg MSBuild 配置选择当前构建类型的 hiredis 库，避免头文件固化端口的库文件命名规则。
// The centralized vcpkg MSBuild integration selects the hiredis library for the active Windows configuration, keeping port-specific library names out of public headers.

namespace KBEngine { 

class DBException;

/*
	数据库接口
	tbl_Account_Auto_increment = uint64(1)
	tbl_Account:1 = hashes(name, password, xxx)
	tbl_Account:2 = hashes(name, password, xxx)
	tbl_Account:3 = hashes(name, password, xxx(array))

	// array of type
	tbl_Account_xxx_values:3:size = uint64(3)
	tbl_Account_xxx_values:3:1 = val
	tbl_Account_xxx_values:3:2 = val
	tbl_Account_xxx_values:3:3 = val	
*/
class DBInterfaceRedis : public DBInterface
{
public:
	DBInterfaceRedis(const char* name);
	virtual ~DBInterfaceRedis();

	static bool initInterface(DBInterface* pdbi);
	
	bool ping(redisContext* pRedisContext = NULL);
	
	void inTransaction(bool value)
	{
		KBE_ASSERT(inTransaction_ != value);
		inTransaction_ = value;
	}

	redisContext* context()				{ return pRedisContext_; }
	
	bool hasLostConnection() const		{ return hasLostConnection_; }
	void hasLostConnection( bool v )	{ hasLostConnection_ = v; }
	
	/**
		检查环境
	*/
	virtual bool checkEnvironment();
	
	/**
		检查错误， 对错误的内容进行纠正
		如果纠正不成功返回失败
	*/
	virtual bool checkErrors();

	/**
		与某个数据库关联
	*/
	bool reattach();
	virtual bool attach(const char* databaseName = NULL);
	virtual bool detach();

	/**
		获取数据库所有的表名
	*/
	virtual bool getTableNames( std::vector<std::string>& tableNames, const char * pattern);

	/**
		获取数据库某个表所有的字段名称
	*/
	virtual bool getTableItemNames(const char* tableName, std::vector<std::string>& itemNames);

	/**
		查询表
	*/
	virtual bool query(const char* cmd, size_t size, bool printlog = true, MemoryStream * result = NULL);
	bool query(const std::string& cmd, redisReply** pRedisReply, bool printlog = true);
	bool query(bool printlog, const char* format, ...);
	bool queryAppend(bool printlog, const char* format, ...);
	bool getQueryReply(redisReply **pRedisReply);
	
	void write_query_result(redisReply* pRedisReply, MemoryStream * result);
	void write_query_result_element(redisReply* pRedisReply, MemoryStream * result);
		
	/**
		返回这个接口的描述
	*/
	virtual const char* c_str();

	/** 
		获取错误
	*/
	virtual const char* getstrerror();

	/** 
		获取错误编号
	*/
	virtual int getlasterror();

	/**
		创建一个entity存储表
	*/
	virtual EntityTable* createEntityTable(EntityTables* pEntityTables);

	/** 
		从数据库删除entity表
	*/
	virtual bool dropEntityTableFromDB(const char* tableName);
	
	/** 
		从数据库删除entity表字段
	*/
	virtual bool dropEntityTableItemFromDB(const char* tableName, const char* tableItemName);

	/**
		锁住接口操作
	*/
	virtual bool lock();
	virtual DBTransactionResult unlock();
	virtual bool rollback();

	void throwError(DBException* pDBException);
	
	/**
		处理异常
	*/
	virtual bool processException(std::exception & e);
	
protected:
	redisContext* pRedisContext_;
	bool hasLostConnection_;
	bool inTransaction_;	
};


}

#endif // KBE_DB_INTERFACE_REDIS_H
