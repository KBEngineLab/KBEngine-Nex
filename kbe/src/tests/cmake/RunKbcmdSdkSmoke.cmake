foreach(_kbe_required_argument IN ITEMS KBE_EXECUTABLE KBE_REPOSITORY_ROOT KBE_ASSETS_ROOT KBE_OUTPUT_ROOT KBE_BINARY_ROOT)
    if(NOT DEFINED ${_kbe_required_argument} OR "${${_kbe_required_argument}}" STREQUAL "")
        message(FATAL_ERROR "${_kbe_required_argument} is required")
    endif()
endforeach()

cmake_path(IS_PREFIX KBE_BINARY_ROOT "${KBE_OUTPUT_ROOT}" NORMALIZE _kbe_output_is_safe)
if(NOT _kbe_output_is_safe)
    message(FATAL_ERROR "SDK smoke output must stay under the CMake binary tree: ${KBE_OUTPUT_ROOT}")
endif()

file(REMOVE_RECURSE "${KBE_OUTPUT_ROOT}")
file(MAKE_DIRECTORY "${KBE_OUTPUT_ROOT}")
get_filename_component(_kbe_binary_dir "${KBE_EXECUTABLE}" DIRECTORY)

if(WIN32)
    set(_kbe_path_separator ";")
else()
    set(_kbe_path_separator ":")
endif()
set(_kbe_res_path
    "${KBE_REPOSITORY_ROOT}/kbe/res${_kbe_path_separator}${KBE_ASSETS_ROOT}${_kbe_path_separator}${KBE_ASSETS_ROOT}/scripts${_kbe_path_separator}${KBE_ASSETS_ROOT}/res"
)

# 使用真实 SDK 生成路径同时覆盖动态 Python、隔离搜索路径、EntityDef 和生成器初始化。
# The real SDK generation path jointly covers dynamic Python, isolated search paths, EntityDef, and generator initialization.
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "KBE_ROOT=${KBE_REPOSITORY_ROOT}"
        "KBE_RES_PATH=${_kbe_res_path}"
        "KBE_BIN_PATH=${_kbe_binary_dir}"
        "${KBE_EXECUTABLE}"
        --clientsdk=typescript
        "--outpath=${KBE_OUTPUT_ROOT}"
    RESULT_VARIABLE _kbe_sdk_result
    OUTPUT_VARIABLE _kbe_sdk_stdout
    ERROR_VARIABLE _kbe_sdk_stderr
    TIMEOUT 150
)
if(NOT _kbe_sdk_result EQUAL 0)
    message(FATAL_ERROR
        "kbcmd TypeScript SDK smoke failed with exit code ${_kbe_sdk_result}\n"
        "stdout:\n${_kbe_sdk_stdout}\n"
        "stderr:\n${_kbe_sdk_stderr}"
    )
endif()

set(_kbe_sdk_log "${_kbe_sdk_stdout}\n${_kbe_sdk_stderr}")
if(_kbe_sdk_log MATCHES "\\[ERROR\\]" OR _kbe_sdk_log MATCHES "Traceback")
    message(FATAL_ERROR
        "kbcmd TypeScript SDK smoke emitted an error despite returning success\n"
        "stdout:\n${_kbe_sdk_stdout}\n"
        "stderr:\n${_kbe_sdk_stderr}"
    )
endif()

file(GLOB_RECURSE _kbe_generated_typescript "${KBE_OUTPUT_ROOT}/*.ts")
list(LENGTH _kbe_generated_typescript _kbe_generated_count)
if(_kbe_generated_count LESS 10)
    message(FATAL_ERROR "kbcmd generated only ${_kbe_generated_count} TypeScript files under ${KBE_OUTPUT_ROOT}")
endif()

message(STATUS "kbcmd generated ${_kbe_generated_count} TypeScript SDK files")
