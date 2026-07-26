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
#include "common/common.h"
#include "common/memorystream.h"
#include "helper/debug_helper.h"

namespace KBEngine {
	namespace mongodb {
		class DBContext
		{
		public:
		/**
			存储所有要操作的表item结构
			Store per-item contexts needed while reading or writing one entity document.
		*/
			struct DB_ITEM_DATA
			{
				char sqlval[MAX_BUF];
				const char* sqlkey;
				std::string extraDatas;
			};

			typedef std::vector< std::pair< std::string, KBEShared_ptr< DBContext > > > DB_RW_CONTEXTS;
			typedef std::vector< KBEShared_ptr<DB_ITEM_DATA>  > DB_ITEM_DATAS;

			DBContext()
			{
			}

			~DBContext()
			{
			}

			DB_ITEM_DATAS items;

			std::string tableName;
			std::string parentTableName;

			DBID parentTableDBID;
			DBID dbid;

			DB_RW_CONTEXTS optable;

			bool isEmpty;

			std::map<DBID, std::vector<DBID> > dbids;
			std::vector< std::string >results;
			std::vector< std::string >::size_type readresultIdx;

		private:

		};

	}
}
