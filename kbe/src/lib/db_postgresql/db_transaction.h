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

#ifndef KBE_DB_POSTGRESQL_TRANSACTION_H
#define KBE_DB_POSTGRESQL_TRANSACTION_H

#include "common/common.h"

namespace KBEngine {

class DBInterface;

namespace postgresql {

/*
	PostgreSQL 事务守卫。
	PostgreSQL transaction guard.
	实体主表、ARRAY 子表、ENTITY_COMPONENT 子表会一起写，失败时必须把已经写入的部分回滚掉。
	Entity rows and ARRAY or ENTITY_COMPONENT child rows are written together, so failures must roll back every partial write.
*/
class DBTransaction
{
public:
	DBTransaction(DBInterface* pdbi, bool autostart = true);
	~DBTransaction();

	bool start();
	bool commit();
	bool active() const { return active_; }
	bool committed() const { return committed_; }

private:
	DBInterface* pdbi_;
	bool active_;
	bool committed_;
	bool autostart_;
	bool ownsTransaction_;
};

}

}

#endif // KBE_DB_POSTGRESQL_TRANSACTION_H
