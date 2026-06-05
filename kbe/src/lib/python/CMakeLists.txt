project(embed_python NONE)

# --------------------------------------------------------
# 0) Python 源码和构建目录
# --------------------------------------------------------
set(PYTHON_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(PYTHON_BUILD_DIR ${PYTHON_SRC_DIR}/build)

# KBE 的 Debug 构建仍使用 Python Release ABI，避免第三方 Release wheel/.so
# 在 Debug Python ABI 下无法加载。
set(PYTHON_LIB_RELEASE ${PYTHON_SRC_DIR}/libpython3.13.a)
set(PYTHON_LIB ${PYTHON_LIB_RELEASE})

# --------------------------------------------------------
# 1) KBE Python 配置
# --------------------------------------------------------
if(NOT DEFINED KBE_ROOT)
    string(REGEX REPLACE "/kbe/src(/.*)?$" "" KBE_ROOT "${CMAKE_SOURCE_DIR}")
endif()

set(KBE_PYTHON_INCLUDE_DIRS
    ${KBE_ROOT}/kbe/src/lib/python/Include
    ${KBE_ROOT}/kbe/src/lib/python
    CACHE PATH "KBE Python include dirs"
)

set(KBE_PYTHON_LIB
    ${KBE_ROOT}/kbe/src/libs/libpython.a
    CACHE FILEPATH "KBE Python static library"
)

set(KBE_PYTHON_EXTRA_LIBS pthread util dl)

set(RES_SCRIPTS_DIR ${KBE_ROOT}/kbe/res/scripts/common)
set(LIB_DIR ${KBE_ROOT}/kbe/src/libs)
set(KBE_CMAKEFILE_DIR ${KBE_ROOT}/kbe/src/cmakefile)

# Keep Python's standalone configure/make build flags aligned with CMake.
set(PYTHON_BUILD_ENV
    CC=${CMAKE_C_COMPILER}
    CXX=${CMAKE_CXX_COMPILER}
)

if(APPLE)
    set(PYTHON_MACOSX_DEPLOYMENT_TARGET "")

    if(CMAKE_OSX_DEPLOYMENT_TARGET)
        set(PYTHON_MACOSX_DEPLOYMENT_TARGET "${CMAKE_OSX_DEPLOYMENT_TARGET}")
    elseif(DEFINED ENV{MACOSX_DEPLOYMENT_TARGET} AND NOT "$ENV{MACOSX_DEPLOYMENT_TARGET}" STREQUAL "")
        set(PYTHON_MACOSX_DEPLOYMENT_TARGET "$ENV{MACOSX_DEPLOYMENT_TARGET}")
    else()
        execute_process(
            COMMAND sw_vers -productVersion
            OUTPUT_VARIABLE _kbe_macos_version
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        string(REGEX MATCH "^[0-9]+" _kbe_macos_major "${_kbe_macos_version}")
        if(_kbe_macos_major)
            set(PYTHON_MACOSX_DEPLOYMENT_TARGET "${_kbe_macos_major}.0")
        endif()
    endif()

    if(PYTHON_MACOSX_DEPLOYMENT_TARGET)
        list(APPEND PYTHON_BUILD_ENV MACOSX_DEPLOYMENT_TARGET=${PYTHON_MACOSX_DEPLOYMENT_TARGET})
        message(STATUS "Embedded Python deployment target: ${PYTHON_MACOSX_DEPLOYMENT_TARGET}")
    endif()

    if(CMAKE_OSX_SYSROOT)
        list(APPEND PYTHON_BUILD_ENV SDKROOT=${CMAKE_OSX_SYSROOT})
    endif()
endif()

# Post-build helper scripts (runtime glob/copy is safer than hardcoded paths)
set(PYTHON_COPY_DYNLOAD_SCRIPT ${CMAKE_CURRENT_BINARY_DIR}/copy_python_dynload.cmake)
configure_file(
    ${KBE_CMAKEFILE_DIR}/copy_python_dynload.cmake.in
    ${PYTHON_COPY_DYNLOAD_SCRIPT}
    @ONLY
)

set(PYTHON_COPY_LIB_SCRIPT ${CMAKE_CURRENT_BINARY_DIR}/copy_python_lib.cmake)
configure_file(
    ${KBE_CMAKEFILE_DIR}/copy_python_lib.cmake.in
    ${PYTHON_COPY_LIB_SCRIPT}
    @ONLY
)

# --------------------------------------------------------
# 2) 编译 Python
# --------------------------------------------------------
# Release 版本
add_custom_target(python_build ALL
    COMMAND test -f ${PYTHON_LIB_RELEASE} || (
        chmod +x configure &&
        ${CMAKE_COMMAND} -E env ${PYTHON_BUILD_ENV}
        ./configure --without-ensurepip &&
        make -j
    )
    BYPRODUCTS ${PYTHON_LIB_RELEASE}
    WORKING_DIRECTORY ${PYTHON_SRC_DIR}
    COMMENT "Building embedded Python (Release)..."
    USES_TERMINAL
)

# --------------------------------------------------------
# 3) 拷贝 libpython.a
# --------------------------------------------------------
add_custom_command(TARGET python_build POST_BUILD
    BYPRODUCTS
        ${LIB_DIR}/libpython.a
    COMMAND ${CMAKE_COMMAND} -E make_directory ${LIB_DIR}
    COMMAND ${CMAKE_COMMAND} -E remove -f
        ${LIB_DIR}/libpython.a
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${PYTHON_LIB}
        ${LIB_DIR}/libpython.a
    COMMENT "Copying libpython.a to ${LIB_DIR}"
)

# --------------------------------------------------------
# 4) 拷贝 lib-dynload
# --------------------------------------------------------
add_custom_command(TARGET python_build POST_BUILD
    COMMAND ${CMAKE_COMMAND} -P ${PYTHON_COPY_DYNLOAD_SCRIPT}
    COMMENT "Copying Python extension modules"
)

# --------------------------------------------------------
# 5) 拷贝 Lib
# --------------------------------------------------------
add_custom_command(TARGET python_build POST_BUILD
    COMMAND ${CMAKE_COMMAND} -P ${PYTHON_COPY_LIB_SCRIPT}
    COMMENT "Copying Python Lib directory"
)

# --------------------------------------------------------
# 6) 定义 INTERFACE target
# --------------------------------------------------------
add_library(python_interface INTERFACE)
add_dependencies(python_interface python_build)

target_include_directories(python_interface INTERFACE ${KBE_PYTHON_INCLUDE_DIRS})

# Debug / Release 都链接 Python Release ABI
target_link_libraries(python_interface INTERFACE
    ${PYTHON_LIB_RELEASE}
    ${KBE_PYTHON_EXTRA_LIBS}
)
