// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved.

#ifndef KBE_DB_POSTGRESQL_TRANSACTION_H
#define KBE_DB_POSTGRESQL_TRANSACTION_H

#include "common/common.h"

namespace KBEngine {

class DBInterface;

namespace postgresql {

/*
	PostgreSQL 事务守卫。
	实体主表、ARRAY 子表、ENTITY_COMPONENT 子表会一起写，失败时必须把已经写入的部分回滚掉。
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
