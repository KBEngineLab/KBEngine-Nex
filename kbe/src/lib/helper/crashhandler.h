/*
This source file is part of KBEngine
For the latest info, see http://www.kbengine.org/

Copyright (c) 2008-2018 KBEngine.

KBEngine is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/

#ifndef KBE_CRASHHANDLER_H
#define KBE_CRASHHANDLER_H

#include "common/common.h"

#ifdef _WIN32
#include <windows.h>
#include <tchar.h>
#include <dbghelp.h>
#include <stdio.h>
#include <crtdbg.h>
#include <time.h>
#include <exception>
#pragma comment(lib, "dbghelp.lib")
#else
#include <errno.h>
#endif

namespace KBEngine { namespace exception {

// 重复安装只刷新组件身份，进程级处理器只注册一次。
// Reinstalling only refreshes component identity; process-wide handlers are registered once.
void installCrashHandler(const char* dumpType, COMPONENT_ID componentID);

#ifdef _WIN32
LONG WINAPI handleStructuredException(EXCEPTION_POINTERS* pep);
void createMiniDump(EXCEPTION_POINTERS* pep);

BOOL CALLBACK dumpCallback(
	PVOID pParam,
	const PMINIDUMP_CALLBACK_INPUT pInput,
	PMINIDUMP_CALLBACK_OUTPUT pOutput);
#endif

#ifndef _DEBUG
	#define THREAD_TRY_EXECUTION int exceptionCode = 0; __try {
	#define THREAD_HANDLE_CRASH } __except(exceptionCode = GetExceptionCode(), KBEngine::exception::handleStructuredException(GetExceptionInformation())) { \
		printf("Unhandled SEH exception: 0x%08X\n", exceptionCode); \
	}
#else
	#define THREAD_TRY_EXECUTION
	#define THREAD_HANDLE_CRASH
#endif

}}

#endif
