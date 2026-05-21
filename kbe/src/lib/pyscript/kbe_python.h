// Keep KBE Debug builds on Python's Release ABI.
#pragma once

#if defined(_MSC_VER) && defined(_DEBUG)
#	define KBE_RESTORE_MSVC_DEBUG_MACRO
#	undef _DEBUG
#endif

#include "Python.h"

#ifdef KBE_RESTORE_MSVC_DEBUG_MACRO
#	define _DEBUG
#	undef KBE_RESTORE_MSVC_DEBUG_MACRO
#endif
