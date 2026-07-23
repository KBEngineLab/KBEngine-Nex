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

#ifndef KBE_READ_ENTITY_HELPER_H
#define KBE_READ_ENTITY_HELPER_H

// common include	
// #define NDEBUG
#include <sstream>
#include "common.h"
#include "sqlstatement.h"
#include "entity_sqlstatement_mapping.h"
#include "common/common.h"
#include "common/memorystream.h"
#include "helper/debug_helper.h"
#include "db_interface/db_interface.h"
#include "db_interface/entity_table.h"
#include "db_interface_mysql.h"

namespace KBEngine{ 

class ReadEntityHelper
{
public:
	ReadEntityHelper()
	{
	}

	virtual ~ReadEntityHelper()
	{
	}

	/**
		�ӱ��в�ѯ����
	*/
	static bool queryDB(DBInterface* pdbi, mysql::DBContext& context)
	{
		// ����ĳ��dbid���һ�ű��ϵ��������
		SqlStatement* pSqlcmd = new SqlStatementQuery(pdbi, context.tableName, 
			context.dbids[context.dbid], 
			context.dbid, context.items);

		bool ret = pSqlcmd->query();
		context.dbid = pSqlcmd->dbid();
		delete pSqlcmd;
		
		if(!ret)
			return ret;

		// ����ѯ���Ľ��д��������
		MYSQL_RES * pResult = mysql_store_result(static_cast<DBInterfaceMysql*>(pdbi)->mysql());

		if(pResult)
		{
			MYSQL_ROW arow;

			while((arow = mysql_fetch_row(pResult)) != NULL)
			{
				uint32 nfields = (uint32)mysql_num_fields(pResult);
				if(nfields <= 0)
					continue;

				unsigned long *lengths = mysql_fetch_lengths(pResult);

				// ��ѯ���֤�˲�ѯ����ÿ����¼������dbid
				std::stringstream sval;
				sval << arow[0];

				DBID item_dbid;
				sval >> item_dbid;

				// ��dbid��¼���б��У������ǰ���������ӱ��������ȥ�ӱ���ÿһ�����dbid��صļ�¼
				std::vector<DBID>& itemDBIDs = context.dbids[context.dbid];
				int fidx = -100;

				// �����ǰ���item��dbidС�ڸñ������һ����¼��dbid��С����ô��Ҫ��itemDBIDs��ָ����λ�ò������dbid���Ա�֤��С�����˳��
				if (itemDBIDs.size() > 0 && itemDBIDs[itemDBIDs.size() - 1] > item_dbid)
				{
					for (fidx = itemDBIDs.size() - 1; fidx > 0; --fidx)
					{
						if (itemDBIDs[fidx] < item_dbid)
							break;
					}

					itemDBIDs.insert(itemDBIDs.begin() + fidx, item_dbid);
				}
				else
				{
					itemDBIDs.push_back(item_dbid);
				}

				// ���������¼����dbid���⻹�����������ݣ���������䵽�������
				if(nfields > 1)
				{
					std::vector<std::string>& itemResults = context.results[item_dbid].second;
					context.results[item_dbid].first = 0;

					KBE_ASSERT(nfields == context.items.size() + 1);

					for (uint32 i = 1; i < nfields; ++i)
					{
						KBEShared_ptr<mysql::DBContext::DB_ITEM_DATA> pSotvs = context.items[i - 1];
						std::string data;
						data.assign(arow[i], lengths[i]);

						// �������������dbidʱ�ǲ��뷽ʽ����ô�������Ҳ��Ҫ���뵽��Ӧ��λ��
						if (fidx != -100)
							itemResults.insert(itemResults.begin() + fidx++, data);
						else
							itemResults.push_back(data);
					}
				}
			}

			mysql_free_result(pResult);
		}
		
		std::vector<DBID>& dbids = context.dbids[context.dbid];

		// ���û���������ѯ�����
		if(dbids.size() == 0)
			return true;

		// �����ǰ�������ӱ���������Ҫ������ѯ�ӱ�
		// ÿһ��dbid����Ҫ����ӱ��ϵ�����
		// �������������ӱ�һ�β�ѯ�����е�dbids����Ȼ����䵽�����

		mysql::DBContext::DB_RW_CONTEXTS::iterator iter1 = context.optable.begin();
		for(; iter1 != context.optable.end(); ++iter1)
		{
			mysql::DBContext& wbox = *iter1->second.get();
			if(!queryChildDB(pdbi, wbox, dbids))
				return false;
		}

		return ret;
	}


