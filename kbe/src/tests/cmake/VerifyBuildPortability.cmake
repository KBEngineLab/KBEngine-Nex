if(NOT DEFINED KBE_SOURCE_ROOT OR NOT IS_DIRECTORY "${KBE_SOURCE_ROOT}")
    message(FATAL_ERROR "KBE_SOURCE_ROOT must identify the KBEngine source tree")
endif()

file(READ "${KBE_SOURCE_ROOT}/CMakeLists.txt" _kbe_root_cmake)
file(READ "${KBE_SOURCE_ROOT}/cmake/KbeDeployPythonRuntime.cmake.in" _kbe_python_deploy)

function(kbe_require_literal source_text required_literal description)
    string(FIND "${source_text}" "${required_literal}" _kbe_required_position)
    if(_kbe_required_position EQUAL -1)
        message(FATAL_ERROR "Build portability contract is missing ${description}: ${required_literal}")
    endif()
endfunction()

# Script mode starts a fresh policy scope, so project() cannot provide CMP0057
# for the generated deployment script. 脚本模式使用独立策略域，必须自行声明版本。
kbe_require_literal(
    "${_kbe_python_deploy}"
    "cmake_minimum_required(VERSION 3.25)"
    "standalone deployment policy initialization"
)

# log4cxx exports bare -lapr names on Unix. Configuration-aware vcpkg directories
# must precede those names without selecting one PkgConfig configuration globally.
# Unix log4cxx 导出裸 APR 库名，必须按构建配置提供 vcpkg 搜索目录。
foreach(_kbe_required IN ITEMS
    "target_link_directories(kbe_log4cxx INTERFACE"
    "$<$<CONFIG:Debug>:\${_kbe_unix_vcpkg_root}/debug/lib>"
    "$<$<NOT:$<CONFIG:Debug>>:\${_kbe_unix_vcpkg_root}/lib>"
)
    kbe_require_literal("${_kbe_root_cmake}" "${_kbe_required}" "configuration-aware APR search path")
endforeach()

string(FIND "${_kbe_root_cmake}" "PkgConfig::KBE_APR" _kbe_pkgconfig_target_position)
if(NOT _kbe_pkgconfig_target_position EQUAL -1)
    message(FATAL_ERROR "Multi-Config Unix builds must not link configure-time APR PkgConfig targets")
endif()

message(STATUS "Build portability source contract verified")
