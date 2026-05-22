// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved.

#include "db_transaction.h"
#include "db_interface_postgresql.h"
#include "db_interface/db_interface.h"
#include "helper/debug_helper.h"

namespace KBEngine {
namespace postgresql {

DBTransaction::DBTransaction(DBInterface* pdbi, bool autostart) :
	pdbi_(pdbi),
	active_(false),
	committed_(false),
	autostart_(autostart),
	ownsTransaction_(false)
{
	if (autostart_)
		start();
}

DBTransaction::~DBTransaction()
{
	if (!autostart_ || !active_ || committed_ || !ownsTransaction_)
		return;

	// 析构里只兜底回滚，不抛异常；上层已经根据失败的 SQL 返回错误。
	try
	{
		if (!pdbi_->query(std::string("ROLLBACK"), false))
		{
			WARNING_MSG(fmt::format("postgresql::DBTransaction::~DBTransaction: rollback failed, error={}\n",
				pdbi_->getstrerror()));
		}
	}
	catch (std::exception& e)
	{
		WARNING_MSG(fmt::format("postgresql::DBTransaction::~DBTransaction: rollback exception, error={}\n",
			e.what()));
	}

	static_cast<DBInterfacePostgresql*>(pdbi_)->inTransaction(false);
	active_ = false;
}

bool DBTransaction::start()
{
	if (active_)
		return true;

	DBInterfacePostgresql* pPostgresql = static_cast<DBInterfacePostgresql*>(pdbi_);
	if (pPostgresql->inTransaction())
	{
		// 已经在外层事务里时只加入当前事务，提交和回滚仍交给外层。
		active_ = true;
		ownsTransaction_ = false;
		committed_ = false;
		return true;
	}

	committed_ = false;
	active_ = pdbi_->query(std::string("BEGIN"), false);
	if (active_)
	{
		ownsTransaction_ = true;
		pPostgresql->inTransaction(true);
	}

	return active_;
}

bool DBTransaction::commit()
{
	KBE_ASSERT(active_ && !committed_);

	if (!ownsTransaction_)
	{
		committed_ = true;
		active_ = false;
		return true;
	}

	if (!pdbi_->query(std::string("COMMIT"), false))
		return false;

	committed_ = true;
	active_ = false;
	ownsTransaction_ = false;
	static_cast<DBInterfacePostgresql*>(pdbi_)->inTransaction(false);
	return true;
}

}
}
