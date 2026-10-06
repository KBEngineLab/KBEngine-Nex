#pragma once

// 平台默认由编译器和宿主框架宏自动识别。
// 可通过定义一个 KBE_PLATFORM_FORCE_* 宏覆盖探测结果，但正常构建不需要任何外部设置。
#if (defined(KBE_PLATFORM_FORCE_UE) + \
     defined(KBE_PLATFORM_FORCE_COCOS) + \
     defined(KBE_PLATFORM_FORCE_CPP)) > 1
#error "Only one KBE_PLATFORM_FORCE_* override may be defined"
#endif

#if defined(KBE_PLATFORM_FORCE_UE)
    #define KBE_PLATFORM_UE 1
#elif defined(KBE_PLATFORM_FORCE_COCOS)
    #define KBE_PLATFORM_COCOS 1
#elif defined(KBE_PLATFORM_FORCE_CPP)
    #define KBE_PLATFORM_CPP 1
#elif defined(UE_BUILD_DEBUG) || \
      defined(UE_BUILD_DEVELOPMENT) || \
      defined(UE_BUILD_TEST) || \
      defined(UE_BUILD_SHIPPING) || \
      defined(UE_EDITOR) || \
      defined(UE_SERVER) || \
      defined(UE_GAME) || \
      defined(UE_CLIENT) || \
      defined(__UNREAL__) || \
      defined(ENGINE_MAJOR_VERSION)
    #define KBE_PLATFORM_UE 1
#elif defined(CC_TARGET_PLATFORM)
    #define KBE_PLATFORM_COCOS 1
#else
    #define KBE_PLATFORM_CPP 1
#endif

// 未选中的平台也必须显式为 0，避免 UE5.8 V7 对裸 #if 宏判断报 C4668。
#ifndef KBE_PLATFORM_UE
    #define KBE_PLATFORM_UE 0
#endif

#ifndef KBE_PLATFORM_COCOS
    #define KBE_PLATFORM_COCOS 0
#endif

#ifndef KBE_PLATFORM_CPP
    #define KBE_PLATFORM_CPP 0
#endif

#if (KBE_PLATFORM_UE + KBE_PLATFORM_COCOS + KBE_PLATFORM_CPP) != 1
    #error "Exactly one KBE platform must be selected"
#endif

#if KBE_PLATFORM_UE
    #define KBE_PLATFORM_NAME "Unreal Engine"
#elif KBE_PLATFORM_COCOS
    #define KBE_PLATFORM_NAME "Cocos2d-x"
#else
    #define KBE_PLATFORM_NAME "Native C++"
#endif

// 需要确认探测结果时，可在编译命令中增加 KBE_PLATFORM_DIAGNOSTIC=1。
#if defined(KBE_PLATFORM_DIAGNOSTIC)
    #if KBE_PLATFORM_UE
        #pragma message("KBE platform detected: Unreal Engine")
    #elif KBE_PLATFORM_COCOS
        #pragma message("KBE platform detected: Cocos2d-x")
    #else
        #pragma message("KBE platform detected: Native C++")
    #endif
#endif
