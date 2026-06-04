cmake_minimum_required(VERSION 3.20)
project(KBEngineNex LANGUAGES C CXX)

# 设置 C++ 标准
set(CMAKE_CXX_STANDARD 17)

set(CMAKE_CXX_STANDARD_REQUIRED ON)




# -----------------------------
# KBE 配置类型
# -----------------------------
set(KBE_SUPPORTED_CONFIGS
    Release
    Hybrid
    Debug
    Evaluation
    Debug_SingleThreaded
    Hybrid_SingleThreaded
    Release_SingleThreaded
    Debug_GCOV
    Debug_GCOV_SingleThreaded
)

# 如果用户没有传入 KBE_CONFIG，则默认 Hybrid
if(NOT DEFINED KBE_CONFIG)
    set(KBE_CONFIG "Hybrid" CACHE STRING "KBE Build Configuration")
endif()

# 校验 KBE_CONFIG 是否有效
list(FIND KBE_SUPPORTED_CONFIGS ${KBE_CONFIG} _config_index)
if(_config_index EQUAL -1)
    message(FATAL_ERROR "Unknown KBE_CONFIG: ${KBE_CONFIG}. Supported configs are: ${KBE_SUPPORTED_CONFIGS}")
endif()

message(STATUS "Using KBE_CONFIG: ${KBE_CONFIG}")

add_compile_definitions(KBE_CONFIG="${KBE_CONFIG}")


# -----------------------------
# 根据 KBE_CONFIG 设置 CMAKE_BUILD_TYPE
# -----------------------------
if(KBE_CONFIG MATCHES "Release")
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
    add_compile_definitions(_RELEASE)
    add_compile_definitions(_FORTIFY_SOURCE=2)
    
endif()

if(KBE_CONFIG MATCHES "Debug")
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type" FORCE)
    add_compile_definitions(KBE_USE_ASSERTS)
    add_compile_definitions(_DEBUG)
endif()

if(KBE_CONFIG MATCHES "Hybrid")
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type" FORCE)
    add_compile_definitions(KBE_USE_ASSERTS)
    add_compile_definitions(_HYBRID)    
    # add_compile_definitions(_FORTIFY_SOURCE=2)

    # 常规选项
    add_compile_options(-O2 -g -fno-omit-frame-pointer)

    # 启用 AddressSanitizer
    add_compile_options(-fsanitize=address)
    add_link_options(-fsanitize=address)
endif()

if(KBE_CONFIG MATCHES "Evaluation")
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type" FORCE)
    add_compile_definitions(KBE_USE_ASSERTS)
    add_compile_definitions(KBE_EVALUATION)
    add_compile_definitions(_HYBRID)
    add_compile_definitions(_FORTIFY_SOURCE=2)
endif()

# if(KBE_CONFIG MATCHES "GCOV")
#     set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type" FORCE)
# endif()





add_compile_options(-Wall)

# -----------------------------
# 根据 KBE_CONFIG 设置宏
# -----------------------------

# Release 或 Hybrid 或 Evaluation 都加 CODE_INLINE
if(KBE_CONFIG MATCHES "Release|Hybrid|Evaluation")
    add_compile_definitions(CODE_INLINE)
endif()

# 单线程模式宏
if(KBE_CONFIG MATCHES "SingleThreaded")
    add_compile_definitions(KBE_SINGLE_THREADED)
endif()


# GCOV 选项
if(KBE_CONFIG MATCHES "GCOV")
    add_compile_options(-fprofile-arcs -ftest-coverage)
endif()

# -----------------------------
# pthread 支持（非单线程才加）
# -----------------------------
if(NOT KBE_CONFIG MATCHES "SingleThreaded")
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)
    link_libraries(Threads::Threads)
endif()



# 关闭一些编译警告
add_compile_options(-Wno-deprecated-declarations)
# add_compile_options(-fsanitize=address -O1 -g)
# add_link_options(-fsanitize=address)



if(NOT DEFINED KBE_ROOT)
    string(REGEX REPLACE "/kbe/src(/.*)?$" "" KBE_ROOT "${CMAKE_SOURCE_DIR}")
endif()

# -----------------------------
# 1. vcpkg toolchain 支持
# -----------------------------
# 这里使用环境变量 VCPKG_ROOT 指向 vcpkg 安装目录
# if(DEFINED ENV{VCPKG_ROOT})
#     set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
#         CACHE STRING "")
# else()
#     message(WARNING "VCPKG_ROOT not defined, vcpkg packages will not be available")
# endif()

