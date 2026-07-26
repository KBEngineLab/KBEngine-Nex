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

#ifndef KBE_MYSQL_EXCEPTION_H
#define KBE_MYSQL_EXCEPTION_H

#include <string>

namespace KBEngine { 

class DBInterface;
class DBException : public std::exception
{
public:
	DBException(DBInterface* pdbi);
	~DBException() throw();

	virtual const char * what() const throw() { return errStr_.c_str(); }

	bool shouldRetry() const;
	bool isLostConnection() const;

	// 暴露 Connector/C 错误码用于结构化诊断，不允许调用方修改异常状态。
	// Expose the Connector/C error code for structured diagnostics without allowing callers to mutate exception state.
	unsigned int errorNumber() const { return errNum_; }

	void setError(const std::string& errStr, unsigned int errNum)
	{
		errStr_ = errStr;
		errNum_ = errNum;
	}

private:
	std::string errStr_;
	unsigned int errNum_;
};

}

#endif // KBE_MYSQL_EXCEPTION_H


