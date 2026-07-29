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

#ifndef KBE_DB_INTERFACE_MYSQL_H
#define KBE_DB_INTERFACE_MYSQL_H

#include "common.h"
#include "db_transaction.h"
#include "common/common.h"
#include "common/singleton.h"
#include "common/memorystream.h"
#include "helper/debug_helper.h"
#include "db_interface/db_interface.h"

#include "mysql/mysql.h"
#include <limits>
#include <stdexcept>
// Windows构建由vcpkg的libmariadb端口提供头文件和自动链接信息，避免再次绑定仓库内的旧VS140库。
// Windows builds obtain headers and auto-link metadata from the vcpkg libmariadb port to avoid rebinding the legacy VS140 libraries.

namespace KBEngine { 

// MariaDB/MySQL在Windows上使用32位unsigned long表示输入长度，该重载在唯一的原生边界完成校验。
// MariaDB/MySQL uses a 32-bit unsigned long length on Windows; this overload validates once at the native boundary.
inline unsigned long mysql_real_escape_string(MYSQL* mysql, char* destination, const char* source, size_t length)
{
	if(length > static_cast<size_t>(std::numeric_limits<unsigned long>::max()))
		throw std::length_error("mysql_real_escape_string input exceeds unsigned long");

	return ::mysql_real_escape_string(mysql, destination, source, static_cast<unsigned long>(length));
}

struct MYSQL_TABLE_FIELD
{
	std::string name;
	int32 length;
	// 字符字段的声明长度独立于字节长度，避免 utf8mb4 下按固定倍率反推。
	// The declared character length is stored separately from byte length so utf8mb4 never relies on a fixed multiplier.
	int32 char_length;
	uint64 maxlength;
	unsigned int flags;
	enum_field_types type;
};

class DBException;

/*
	数据库接口
*/
class DBInterfaceMysql : public DBInterface
{
public:
	DBInterfaceMysql(const char* name, std::string characterSet, std::string collation,
		const DBMysqlTLSInfo& tlsInfo);
	virtual ~DBInterfaceMysql();

	static bool initInterface(DBInterface* pdbi);
	
	/**
		与某个数据库关联
	*/
	bool reattach();
	virtual bool attach(const char* databaseName = NULL);
	virtual bool detach();

	bool ping(){ 
		return mysql_ping(pMysql_) == 0; 
	}

	void inTransaction(bool value)
	{
		KBE_ASSERT(inTransaction_ != value);
		inTransaction_ = value;
	}

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

	virtual bool query(const char* strCommand, size_t size, bool printlog = true, MemoryStream * result = NULL);

	bool write_query_result(MemoryStream * result);

	/**
		获取数据库所有的表名
	*/
	virtual bool getTableNames( std::vector<std::string>& tableNames, const char * pattern);

	/**
		获取数据库某个表所有的字段名称
	*/
	virtual bool getTableItemNames(const char* tableName, std::vector<std::string>& itemNames);

	/** 
		从数据库删除entity表字段
	*/
	virtual bool dropEntityTableItemFromDB(const char* tableName, const char* tableItemName);

	MYSQL* mysql(){ return pMysql_; }

	void throwError(DBException* pDBException);

	my_ulonglong insertID()		{ return mysql_insert_id( pMysql_ ); }

	my_ulonglong affectedRows()	{ return mysql_affected_rows( pMysql_ ); }

	const char* info()			{ return mysql_info( pMysql_ ); }

	const char* getLastError()	
	{
		if(pMysql_ == NULL)
			return "pMysql is NULL";

		return mysql_error( pMysql_ ); 
	}

	unsigned int getLastErrorNum() { return mysql_errno( pMysql_ ); }

	typedef KBEUnordered_map<std::string, MYSQL_TABLE_FIELD> TABLE_FIELDS;
	void getFields(TABLE_FIELDS& outs, const char* tableName);

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
		如果数据库不存在则创建一个数据库
	*/
	virtual bool createDatabaseIfNotExist();
	
	/**
		创建一个entity存储表
	*/
	virtual EntityTable* createEntityTable(EntityTables* pEntityTables);

	/** 
		从数据库删除entity表
	*/
	virtual bool dropEntityTableFromDB(const char* tableName);

	/**
		锁住接口操作
	*/
	virtual bool lock();
	virtual DBTransactionResult unlock();
	virtual bool rollback();

	/**
		处理异常
	*/
	bool processException(std::exception & e);

	/**
		SQL命令最长大小
	*/
	static size_t sql_max_allowed_packet(){ return sql_max_allowed_packet_; }

protected:
	// 每个新MYSQL句柄都需要重新应用连接选项，包括数据库创建后的重连句柄。
	// Every new MYSQL handle must receive the connection options, including handles recreated while creating a missing database.
	bool configureConnectionOptions();

	MYSQL* pMysql_;

	bool hasLostConnection_;

	bool inTransaction_;

	mysql::DBTransaction lock_;

	std::string characterSet_;
	std::string collation_;

	// 连接对象保存配置快照，确保连接池线程和后续重连不依赖可变的全局配置。
	// Each connection stores a configuration snapshot so pooled threads and reconnects do not depend on mutable global configuration.
	DBMysqlTLSInfo tlsInfo_;

	static size_t sql_max_allowed_packet_;
};


}

#endif // KBE_DB_INTERFACE_MYSQL_H