	/**
		���ӱ��в�ѯ����
	*/
	static bool queryChildDB(DBInterface* pdbi, mysql::DBContext& context, std::vector<DBID>& parentTableDBIDs)
	{
		// ����ĳ��dbid���һ�ű��ϵ��������
		SqlStatement* pSqlcmd = new SqlStatementQuery(pdbi, context.tableName, 
			parentTableDBIDs, 
			context.dbid, context.items);

		bool ret = pSqlcmd->query();
		context.dbid = pSqlcmd->dbid();
		delete pSqlcmd;
		
		if(!ret)
			return ret;

		std::vector<DBID> t_parentTableDBIDs;

		// ����ѯ���Ľ��д��������
		MYSQL_RES * pResult = mysql_store_result(static_cast<DBInterfaceMysql*>(pdbi)->mysql());

		if(pResult)
		{
			MYSQL_ROW arow;

			while((arow = mysql_fetch_row(pResult)) != NULL)
			{
				uint32 nfields = (uint32)mysql_num_fields(pResult);
				if(nfields <= 0)
					continue;

				unsigned long *lengths = mysql_fetch_lengths(pResult);

				// ��ѯ���֤�˲�ѯ����ÿ����¼������dbid
				std::stringstream sval;
				sval << arow[0];

				DBID item_dbid;
				sval >> item_dbid;

				sval.clear();
				sval << arow[1];

				DBID parentID;
				sval >> parentID;

				// ��dbid��¼���б��У������ǰ���������ӱ��������ȥ�ӱ���ÿһ�����dbid��صļ�¼
				std::vector<DBID>& itemDBIDs = context.dbids[parentID];
				int fidx = -100;

				// �����ǰ���item��dbidС�ڸñ������һ����¼��dbid��С����ô��Ҫ��itemDBIDs��ָ����λ�ò������dbid���Ա�֤��С�����˳��
				if (itemDBIDs.size() > 0 && itemDBIDs[itemDBIDs.size() - 1] > item_dbid)
				{
					for (fidx = itemDBIDs.size() - 1; fidx > 0; --fidx)
					{
						if (itemDBIDs[fidx] < item_dbid)
							break;
					}

					itemDBIDs.insert(itemDBIDs.begin() + fidx, item_dbid);
					t_parentTableDBIDs.insert(t_parentTableDBIDs.begin() + t_parentTableDBIDs.size() - (itemDBIDs.size() - fidx - 1), item_dbid);
				}
				else
				{
					itemDBIDs.push_back(item_dbid);
					t_parentTableDBIDs.push_back(item_dbid);
				}

				// ���������¼����dbid���⻹�����������ݣ���������䵽�������
				const uint32 const_fields = 2; // id, parentID
				if(nfields > const_fields)
				{
					std::vector<std::string>& itemResults = context.results[item_dbid].second;
					context.results[item_dbid].first = 0;

					KBE_ASSERT(nfields == context.items.size() + const_fields);

					for (uint32 i = const_fields; i < nfields; ++i)
					{
						KBEShared_ptr<mysql::DBContext::DB_ITEM_DATA> pSotvs = context.items[i - const_fields];
						std::string data;
						data.assign(arow[i], lengths[i]);

						// �����ǰ���item��dbid���ڸñ������м�¼����dbid��С����ô��Ҫ��itemDBIDs��ָ����λ�ò������dbid���Ա�֤��С�����˳��
						if (fidx != -100)
							itemResults.insert(itemResults.begin() + fidx++, data);
						else
							itemResults.push_back(data);
					}
				}
			}

			mysql_free_result(pResult);
		}

		// ���û���������ѯ�����
		if(t_parentTableDBIDs.size() == 0)
			return true;

		// �����ǰ�������ӱ���������Ҫ������ѯ�ӱ�
		// ÿһ��dbid����Ҫ����ӱ��ϵ�����
		// �������������ӱ�һ�β�ѯ�����е�dbids����Ȼ����䵽�����
		mysql::DBContext::DB_RW_CONTEXTS::iterator iter1 = context.optable.begin();
		for(; iter1 != context.optable.end(); ++iter1)
		{
			mysql::DBContext& wbox = *iter1->second.get();

			if(!queryChildDB(pdbi, wbox, t_parentTableDBIDs))
				return false;
		}

		return ret;
	}

protected:
};

}
#endif // KBE_READ_ENTITY_HELPER_H
