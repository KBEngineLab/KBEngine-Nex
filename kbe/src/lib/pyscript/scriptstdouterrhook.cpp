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


#include "scriptstdouterrhook.h"

#ifndef CODE_INLINE
#include "scriptstdouterrhook.inl"
#endif

namespace KBEngine{ namespace script{
								
//-------------------------------------------------------------------------------------
ScriptStdOutErrHook::ScriptStdOutErrHook():
pBuffer_(NULL),
isPrint_(true)
{
}

//-------------------------------------------------------------------------------------
ScriptStdOutErrHook::~ScriptStdOutErrHook()
{
}

//-------------------------------------------------------------------------------------
void ScriptStdOutErrHook::info_msg(const char* msg, uint32 msglen)
{
	if(isPrint_)
		ScriptStdOutErr::info_msg(msg, msglen);

	// Hook 返回的是 Python 原始字符流，不应受日志行缓冲和 flush 时机影响。
	// The hook returns Python's original character stream and must not depend on log line buffering or flush timing.
	if (pBuffer_ && msg != NULL && msglen > 0)
		pBuffer_->append(msg, msglen);
}

//-------------------------------------------------------------------------------------
void ScriptStdOutErrHook::error_msg(const char* msg, uint32 msglen)
{
	if(isPrint_)
		ScriptStdOutErr::error_msg(msg, msglen);

	if (pBuffer_ && msg != NULL && msglen > 0)
		pBuffer_->append(msg, msglen);
}

//-------------------------------------------------------------------------------------

}
}
