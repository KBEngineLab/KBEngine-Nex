if(WIN32)
    set(KBE_SIGAR_PLATFORM_DIR win32)
    set(KBE_SIGAR_SOURCES
        win32/peb.c
        win32/sigar.c
        win32/sigar_cache.c
        win32/sigar_fileinfo.c
        win32/sigar_format.c
        win32/sigar_getline.c
        win32/sigar_ptql.c
        win32/sigar_signal.c
        win32/sigar_util.c
        win32/win32_sigar.c
        win32/wmi.cpp
    )
elseif(APPLE)
    message(FATAL_ERROR "The macOS sigar source must be migrated before building helper on macOS.")
else()
    set(KBE_SIGAR_PLATFORM_DIR linux)
    set(KBE_SIGAR_SOURCES
        linux/sigar.c
        linux/sigar_cache.c
        linux/sigar_fileinfo.c
        linux/sigar_format.c
        linux/sigar_getline.c
        linux/sigar_ptql.c
        linux/sigar_signal.c
        linux/sigar_util.c
        linux/linux_sigar.c
    )
endif()

add_library(sigar STATIC ${KBE_SIGAR_SOURCES})
kbe_configure_target(sigar)
target_include_directories(sigar PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/${KBE_SIGAR_PLATFORM_DIR}")

if(MSVC)
    target_compile_definitions(sigar PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(sigar PUBLIC iphlpapi psapi wbemuuid ws2_32)
elseif(UNIX AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(TIRPC REQUIRED IMPORTED_TARGET libtirpc)
    target_link_libraries(sigar PUBLIC PkgConfig::TIRPC)
endif()
