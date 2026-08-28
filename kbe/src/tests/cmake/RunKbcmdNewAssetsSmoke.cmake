foreach(_kbe_required_argument IN ITEMS KBE_EXECUTABLE KBE_REPOSITORY_ROOT KBE_OUTPUT_ROOT KBE_BINARY_ROOT)
    if(NOT DEFINED ${_kbe_required_argument} OR "${${_kbe_required_argument}}" STREQUAL "")
        message(FATAL_ERROR "${_kbe_required_argument} is required")
    endif()
endforeach()

cmake_path(IS_PREFIX KBE_BINARY_ROOT "${KBE_OUTPUT_ROOT}" NORMALIZE _kbe_output_is_safe)
if(NOT _kbe_output_is_safe)
    message(FATAL_ERROR "New assets smoke output must stay under the CMake binary tree: ${KBE_OUTPUT_ROOT}")
endif()

file(REMOVE_RECURSE "${KBE_OUTPUT_ROOT}")
get_filename_component(_kbe_binary_dir "${KBE_EXECUTABLE}" DIRECTORY)

# 只提供进程静态初始化所需的引擎资源，不提供任何项目 entities.xml/types.xml。
# Only engine resources required by process-wide static initialization are provided; no project entities.xml/types.xml exist.
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "KBE_ROOT=${KBE_REPOSITORY_ROOT}"
        "KBE_RES_PATH=${KBE_REPOSITORY_ROOT}/kbe/res"
        "KBE_BIN_PATH=${_kbe_binary_dir}"
        "${KBE_EXECUTABLE}"
        --newassets=python
        "--outpath=${KBE_OUTPUT_ROOT}"
    RESULT_VARIABLE _kbe_new_assets_result
    OUTPUT_VARIABLE _kbe_new_assets_stdout
    ERROR_VARIABLE _kbe_new_assets_stderr
    TIMEOUT 60
)
if(NOT _kbe_new_assets_result EQUAL 0)
    message(FATAL_ERROR
        "kbcmd new assets smoke failed with exit code ${_kbe_new_assets_result}\n"
        "stdout:\n${_kbe_new_assets_stdout}\n"
        "stderr:\n${_kbe_new_assets_stderr}"
    )
endif()

set(_kbe_new_assets_log "${_kbe_new_assets_stdout}\n${_kbe_new_assets_stderr}")
if(_kbe_new_assets_log MATCHES "EntityDef" OR _kbe_new_assets_log MATCHES "TiXmlNode::openXML" OR
   _kbe_new_assets_log MATCHES "\\[ERROR\\]" OR _kbe_new_assets_log MATCHES "Traceback")
    message(FATAL_ERROR
        "kbcmd new assets smoke emitted an initialization error\n"
        "stdout:\n${_kbe_new_assets_stdout}\n"
        "stderr:\n${_kbe_new_assets_stderr}"
    )
endif()

set(_kbe_template_root "${KBE_REPOSITORY_ROOT}/kbe/res/sdk_templates/server/python_assets")
file(GLOB_RECURSE _kbe_template_files
    LIST_DIRECTORIES false
    RELATIVE "${_kbe_template_root}"
    "${_kbe_template_root}/*"
)
file(GLOB_RECURSE _kbe_generated_files
    LIST_DIRECTORIES false
    RELATIVE "${KBE_OUTPUT_ROOT}"
    "${KBE_OUTPUT_ROOT}/*"
)
list(SORT _kbe_template_files)
list(SORT _kbe_generated_files)

# 全量比较可以同时发现漏复制、意外新增和文本/二进制内容变化。
# A full comparison detects missing files, unexpected additions, and text or binary content changes.
if(NOT _kbe_template_files STREQUAL _kbe_generated_files)
    message(FATAL_ERROR
        "Generated assets file list differs from the source template\n"
        "template files: ${_kbe_template_files}\n"
        "generated files: ${_kbe_generated_files}"
    )
endif()

foreach(_kbe_relative_file IN LISTS _kbe_template_files)
    file(SHA256 "${_kbe_template_root}/${_kbe_relative_file}" _kbe_template_hash)
    file(SHA256 "${KBE_OUTPUT_ROOT}/${_kbe_relative_file}" _kbe_generated_hash)
    if(NOT _kbe_template_hash STREQUAL _kbe_generated_hash)
        message(FATAL_ERROR "Generated asset content differs for ${_kbe_relative_file}")
    endif()
endforeach()

list(LENGTH _kbe_generated_files _kbe_generated_file_count)
message(STATUS
    "kbcmd generated ${_kbe_generated_file_count} standalone Python server asset files under ${KBE_OUTPUT_ROOT}")

# KBE_RES_PATH 历史上允许调用者覆盖内置模板；根因修复不能改变这项资源优先级。
# KBE_RES_PATH historically allows callers to override built-in templates; the root-cause fix must preserve that priority.
set(_kbe_override_resource_root "${KBE_OUTPUT_ROOT}-override-resources")
set(_kbe_override_output_root "${KBE_OUTPUT_ROOT}-override-output")
file(REMOVE_RECURSE "${_kbe_override_resource_root}" "${_kbe_override_output_root}")
file(MAKE_DIRECTORY "${_kbe_override_resource_root}/sdk_templates/server")
file(COPY "${_kbe_template_root}" DESTINATION "${_kbe_override_resource_root}/sdk_templates/server")

set(_kbe_override_marker "KBE_NEW_ASSETS_OVERRIDE_MARKER")
set(_kbe_override_readme
    "${_kbe_override_resource_root}/sdk_templates/server/python_assets/README.md")
file(APPEND "${_kbe_override_readme}" "\n${_kbe_override_marker}\n")

if(WIN32)
    set(_kbe_path_separator ";")
else()
    set(_kbe_path_separator ":")
endif()
set(_kbe_override_res_path
    "${_kbe_override_resource_root}${_kbe_path_separator}${KBE_REPOSITORY_ROOT}/kbe/res")

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "KBE_ROOT=${KBE_REPOSITORY_ROOT}"
        "KBE_RES_PATH=${_kbe_override_res_path}"
        "KBE_BIN_PATH=${_kbe_binary_dir}"
        "${KBE_EXECUTABLE}"
        --newassets=python
        "--outpath=${_kbe_override_output_root}"
    RESULT_VARIABLE _kbe_override_result
    OUTPUT_VARIABLE _kbe_override_stdout
    ERROR_VARIABLE _kbe_override_stderr
    TIMEOUT 60
)
if(NOT _kbe_override_result EQUAL 0)
    message(FATAL_ERROR
        "kbcmd custom template override failed with exit code ${_kbe_override_result}\n"
        "stdout:\n${_kbe_override_stdout}\n"
        "stderr:\n${_kbe_override_stderr}"
    )
endif()

file(READ "${_kbe_override_output_root}/README.md" _kbe_generated_override_readme)
string(FIND "${_kbe_generated_override_readme}" "${_kbe_override_marker}" _kbe_override_marker_offset)
if(_kbe_override_marker_offset EQUAL -1)
    message(FATAL_ERROR "kbcmd did not preserve the caller's custom server assets template priority")
endif()
