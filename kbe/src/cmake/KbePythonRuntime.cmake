include_guard(GLOBAL)

set(KBE_PYTHON_VERSION "3.12" CACHE STRING "Embedded Python major and minor version")
string(REPLACE "." "" KBE_PYTHON_ABI "${KBE_PYTHON_VERSION}")

if(NOT VCPKG_INSTALLED_DIR OR NOT VCPKG_TARGET_TRIPLET)
    message(FATAL_ERROR
        "Embedded Python must be resolved from the main vcpkg manifest. "
        "Configure with a preset that defines VCPKG_INSTALLED_DIR and VCPKG_TARGET_TRIPLET."
    )
endif()

# Python 与其余第三方库共享一个安装树；overlay triplet 只对 python3 port 切换为动态 Release。
# Python shares the main dependency tree; the overlay triplet switches only the python3 port to dynamic Release.
set(KBE_PYTHON_ROOT "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
cmake_path(ABSOLUTE_PATH KBE_PYTHON_ROOT
    BASE_DIRECTORY "${CMAKE_SOURCE_DIR}"
    NORMALIZE
    OUTPUT_VARIABLE KBE_PYTHON_ROOT)
set(KBE_PYTHON_INCLUDE_DIR "${KBE_PYTHON_ROOT}/include/python${KBE_PYTHON_VERSION}")

if(WIN32)
    set(KBE_PYTHON_LIBRARY "${KBE_PYTHON_ROOT}/lib/python${KBE_PYTHON_ABI}.lib")
    set(KBE_PYTHON_RUNTIME_LIBRARY "${KBE_PYTHON_ROOT}/bin/python${KBE_PYTHON_ABI}.dll")
    set(KBE_PYTHON_RUNTIME_FILES
        "${KBE_PYTHON_ROOT}/bin/python3.dll"
        "${KBE_PYTHON_RUNTIME_LIBRARY}"
    )
    set(KBE_PYTHON_STDLIB_SOURCE "${KBE_PYTHON_ROOT}/tools/python3/Lib")
    set(KBE_PYTHON_EXTENSIONS_SOURCE "${KBE_PYTHON_ROOT}/tools/python3/DLLs")
    set(KBE_PYTHON_EXTENSION_PROBE "${KBE_PYTHON_EXTENSIONS_SOURCE}/_asyncio.pyd")
elseif(APPLE)
    set(KBE_PYTHON_LIBRARY "${KBE_PYTHON_ROOT}/lib/libpython${KBE_PYTHON_VERSION}.dylib")
    set(KBE_PYTHON_RUNTIME_LIBRARY "${KBE_PYTHON_LIBRARY}")
    file(GLOB KBE_PYTHON_RUNTIME_FILES CONFIGURE_DEPENDS
        "${KBE_PYTHON_ROOT}/lib/libpython3*.dylib")
    set(KBE_PYTHON_STDLIB_SOURCE "${KBE_PYTHON_ROOT}/lib/python${KBE_PYTHON_VERSION}")
    set(KBE_PYTHON_EXTENSIONS_SOURCE "${KBE_PYTHON_STDLIB_SOURCE}/lib-dynload")
    file(GLOB _kbe_python_extension_probes
        "${KBE_PYTHON_EXTENSIONS_SOURCE}/_asyncio*.so")
    if(_kbe_python_extension_probes)
        list(GET _kbe_python_extension_probes 0 KBE_PYTHON_EXTENSION_PROBE)
    endif()
else()
    set(KBE_PYTHON_LIBRARY "${KBE_PYTHON_ROOT}/lib/libpython${KBE_PYTHON_VERSION}.so")
    set(KBE_PYTHON_RUNTIME_LIBRARY "${KBE_PYTHON_LIBRARY}")
    file(GLOB KBE_PYTHON_RUNTIME_FILES CONFIGURE_DEPENDS
        "${KBE_PYTHON_ROOT}/lib/libpython3*.so*")
    set(KBE_PYTHON_STDLIB_SOURCE "${KBE_PYTHON_ROOT}/lib/python${KBE_PYTHON_VERSION}")
    set(KBE_PYTHON_EXTENSIONS_SOURCE "${KBE_PYTHON_STDLIB_SOURCE}/lib-dynload")
    file(GLOB _kbe_python_extension_probes
        "${KBE_PYTHON_EXTENSIONS_SOURCE}/_asyncio*.so")
    if(_kbe_python_extension_probes)
        list(GET _kbe_python_extension_probes 0 KBE_PYTHON_EXTENSION_PROBE)
    endif()
endif()

foreach(_kbe_python_required_path IN ITEMS
    "${KBE_PYTHON_INCLUDE_DIR}/Python.h"
    "${KBE_PYTHON_LIBRARY}"
    "${KBE_PYTHON_RUNTIME_LIBRARY}"
    "${KBE_PYTHON_STDLIB_SOURCE}/os.py"
    "${KBE_PYTHON_EXTENSION_PROBE}"
)
    if(NOT EXISTS "${_kbe_python_required_path}")
        message(FATAL_ERROR
            "Embedded Python ${KBE_PYTHON_VERSION} is incomplete: ${_kbe_python_required_path}")
    endif()
endforeach()

if(NOT KBE_PYTHON_RUNTIME_FILES)
    message(FATAL_ERROR "Python runtime libraries were not found under ${KBE_PYTHON_ROOT}")
endif()
list(SORT KBE_PYTHON_RUNTIME_FILES)

# 只暴露一个 imported target，不生成新的 VS/CMake 工程；所有配置都映射到 Release Python。
# A single imported target adds no IDE project, and every engine configuration maps to Release Python.
add_library(KBE::PythonRuntime SHARED IMPORTED GLOBAL)
set_target_properties(KBE::PythonRuntime PROPERTIES
    IMPORTED_CONFIGURATIONS RELEASE
    IMPORTED_LOCATION_RELEASE "${KBE_PYTHON_RUNTIME_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${KBE_PYTHON_INCLUDE_DIR}"
    MAP_IMPORTED_CONFIG_DEBUG Release
    MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release
    MAP_IMPORTED_CONFIG_MINSIZEREL Release
)

if(WIN32)
    set_target_properties(KBE::PythonRuntime PROPERTIES
        IMPORTED_IMPLIB_RELEASE "${KBE_PYTHON_LIBRARY}"
    )
endif()
