/*
This source file is part of KBEngine
For the latest info, see http://www.kbengine.org/

Copyright (c) 2008-2018 KBEngine.

KBEngine is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/

#ifndef KBE_DB_TRANSACTION_RESULT_H
#define KBE_DB_TRANSACTION_RESULT_H

namespace KBEngine {

// 数据库提交结果必须区分确定失败与结果未知；只有确定失败才允许上层按幂等策略重试整个事务。
// Database commit outcomes distinguish definite failure from uncertainty; only definite failure may permit a whole-transaction retry under an idempotency policy.
enum DBTransactionResult
{
	DB_TRANSACTION_COMMITTED = 0,
	DB_TRANSACTION_NOT_COMMITTED = 1,
	DB_TRANSACTION_UNKNOWN = 2
};

inline const char* dbTransactionResultName(DBTransactionResult result)
{
	switch (result)
	{
	case DB_TRANSACTION_COMMITTED:
		return "COMMITTED";
	case DB_TRANSACTION_NOT_COMMITTED:
		return "NOT_COMMITTED";
	case DB_TRANSACTION_UNKNOWN:
		return "UNKNOWN";
	default:
		return "INVALID";
	}
}

}

#endif // KBE_DB_TRANSACTION_RESULT_H