# 输出目录
set(LIB_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/libs")
file(MAKE_DIRECTORY ${LIB_OUTPUT_DIR})

# # 统一输出目录
# set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${KBE_ROOT}/kbe/src/libs)
# set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${KBE_ROOT}/kbe/src/libs)
# set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${KBE_ROOT}/kbe/src/libs)



include_directories(
    ${KBE_ROOT}/kbe/src
    ${KBE_ROOT}/kbe/src/lib
    ${KBE_ROOT}/kbe/src/server
    ${KBE_ROOT}/kbe/src/lib/dependencies
    ${KBE_ROOT}/kbe/src/lib/dependencies/g3dlite
    ${KBE_ROOT}/kbe/src/lib/dependencies/fmt/include
    ${KBE_ROOT}/kbe/src/lib/dependencies/curl/include
)


include(${CMAKE_SOURCE_DIR}/cmakefile/OpenSSL.cmake)

# vcpkg 包

find_package(PkgConfig REQUIRED)
# jemalloc 包
pkg_check_modules(JEMALLOC REQUIRED jemalloc)
# 头文件路径
include_directories(${JEMALLOC_INCLUDE_DIRS})
# 宏定义
add_definitions(${JEMALLOC_CFLAGS_OTHER})
# 所有 target 默认都加 jemalloc 链接选项
add_link_options(${JEMALLOC_LDFLAGS})
add_compile_definitions(USE_JEMALLOC)



add_compile_definitions(KBE_SERVER)

if(APPLE)
    add_compile_definitions(HAVE_DARWIN)
endif()

# # Release 才开启 CODE_INLINE
# if(KBE_CONFIG MATCHES "^Release")
#     add_compile_definitions(CODE_INLINE)
#     add_compile_definitions(_RELEASE)
# endif()
# if(KBE_CONFIG MATCHES "^Debug")
#     add_compile_definitions(_DEBUG)
# endif()


find_package(fmt CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(CURL REQUIRED)
find_package(EXPAT  REQUIRED COMPONENTS core)

find_package(log4cxx CONFIG REQUIRED)
find_package(ZLIB REQUIRED)
find_package(hiredis CONFIG REQUIRED)
find_package(mongoc CONFIG REQUIRED)
find_package(PostgreSQL REQUIRED)
find_package(recastnavigation CONFIG REQUIRED)


# apr
pkg_check_modules(APR REQUIRED apr-1)
include_directories(${APR_INCLUDE_DIRS})
link_libraries(${APR_LIBRARIES})



# apr-util
pkg_check_modules(APRUTIL REQUIRED apr-util-1)
include_directories(${APRUTIL_INCLUDE_DIRS})
link_libraries(${APRUTIL_LIBRARIES})

# $<$<CONFIG:Debug>:KBE_CONFIG=\"Debug\">
# $<$<CONFIG:Release>:KBE_CONFIG=\"Release\">
# $<$<CONFIG:RelWithDebInfo>:KBE_CONFIG=\"RelWithDebInfo\">
# $<$<CONFIG:MinSizeRel>:KBE_CONFIG=\"MinSizeRel\">

add_compile_definitions(

    ENABLE_WATCHERS=1
)


add_subdirectory(lib/dependencies/g3dlite)
add_subdirectory(lib/dependencies/jwsmtp/jwsmtp/jwsmtp)
if(WIN32)
    add_subdirectory(lib/dependencies/sigar/win32)
elseif(APPLE)
    add_subdirectory(lib/dependencies/sigar/macos)
else()
    add_subdirectory(lib/dependencies/sigar/linux)
endif()


add_subdirectory(lib/dependencies/tinyxml)
add_subdirectory(lib/dependencies/tmxparser)

add_subdirectory(lib/python)
add_subdirectory(lib/helper)
add_subdirectory(lib/client_lib)
add_subdirectory(lib/common)
add_subdirectory(lib/db_mysql)
add_subdirectory(lib/db_mongodb)
add_subdirectory(lib/db_postgresql)
add_subdirectory(lib/db_interface)
add_subdirectory(lib/xml)
add_subdirectory(lib/resmgr)
add_subdirectory(lib/entitydef)
add_subdirectory(lib/math)
add_subdirectory(lib/navigation)
add_subdirectory(lib/network)
add_subdirectory(lib/pyscript)
add_subdirectory(lib/server)
add_subdirectory(lib/thread)


add_subdirectory(server/baseapp)
add_subdirectory(server/baseappmgr)
add_subdirectory(server/cellapp)
add_subdirectory(server/cellappmgr)
add_subdirectory(server/dbmgr)
add_subdirectory(server/loginapp)
add_subdirectory(server/machine)
add_subdirectory(server/tools/interfaces)
add_subdirectory(server/tools/logger)
add_subdirectory(server/tools/kbcmd)
add_subdirectory(server/tools/bots)

