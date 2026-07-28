include_guard(GLOBAL)

set(KBE_PYTHON_VERSION "3.12" CACHE STRING "Embedded Python major and minor version")

# Python 使用独立的动态 triplet，避免主工程的静态依赖策略改变 CPython ABI 和第三方扩展加载方式。
# Python uses an independent dynamic triplet so the main project's static dependency policy cannot alter the CPython ABI or extension loading behavior.
if(NOT KBE_PYTHON_TRIPLET)
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _kbe_python_processor)
    if(_kbe_python_processor MATCHES "^(arm64|aarch64)$" OR CMAKE_VS_PLATFORM_NAME STREQUAL "ARM64")
        set(_kbe_python_arch "arm64")
    elseif(_kbe_python_processor MATCHES "^(amd64|x86_64|x64)$" OR CMAKE_VS_PLATFORM_NAME STREQUAL "x64")
        set(_kbe_python_arch "x64")
    else()
        message(FATAL_ERROR "Unsupported Python architecture: ${CMAKE_SYSTEM_PROCESSOR}")
    endif()

    if(WIN32)
        set(_kbe_python_platform "windows")
    elseif(APPLE)
        set(_kbe_python_platform "osx")
    elseif(UNIX)
        set(_kbe_python_platform "linux")
    else()
        message(FATAL_ERROR "Unsupported Python platform: ${CMAKE_SYSTEM_NAME}")
    endif()

    set(KBE_PYTHON_TRIPLET "${_kbe_python_arch}-${_kbe_python_platform}" CACHE STRING "vcpkg triplet for the embedded Python runtime")
endif()

set(KBE_PYTHON_INSTALL_ROOT
    "${CMAKE_SOURCE_DIR}/python-runtime/vcpkg_installed"
    CACHE PATH "Independent vcpkg installation root for embedded Python"
)
set(KBE_PYTHON_ROOT "${KBE_PYTHON_INSTALL_ROOT}/${KBE_PYTHON_TRIPLET}")
set(KBE_PYTHON_INCLUDE_DIR "${KBE_PYTHON_ROOT}/include/python${KBE_PYTHON_VERSION}")

if(WIN32)
    string(REPLACE "." "" _kbe_python_abi "${KBE_PYTHON_VERSION}")
    set(KBE_PYTHON_LIBRARY "${KBE_PYTHON_ROOT}/lib/python${_kbe_python_abi}.lib")
else()
    find_library(KBE_PYTHON_LIBRARY
        NAMES "python${KBE_PYTHON_VERSION}"
        PATHS "${KBE_PYTHON_ROOT}/lib"
        NO_DEFAULT_PATH
    )
endif()

# 缺失运行时时复用 python-runtime manifest 自动安装；普通增量配置只做文件校验，不重复调用 vcpkg。
# Reuse the python-runtime manifest for automatic installation when absent; normal incremental configure only validates files and does not invoke vcpkg again.
if(NOT EXISTS "${KBE_PYTHON_INCLUDE_DIR}/Python.h" OR NOT EXISTS "${KBE_PYTHON_LIBRARY}")
    if(DEFINED ENV{VCPKG_ROOT})
        set(_kbe_vcpkg_root "$ENV{VCPKG_ROOT}")
    elseif(CMAKE_TOOLCHAIN_FILE MATCHES "[/\\\\]scripts[/\\\\]buildsystems[/\\\\]vcpkg\\.cmake$")
        get_filename_component(_kbe_vcpkg_root "${CMAKE_TOOLCHAIN_FILE}/../../.." ABSOLUTE)
    endif()

    find_program(_kbe_vcpkg_executable
        NAMES vcpkg vcpkg.exe
        HINTS "${_kbe_vcpkg_root}"
        NO_DEFAULT_PATH
    )
    if(NOT _kbe_vcpkg_executable)
        message(FATAL_ERROR "Embedded Python is missing and vcpkg was not found. Set VCPKG_ROOT.")
    endif()

    execute_process(
        COMMAND "${_kbe_vcpkg_executable}"
            install
            --triplet "${KBE_PYTHON_TRIPLET}"
            --x-manifest-root "${CMAKE_SOURCE_DIR}/python-runtime"
            --x-install-root "${KBE_PYTHON_INSTALL_ROOT}"
        RESULT_VARIABLE _kbe_python_install_result
        COMMAND_ECHO STDOUT
    )
    if(NOT _kbe_python_install_result EQUAL 0)
        message(FATAL_ERROR "vcpkg failed to install the embedded Python runtime: ${_kbe_python_install_result}")
    endif()

    if(NOT WIN32)
        find_library(KBE_PYTHON_LIBRARY
            NAMES "python${KBE_PYTHON_VERSION}"
            PATHS "${KBE_PYTHON_ROOT}/lib"
            NO_DEFAULT_PATH
            NO_CACHE
        )
    endif()
endif()

if(NOT EXISTS "${KBE_PYTHON_INCLUDE_DIR}/Python.h" OR NOT EXISTS "${KBE_PYTHON_LIBRARY}")
    message(FATAL_ERROR "Embedded Python ${KBE_PYTHON_VERSION} is incomplete under ${KBE_PYTHON_ROOT}")
endif()

add_library(kbe_python_runtime INTERFACE)
add_library(KBE::PythonRuntime ALIAS kbe_python_runtime)
target_include_directories(kbe_python_runtime INTERFACE "${KBE_PYTHON_INCLUDE_DIR}")
target_link_libraries(kbe_python_runtime INTERFACE "${KBE_PYTHON_LIBRARY}")

if(CMAKE_DL_LIBS)
    target_link_libraries(kbe_python_runtime INTERFACE "${CMAKE_DL_LIBS}")
endif()
