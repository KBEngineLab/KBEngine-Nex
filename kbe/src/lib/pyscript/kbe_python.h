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

#ifndef KBE_PYTHON_H
#define KBE_PYTHON_H

/*
	使用带 # 的 Python 参数格式时必须采用 Py_ssize_t，否则 Python 3.12 会拒绝调用并抛出 SystemError。
	Python argument formats containing # must use Py_ssize_t; otherwise Python 3.12 rejects the call with SystemError.
*/
#ifndef PY_SSIZE_T_CLEAN
#	define PY_SSIZE_T_CLEAN
#endif

/*
	KBE 的 Debug 配置仍使用 Python Release ABI；包含 CPython 头时临时隐藏 _DEBUG，避免头文件启用 Py_DEBUG 并自动链接 pythonXY_d.lib。
	KBE Debug configurations still use Python's Release ABI; hide _DEBUG while including CPython headers so they neither enable Py_DEBUG nor auto-link pythonXY_d.lib.
*/
#if defined(_MSC_VER) && defined(_DEBUG)
#	define KBE_RESTORE_MSVC_DEBUG_MACRO
#	undef _DEBUG
#endif

#include "Python.h"

#ifdef KBE_RESTORE_MSVC_DEBUG_MACRO
#	define _DEBUG
#	undef KBE_RESTORE_MSVC_DEBUG_MACRO
#endif

#endif // KBE_PYTHON_H
