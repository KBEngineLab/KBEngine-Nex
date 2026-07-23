#include "KBDebug.h"
#include <cstdarg>
#include <cstdio>

#if KBE_PLATFORM_UE
#include "Engine/Engine.h"

// 定义日志类别
DEFINE_LOG_CATEGORY(LogKBE);

namespace KBDebug
{
    void log_info(const char* fmt, ...)
    {
        char buffer[2048];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buffer, sizeof(buffer), fmt, args);
        va_end(args);

        FString msg = UTF8_TO_TCHAR(buffer);
        UE_LOG(LogKBE, Log, TEXT("%s"), *msg);
    }

    void log_debug(const char* fmt, ...)
    {
        char buffer[2048];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buffer, sizeof(buffer), fmt, args);
        va_end(args);

        FString msg = UTF8_TO_TCHAR(buffer);
        UE_LOG(LogKBE, Log, TEXT("[DEBUG] %s"), *msg);
    }

    void log_warning(const char* fmt, ...)
    {
        char buffer[2048];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buffer, sizeof(buffer), fmt, args);
        va_end(args);

        FString msg = UTF8_TO_TCHAR(buffer);
        UE_LOG(LogKBE, Warning, TEXT("[WARNING] %s"), *msg);
    }

    void log_error(const char* fmt, ...)
    {
        char buffer[2048];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buffer, sizeof(buffer), fmt, args);
        va_end(args);

        FString msg = UTF8_TO_TCHAR(buffer);
        UE_LOG(LogKBE, Error, TEXT("[ERROR] %s"), *msg);
    }

    void log_screen_warning(const char* fmt, ...)
    {
        char buffer[2048];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buffer, sizeof(buffer), fmt, args);
        va_end(args);

        FString msg = UTF8_TO_TCHAR(buffer);
        UE_LOG(LogKBE, Warning, TEXT("%s"), *msg);
        if (GEngine)
            GEngine->AddOnScreenDebugMessage(-1, 5.f, FColor::Yellow, msg);
    }

    void log_screen_error(const char* fmt, ...)
    {
        char buffer[2048];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buffer, sizeof(buffer), fmt, args);
        va_end(args);

        FString msg = UTF8_TO_TCHAR(buffer);
        UE_LOG(LogKBE, Error, TEXT("%s"), *msg);
        if (GEngine)
            GEngine->AddOnScreenDebugMessage(-1, 10.f, FColor::Red, msg);
    }

    void log_screen_debug(const char* fmt, ...)
    {
        char buffer[2048];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buffer, sizeof(buffer), fmt, args);
        va_end(args);

        FString msg = UTF8_TO_TCHAR(buffer);
        if (GEngine)
            GEngine->AddOnScreenDebugMessage(-1, 5.f, FColor::White, msg);
    }
}

#elif KBE_PLATFORM_COCOS

#include "base/CCConsole.h"
#include "base/CCDirector.h"
#include "base/CCScheduler.h"
#include "base/ccUTF8.h"
#include <cstdarg>

namespace KBDebug
{
    void log_info(const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        cocos2d::log("[INFO] %s", fmt, args);
        va_end(args);
    }

    void log_debug(const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        cocos2d::log("[DEBUG] %s", fmt, args);
        va_end(args);
    }

    void log_warning(const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        cocos2d::log("[WARNING] %s", fmt, args);
        va_end(args);
    }

    void log_error(const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        cocos2d::log("[ERROR] %s", fmt, args);
        va_end(args);
    }

    void log_screen_warning(const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        auto msg = cocos2d::StringUtils::format(fmt, args);
        cocos2d::MessageBox(msg.c_str(), "Warning");
        va_end(args);
    }

    void log_screen_error(const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        auto msg = cocos2d::StringUtils::format(fmt, args);
        cocos2d::MessageBox(msg.c_str(), "Error");
        va_end(args);
    }

    void log_screen_debug(const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        cocos2d::log("[SCREEN] %s", fmt, args);
        va_end(args);
    }
}

#elif KBE_PLATFORM_CPP

#include <cstdarg>
#include <cstdio>

namespace KBDebug
{
    void kbe_printf(const char* prefix, const char* fmt, ...)
    {
        char buffer[2048];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buffer, sizeof(buffer), fmt, args);
        va_end(args);


        printf("%s %s\n", prefix, buffer);
        fflush(stdout);
    }

    void log_info(const char* fmt, ...) {
        char buffer[2048];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buffer, sizeof(buffer), fmt, args);
        va_end(args);

        printf("[INFO] %s\n", buffer);
        fflush(stdout);
    }
    void log_debug(const char* fmt, ...) {
        char buffer[2048];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buffer, sizeof(buffer), fmt, args);
        va_end(args);

        printf("[DEBUG] %s\n", buffer);
        fflush(stdout);
    }
    void log_warning(const char* fmt, ...) {
        char buffer[2048];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buffer, sizeof(buffer), fmt, args);
        va_end(args);

        printf("[WARNING] %s\n", buffer);
        fflush(stdout);
    }
    void log_error(const char* fmt, ...) {
        char buffer[2048];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buffer, sizeof(buffer), fmt, args);
        va_end(args);

        printf("[ERROR] %s\n", buffer);
        fflush(stdout);
    }
    void log_screen_warning(const char* fmt, ...) {
        char buffer[2048];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buffer, sizeof(buffer), fmt, args);
        va_end(args);

        printf("[WARNING][SCREEN] %s\n", buffer);
        fflush(stdout);
    }
    void log_screen_error(const char* fmt, ...) {
        char buffer[2048];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buffer, sizeof(buffer), fmt, args);
        va_end(args);

        printf("[ERROR][SCREEN] %s\n", buffer);
        fflush(stdout);
    }
    void log_screen_debug(const char* fmt, ...) {
        char buffer[2048];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buffer, sizeof(buffer), fmt, args);
        va_end(args);

        printf("[SCREEN] %s\n", buffer);
        fflush(stdout);
    }
}

#endif
