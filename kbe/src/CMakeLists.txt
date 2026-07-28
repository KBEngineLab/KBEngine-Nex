cmake_minimum_required(VERSION 3.25)

# 当前锁定的 vcpkg 基线仍包含声明 CMake 3.5 以下兼容策略的端口；CMake 4 会在依赖安装阶段中途失败。
# The pinned vcpkg baseline still contains ports declaring pre-3.5 compatibility policies; CMake 4 fails partway through dependency installation.
if(CMAKE_VERSION VERSION_GREATER_EQUAL 4.0)
    message(FATAL_ERROR
        "The current vcpkg baseline requires CMake 3.x. "
        "On Windows, use the CMake 3.31 bundled with Visual Studio 2022."
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

find_package(fmt CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

add_subdirectory(lib/common)
add_subdirectory(lib/math)
add_subdirectory(lib/thread)
add_subdirectory(lib/xml)

# 首阶段只迁移无 Python 和数据库依赖的底层库，后续目标按依赖拓扑逐层接入。
# The first stage migrates foundational libraries without Python or database dependencies; later targets are added in dependency order.
add_custom_target(kbe_core DEPENDS common math thread xml)
