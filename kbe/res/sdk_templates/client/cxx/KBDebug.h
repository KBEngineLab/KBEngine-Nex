#pragma once
#include <string>
#include <cstdarg>

#if defined(__UNREAL__) || defined(UE_BUILD_DEBUG) || defined(UE_SERVER) || defined(UE_GAME) || defined(UE_CLIENT) || defined(UE_BUILD_DEVELOPMENT) || defined(UE_BUILD_SHIPPING)
    #define KBE_PLATFORM_UE 1
#elif defined(CC_TARGET_PLATFORM)
    #define KBE_PLATFORM_COCOS 1
#else
    #define KBE_PLATFORM_CPP 1
#endif

// =========================================
// UE 平台日志类别声明
// =========================================
#if KBE_PLATFORM_UE
#include "CoreMinimal.h"
DECLARE_LOG_CATEGORY_EXTERN(LogKBE, Log, All);
#elif KBE_PLATFORM_COCOS
#include "base/CCConsole.h"
#include "base/CCDirector.h"
#include "base/CCScheduler.h"
#include <string>
#elif KBE_PLATFORM_CPP
#include <cstdio>
#endif

// =========================================
// 日志函数声明
// =========================================
namespace KBDebug
{
#if KBE_PLATFORM_CPP
    void kbe_printf(const char* prefix, const char* fmt, ...);
#endif

    void log_info(const char* fmt, ...);
    void log_debug(const char* fmt, ...);
    void log_warning(const char* fmt, ...);
    void log_error(const char* fmt, ...);
    void log_screen_warning(const char* fmt, ...);
    void log_screen_error(const char* fmt, ...);
    void log_screen_debug(const char* fmt, ...);
}

// =========================================
// 日志宏接口
// =========================================
#define INFO_MSG(Format, ...)      KBDebug::log_info(Format, ##__VA_ARGS__)
#define DEBUG_MSG(Format, ...)     KBDebug::log_debug(Format, ##__VA_ARGS__)
#define WARNING_MSG(Format, ...)   KBDebug::log_warning(Format, ##__VA_ARGS__)
#define ERROR_MSG(Format, ...)     KBDebug::log_error(Format, ##__VA_ARGS__)
#define SCREEN_WARNING_MSG(Format, ...) KBDebug::log_screen_warning(Format, ##__VA_ARGS__)
#define SCREEN_ERROR_MSG(Format, ...)   KBDebug::log_screen_error(Format, ##__VA_ARGS__)
#define SCREENDEBUG(Format, ...)   KBDebug::log_screen_debug(Format, ##__VA_ARGS__)
