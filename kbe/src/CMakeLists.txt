cmake_minimum_required(VERSION 3.25)

# Preset 依赖 VCPKG_ROOT 展开工具链路径；环境变量为空时必须在 project() 触发自动依赖恢复前终止。
# The preset expands VCPKG_ROOT into the toolchain path; an empty variable must fail before project() can trigger automatic dependency restoration.
if(DEFINED CMAKE_TOOLCHAIN_FILE AND NOT EXISTS "${CMAKE_TOOLCHAIN_FILE}")
    message(FATAL_ERROR
        "CMAKE_TOOLCHAIN_FILE does not exist: ${CMAKE_TOOLCHAIN_FILE}. "
        "Set VCPKG_ROOT to the vcpkg checkout that provides the pinned manifest baseline."
    )
endif()

project(KBEngineNex VERSION 2.8.2 LANGUAGES C CXX)

# 生成器文件夹只影响 IDE 展示，不改变目标名称、链接关系或命令行构建接口。
# Generator folders affect only IDE presentation and do not change target names, linkage, or command-line build interfaces.
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "_CMake")

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
include(KbeRuntime)
include(KbeServerTarget)

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
add_subdirectory(server/machine)
add_subdirectory(server/baseappmgr)
add_subdirectory(server/cellappmgr)
add_subdirectory(server/dbmgr)
add_subdirectory(server/loginapp)
add_subdirectory(server/baseapp)
add_subdirectory(server/cellapp)
add_subdirectory(server/tools/bots)
add_subdirectory(server/tools/logger)
add_subdirectory(server/tools/interfaces)
add_subdirectory(server/tools/kbcmd)

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

# 聚合目标覆盖全部跨平台服务进程和命令行工具；MFC guiconsole 继续由既有 VS 工程维护。
# The aggregate target covers every cross-platform server process and command-line tool; the MFC guiconsole remains maintained by the existing VS project.
add_custom_target(kbe_servers DEPENDS
    machine
    baseappmgr
    cellappmgr
    dbmgr
    loginapp
    baseapp
    cellapp
    bots
    logger
    interfaces
    kbcmd
)

kbe_configure_runtime()

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# 按职责组织生成的 Visual Studio solution；同一分组也会被支持 folder 的其他 IDE 生成器复用。
# Organize the generated Visual Studio solution by responsibility; other IDE generators with folder support reuse the same grouping.
function(kbe_set_target_folder folder_name)
    foreach(_kbe_target IN LISTS ARGN)
        if(TARGET "${_kbe_target}")
            set_target_properties("${_kbe_target}" PROPERTIES FOLDER "${folder_name}")
        endif()
    endforeach()
endfunction()

kbe_set_target_folder("Engine/Core"
    common math thread xml helper resmgr network pyscript entitydef
)
kbe_set_target_folder("Engine/Runtime"
    client_lib navigation server kbe_build_options kbe_python_runtime
)
kbe_set_target_folder("Engine/Database"
    db_interface db_mysql db_redis db_mongodb db_postgresql
)
kbe_set_target_folder("Engine/ThirdParty"
    sigar jwsmtp tmxparser
)
kbe_set_target_folder("Server/Components"
    machine baseappmgr cellappmgr dbmgr loginapp baseapp cellapp
)
kbe_set_target_folder("Server/Tools"
    bots logger interfaces kbcmd
)
kbe_set_target_folder("Build/Aggregates"
    kbe_core kbe_core_dependencies kbe_libraries kbe_servers
    kbe_python_stdlib kbe_python_runtime_files kbe_runtime
)

if(BUILD_TESTING)
    # 统一测试构建入口包含 CTest 所需运行时和编译型 fixture，避免先运行 CTest 才发现测试程序尚未生成。
    # The unified test build entry includes the runtime and compiled fixtures required by CTest, avoiding late missing-executable failures.
    add_custom_target(kbe_tests DEPENDS
        kbe_runtime
        kbe_sdk_tests
        kbe_network_tests
        kbe_common_tests
        kbe_security_tests
    )
    kbe_set_target_folder("Build/Aggregates" kbe_tests)
endif()
