/*
This source file is part of KBEngine
For the latest info, see http://www.kbengine.org/

Copyright (c) 2008-2018 KBEngine.

KBEngine is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/

#include "crashhandler.h"

#if KBE_PLATFORM == PLATFORM_WIN32

#include <intrin.h>
#include <signal.h>
#include <stdlib.h>

namespace KBEngine { namespace exception {
namespace
{
const wchar_t CRASH_DIR[] = L"CrashDumps";
const DWORD KBE_EXCEPTION_ABORT = 0xE0000001u;
const DWORD KBE_EXCEPTION_TERMINATE = 0xE0000002u;
const DWORD KBE_EXCEPTION_PURECALL = 0xE0000003u;
const DWORD KBE_EXCEPTION_INVALID_PARAMETER = 0xE0000004u;

char g_dumpTag[MAX_NAME] = "bootstrap";
COMPONENT_ID g_componentID = 0;
LONG g_isInstalled = 0;
LONG g_isHandlingCrash = 0;
wchar_t g_dumpPath[MAX_PATH * 2] = { 0 };
wchar_t g_logPath[MAX_PATH * 2] = { 0 };

const char* signalName(int sigNum)
{
	switch (sigNum)
	{
	case SIGABRT: return "SIGABRT";
	case SIGFPE: return "SIGFPE";
	case SIGILL: return "SIGILL";
	case SIGINT: return "SIGINT";
	case SIGSEGV: return "SIGSEGV";
	case SIGTERM: return "SIGTERM";
#ifdef SIGBREAK
	case SIGBREAK: return "SIGBREAK";
#endif
	default: return "UNKNOWN_SIGNAL";
	}
}

DWORD signalToExceptionCode(int sigNum)
{
	switch (sigNum)
	{
	case SIGABRT: return KBE_EXCEPTION_ABORT;
	case SIGFPE: return EXCEPTION_FLT_DIVIDE_BY_ZERO;
	case SIGILL: return EXCEPTION_ILLEGAL_INSTRUCTION;
	case SIGSEGV: return EXCEPTION_ACCESS_VIOLATION;
	case SIGTERM: return KBE_EXCEPTION_TERMINATE;
	default: return 0xE0000000u + static_cast<DWORD>(sigNum);
	}
}

void ensureCrashDir()
{
	::CreateDirectoryW(CRASH_DIR, NULL);
}

void refreshOutputPaths()
{
	ensureCrashDir();

	SYSTEMTIME st;
	::GetLocalTime(&st);

	wchar_t dumpTagW[MAX_NAME] = { 0 };
	::MultiByteToWideChar(CP_UTF8, 0, g_dumpTag, -1, dumpTagW, MAX_NAME);
	if (dumpTagW[0] == L'\0')
		wcscpy_s(dumpTagW, L"bootstrap");

	_snwprintf_s(g_dumpPath, _countof(g_dumpPath), _TRUNCATE,
		L"%ls\\%ls_%llu_%lu_%04d%02d%02d_%02d%02d%02d_%03d.dmp",
		CRASH_DIR, dumpTagW, static_cast<unsigned long long>(g_componentID),
		::GetCurrentProcessId(), st.wYear, st.wMonth, st.wDay,
		st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

	_snwprintf_s(g_logPath, _countof(g_logPath), _TRUNCATE,
		L"%ls\\%ls_%llu_%lu_%04d%02d%02d_%02d%02d%02d_%03d.log",
		CRASH_DIR, dumpTagW, static_cast<unsigned long long>(g_componentID),
		::GetCurrentProcessId(), st.wYear, st.wMonth, st.wDay,
		st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

void writeText(HANDLE hFile, const char* text)
{
	if (hFile == INVALID_HANDLE_VALUE || text == NULL)
		return;

	DWORD written = 0;
	::WriteFile(hFile, text, static_cast<DWORD>(strlen(text)), &written, NULL);
}

void writeCrashLog(const char* reason, EXCEPTION_POINTERS* pep, int sigNum,
	bool dumpCreated, DWORD dumpError)
{
	HANDLE hFile = ::CreateFileW(g_logPath, GENERIC_WRITE, FILE_SHARE_READ, NULL,
		CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE)
		return;

	SYSTEMTIME st;
	::GetLocalTime(&st);
	char buffer[2048] = { 0 };
	_snprintf_s(buffer, _countof(buffer), _TRUNCATE,
		"time=%04d-%02d-%02d %02d:%02d:%02d.%03d\r\n"
		"pid=%lu\r\ntid=%lu\r\ncomponent=%s\r\ncomponent_id=%" PRIu64 "\r\nreason=%s\r\n",
		st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
		::GetCurrentProcessId(), ::GetCurrentThreadId(), g_dumpTag,
		static_cast<uint64>(g_componentID), reason ? reason : "unknown");
	writeText(hFile, buffer);

	if (sigNum != 0)
	{
		_snprintf_s(buffer, _countof(buffer), _TRUNCATE,
			"signal=%d (%s)\r\n", sigNum, signalName(sigNum));
		writeText(hFile, buffer);
	}

	if (pep && pep->ExceptionRecord)
	{
		_snprintf_s(buffer, _countof(buffer), _TRUNCATE,
			"exception_code=0x%08lX\r\nexception_address=%p\r\n",
			pep->ExceptionRecord->ExceptionCode, pep->ExceptionRecord->ExceptionAddress);
		writeText(hFile, buffer);
	}

	_snprintf_s(buffer, _countof(buffer), _TRUNCATE,
		"dump_created=%s\r\ndump_path=%ls\r\n",
		dumpCreated ? "true" : "false", g_dumpPath);
	writeText(hFile, buffer);

	if (!dumpCreated)
	{
		_snprintf_s(buffer, _countof(buffer), _TRUNCATE,
			"dump_error=%lu\r\n", dumpError);
		writeText(hFile, buffer);
	}

	::CloseHandle(hFile);
}

bool createMiniDumpInternal(EXCEPTION_POINTERS* pep, DWORD& lastError)
{
	HANDLE hFile = ::CreateFileW(g_dumpPath, GENERIC_READ | GENERIC_WRITE, 0, NULL,
		CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE)
	{
		lastError = ::GetLastError();
		return false;
	}

	MINIDUMP_EXCEPTION_INFORMATION mdei;
	mdei.ThreadId = ::GetCurrentThreadId();
	mdei.ExceptionPointers = pep;
	mdei.ClientPointers = FALSE;

	MINIDUMP_CALLBACK_INFORMATION mci;
	mci.CallbackRoutine = static_cast<MINIDUMP_CALLBACK_ROUTINE>(dumpCallback);
	mci.CallbackParam = 0;

	const MINIDUMP_TYPE dumpType = static_cast<MINIDUMP_TYPE>(
		MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory |
		MiniDumpWithHandleData | MiniDumpWithThreadInfo);
	const BOOL ok = ::MiniDumpWriteDump(::GetCurrentProcess(), ::GetCurrentProcessId(),
		hFile, dumpType, pep ? &mdei : NULL, 0, &mci);
	if (!ok)
		lastError = ::GetLastError();

	::CloseHandle(hFile);
	return ok == TRUE;
}

EXCEPTION_POINTERS* makeSignalExceptionPointers(int sigNum, EXCEPTION_RECORD& record,
	CONTEXT& context, EXCEPTION_POINTERS& pointers)
{
	memset(&record, 0, sizeof(record));
	memset(&context, 0, sizeof(context));
	::RtlCaptureContext(&context);
	record.ExceptionCode = signalToExceptionCode(sigNum);
	record.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
#if defined(_M_X64)
	record.ExceptionAddress = reinterpret_cast<PVOID>(context.Rip);
#elif defined(_M_IX86)
	record.ExceptionAddress = reinterpret_cast<PVOID>(context.Eip);
#elif defined(_M_ARM64)
	record.ExceptionAddress = reinterpret_cast<PVOID>(context.Pc);
#else
	record.ExceptionAddress = _ReturnAddress();
#endif
	pointers.ExceptionRecord = &record;
	pointers.ContextRecord = &context;
	return &pointers;
}

LONG writeCrashArtifacts(const char* reason, EXCEPTION_POINTERS* pep, int sigNum)
{
	// 崩溃路径不能重入，否则二次异常会覆盖首次现场或递归耗尽栈。
	// Crash handling must not re-enter, or a secondary fault can overwrite the first failure or recurse indefinitely.
	if (::InterlockedCompareExchange(&g_isHandlingCrash, 1, 0) != 0)
		return EXCEPTION_EXECUTE_HANDLER;

	::fflush(stdout);
	::fflush(stderr);
	refreshOutputPaths();
	DWORD dumpError = 0;
	const bool dumpCreated = createMiniDumpInternal(pep, dumpError);
	writeCrashLog(reason, pep, sigNum, dumpCreated, dumpError);
	fprintf(stderr, "[CRASH] %s, dump=%ls, log=%ls\n",
		reason ? reason : "unknown", g_dumpPath, g_logPath);
	::fflush(stderr);
	return EXCEPTION_EXECUTE_HANDLER;
}

LONG WINAPI topLevelExceptionFilter(EXCEPTION_POINTERS* pep)
{
	return handleStructuredException(pep);
}

void __cdecl signalCrashHandler(int sigNum)
{
	EXCEPTION_RECORD record;
	CONTEXT context;
	EXCEPTION_POINTERS pointers;
	EXCEPTION_POINTERS* pep = makeSignalExceptionPointers(sigNum, record, context, pointers);
	char reason[128] = { 0 };
	_snprintf_s(reason, _countof(reason), _TRUNCATE,
		"Unhandled signal: %s(%d)", signalName(sigNum), sigNum);
	writeCrashArtifacts(reason, pep, sigNum);
	::TerminateProcess(::GetCurrentProcess(), static_cast<UINT>(signalToExceptionCode(sigNum)));
}

void __cdecl terminateHandler()
{
	writeCrashArtifacts("Unhandled std::terminate", NULL, 0);
	::TerminateProcess(::GetCurrentProcess(), static_cast<UINT>(KBE_EXCEPTION_TERMINATE));
}

void __cdecl purecallHandler()
{
	writeCrashArtifacts("Unhandled CRT purecall", NULL, 0);
	::TerminateProcess(::GetCurrentProcess(), static_cast<UINT>(KBE_EXCEPTION_PURECALL));
}

void __cdecl invalidParameterHandler(const wchar_t* expression,
	const wchar_t* functionName, const wchar_t* fileName, unsigned int line, uintptr_t)
{
	char reason[256] = { 0 };
	_snprintf_s(reason, _countof(reason), _TRUNCATE,
		"Unhandled invalid parameter at line %u", line);
	writeCrashArtifacts(reason, NULL, 0);

	if (expression || functionName || fileName)
	{
		HANDLE hFile = ::CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
			OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (hFile != INVALID_HANDLE_VALUE)
		{
			char extra[1024] = { 0 };
			char expressionA[256] = { 0 };
			char functionNameA[256] = { 0 };
			char fileNameA[256] = { 0 };
			if (expression)
				::WideCharToMultiByte(CP_UTF8, 0, expression, -1, expressionA, sizeof(expressionA), NULL, NULL);
			if (functionName)
				::WideCharToMultiByte(CP_UTF8, 0, functionName, -1, functionNameA, sizeof(functionNameA), NULL, NULL);
			if (fileName)
				::WideCharToMultiByte(CP_UTF8, 0, fileName, -1, fileNameA, sizeof(fileNameA), NULL, NULL);
			_snprintf_s(extra, _countof(extra), _TRUNCATE,
				"invalid_parameter.expression=%s\r\n"
				"invalid_parameter.function=%s\r\n"
				"invalid_parameter.file=%s\r\n",
				expressionA, functionNameA, fileNameA);
			writeText(hFile, extra);
			::CloseHandle(hFile);
		}
	}

	::TerminateProcess(::GetCurrentProcess(), static_cast<UINT>(KBE_EXCEPTION_INVALID_PARAMETER));
}

void registerSignalHandlers()
{
	::signal(SIGABRT, signalCrashHandler);
	::signal(SIGFPE, signalCrashHandler);
	::signal(SIGILL, signalCrashHandler);
	::signal(SIGSEGV, signalCrashHandler);
}
}

void installCrashHandler(const char* dumpType, COMPONENT_ID componentID)
{
	if (dumpType && dumpType[0] != '\0')
		strncpy_s(g_dumpTag, dumpType, _TRUNCATE);
	else
		strcpy_s(g_dumpTag, "bootstrap");

	g_componentID = componentID;
	refreshOutputPaths();
	if (::InterlockedCompareExchange(&g_isInstalled, 1, 0) == 0)
	{
		::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
		::SetUnhandledExceptionFilter(topLevelExceptionFilter);
		_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
		_set_invalid_parameter_handler(invalidParameterHandler);
		_set_purecall_handler(purecallHandler);
		std::set_terminate(terminateHandler);
		registerSignalHandlers();
	}
	else
	{
		// 某些运行库会替换顶层过滤器，刷新身份时同步恢复它。
		// Some runtimes replace the top-level filter, so identity refresh also restores it.
		::SetUnhandledExceptionFilter(topLevelExceptionFilter);
	}
}

LONG WINAPI handleStructuredException(EXCEPTION_POINTERS* pep)
{
	char reason[128] = { 0 };
	if (pep && pep->ExceptionRecord)
	{
		_snprintf_s(reason, _countof(reason), _TRUNCATE,
			"Unhandled SEH exception: 0x%08lX", pep->ExceptionRecord->ExceptionCode);
	}
	else
	{
		strcpy_s(reason, "Unhandled SEH exception");
	}
	return writeCrashArtifacts(reason, pep, 0);
}

void createMiniDump(EXCEPTION_POINTERS* pep)
{
	refreshOutputPaths();
	DWORD dumpError = 0;
	createMiniDumpInternal(pep, dumpError);
}

BOOL CALLBACK dumpCallback(PVOID, const PMINIDUMP_CALLBACK_INPUT pInput,
	PMINIDUMP_CALLBACK_OUTPUT pOutput)
{
	if (pInput == NULL || pOutput == NULL)
		return FALSE;

	switch (pInput->CallbackType)
	{
	case IncludeModuleCallback:
	case IncludeThreadCallback:
	case ThreadCallback:
	case ThreadExCallback:
		return TRUE;
	case ModuleCallback:
		if (!(pOutput->ModuleWriteFlags & ModuleReferencedByMemory))
			pOutput->ModuleWriteFlags &= (~ModuleWriteModule);
		return TRUE;
	case MemoryCallback:
	case CancelCallback:
	default:
		return FALSE;
	}
}

}}
#endif
