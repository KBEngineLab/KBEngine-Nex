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

#include "db_interface_mongodb.h"
#include "db_transaction.h"
#include "db_exception.h"
#include "db_interface/db_interface.h"
#include "helper/debug_helper.h"
#include "common/timestamp.h"

namespace KBEngine {
	namespace mongodb {

		DBTransaction::DBTransaction(DBInterface* pdbi, bool autostart) :
			pdbi_(pdbi),
			active_(false),
			committed_(false),
			autostart_(autostart),
			ownsTransaction_(false)
		{
			if (autostart)
				start();
		}

		DBTransaction::~DBTransaction()
		{
			if (autostart_ && active_ && !committed_)
				end();
		}

		bool DBTransaction::start()
		{
			if (active_)
				return true;

			DBInterfaceMongodb* pMongodb = static_cast<DBInterfaceMongodb*>(pdbi_);
			if (pMongodb->inTransaction())
			{
				// 嵌套事务守卫加入外层事务，只有外层拥有提交和回滚责任。
				// Nested transaction guards join the outer transaction, which alone owns commit and rollback.
				active_ = true;
				ownsTransaction_ = false;
				committed_ = false;
				return true;
			}

			committed_ = false;
			active_ = pMongodb->beginTransaction();
			ownsTransaction_ = active_ && pMongodb->inTransaction();
			return active_;
		}

		void DBTransaction::end()
		{
			if (active_ && ownsTransaction_ && !committed_ &&
				!static_cast<DBInterfaceMongodb*>(pdbi_)->hasLostConnection())
			{
				// 析构路径只做尽力回滚，错误由原始数据库操作报告，不能从析构函数继续抛出。
				// The destructor performs best-effort rollback only; the original operation reports the error and no exception escapes destruction.
				static_cast<DBInterfaceMongodb*>(pdbi_)->abortTransaction();
			}

			active_ = false;
			ownsTransaction_ = false;
		}

		DBTransactionResult DBTransaction::commit()
		{
			KBE_ASSERT(active_ && !committed_);

			if (!ownsTransaction_)
			{
				committed_ = true;
				active_ = false;
				return DB_TRANSACTION_COMMITTED;
			}

			uint64 startTime = timestamp();
			DBTransactionResult result = static_cast<DBInterfaceMongodb*>(pdbi_)->commitTransaction();
			if (result != DB_TRANSACTION_COMMITTED)
				return result;

			uint64 duration = timestamp() - startTime;
			if (duration > stampsPerSecond() * 0.2f)
			{
				WARNING_MSG(fmt::format("DBTransaction::commit(): took {:.2f} seconds\n",
					(double(duration) / stampsPerSecondD())));
			}

			committed_ = true;
			active_ = false;
			ownsTransaction_ = false;
			return DB_TRANSACTION_COMMITTED;
		}

		bool DBTransaction::shouldRetry() const
		{
			return false;
		}

	}
}
