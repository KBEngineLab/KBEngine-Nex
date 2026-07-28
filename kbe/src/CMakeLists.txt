cmake_minimum_required(VERSION 3.25)

# 当前锁定的 vcpkg 基线仍包含声明 CMake 3.5 以下兼容策略的端口；CMake 4 会在依赖安装阶段中途失败。
# The pinned vcpkg baseline still contains ports declaring pre-3.5 compatibility policies; CMake 4 fails partway through dependency installation.
if(CMAKE_VERSION VERSION_GREATER_EQUAL 4.0)
    message(FATAL_ERROR
        "The current vcpkg baseline requires CMake 3.x. "
        "On Windows, use the CMake 3.31 bundled with Visual Studio 2022."
    )
endif()

# Preset 依赖 VCPKG_ROOT 展开工具链路径；环境变量为空时必须在 project() 触发自动依赖恢复前终止。
# The preset expands VCPKG_ROOT into the toolchain path; an empty variable must fail before project() can trigger automatic dependency restoration.
if(DEFINED CMAKE_TOOLCHAIN_FILE AND NOT EXISTS "${CMAKE_TOOLCHAIN_FILE}")
    message(FATAL_ERROR
        "CMAKE_TOOLCHAIN_FILE does not exist: ${CMAKE_TOOLCHAIN_FILE}. "
        "Set VCPKG_ROOT to the vcpkg checkout that provides the pinned manifest baseline."
    )
endif()

project(KBEngineNex VERSION 2.8.2 LANGUAGES C CXX)

# CMake 构建与现有 VS 工程并行存在；独立输出目录避免两套构建互相覆盖产物。
# The CMake build coexists with the existing VS projects; isolated output directories prevent either build from overwriting the other.
set(KBE_CMAKE_OUTPUT_ROOT "${CMAKE_SOURCE_DIR}/out/cmake" CACHE PATH "CMake output root")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${KBE_CMAKE_OUTPUT_ROOT}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${KBE_CMAKE_OUTPUT_ROOT}/lib")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${KBE_CMAKE_OUTPUT_ROOT}/bin")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(KbeBuildOptions)
include(KbePythonRuntime)

find_package(fmt CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(CURL CONFIG REQUIRED)
find_package(EXPAT REQUIRED)
find_package(log4cxx CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_subdirectory(lib/common)
add_subdirectory(lib/math)
add_subdirectory(lib/thread)
add_subdirectory(lib/xml)
add_subdirectory(lib/dependencies/sigar)
add_subdirectory(lib/helper)
add_subdirectory(lib/resmgr)
add_subdirectory(lib/network)
add_subdirectory(lib/pyscript)
add_subdirectory(lib/entitydef)
add_subdirectory(lib/dependencies/jwsmtp)
add_subdirectory(lib/dependencies/tmxparser)
add_subdirectory(lib/navigation)
add_subdirectory(lib/server)
add_subdirectory(lib/client_lib)
add_subdirectory(lib/db_interface)
add_subdirectory(lib/db_mysql)
add_subdirectory(lib/db_redis)
add_subdirectory(lib/db_mongodb)
add_subdirectory(lib/db_postgresql)

# 首阶段只迁移无 Python 和数据库依赖的底层库，后续目标按依赖拓扑逐层接入。
# The first stage migrates foundational libraries without Python or database dependencies; later targets are added in dependency order.
add_custom_target(kbe_core DEPENDS common math thread xml)

# 第二层闭合资源、网络、脚本和实体定义依赖，但仍不改变旧 VS 工程或服务端链接入口。
# The second layer closes resource, network, scripting, and entity-definition dependencies without changing legacy VS projects or server link entry points.
add_custom_target(kbe_core_dependencies DEPENDS helper resmgr network pyscript entitydef)

# 完整库层用于验证所有服务端和工具共享依赖，组件可执行文件在下一阶段按真实链接入口接入。
# The complete library layer validates every dependency shared by server components and tools; executables are added from their real link entry points in the next stage.
add_custom_target(kbe_libraries DEPENDS
    client_lib
    db_interface
    db_mysql
    db_redis
    db_mongodb
    db_postgresql
    navigation
    server
    jwsmtp
    tmxparser
)
