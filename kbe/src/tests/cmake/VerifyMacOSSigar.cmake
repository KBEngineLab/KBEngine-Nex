if(NOT DEFINED KBE_SIGAR_ROOT OR NOT IS_DIRECTORY "${KBE_SIGAR_ROOT}")
    message(FATAL_ERROR "KBE_SIGAR_ROOT must identify the bundled sigar source tree")
endif()

set(_kbe_macos_source "${KBE_SIGAR_ROOT}/macos/sigar_macos.c")
set(_kbe_macos_header "${KBE_SIGAR_ROOT}/macos/sigar.h")
set(_kbe_macos_format_header "${KBE_SIGAR_ROOT}/macos/sigar_format.h")
foreach(_kbe_required_file IN ITEMS
    "${_kbe_macos_source}"
    "${_kbe_macos_header}"
    "${_kbe_macos_format_header}"
)
    if(NOT EXISTS "${_kbe_required_file}")
        message(FATAL_ERROR "macOS sigar source contract is missing: ${_kbe_required_file}")
    endif()
endforeach()

file(READ "${_kbe_macos_source}" _kbe_macos_source_text)
foreach(_kbe_required_api IN ITEMS
    sigar_open
    sigar_close
    sigar_strerror
    sigar_mem_get
    sigar_cpu_get
    sigar_cpu_list_get
    sigar_cpu_list_destroy
    sigar_proc_list_get
    sigar_proc_list_destroy
    sigar_proc_state_get
    sigar_proc_time_get
    sigar_proc_cpu_get
    sigar_proc_mem_get
    sigar_cpu_perc_calculate
)
    string(FIND "${_kbe_macos_source_text}" " ${_kbe_required_api}(" _kbe_api_position)
    if(_kbe_api_position EQUAL -1)
        message(FATAL_ERROR "macOS sigar does not implement required API: ${_kbe_required_api}")
    endif()
endforeach()

file(READ "${_kbe_macos_header}" _kbe_macos_header_text)
file(READ "${_kbe_macos_format_header}" _kbe_macos_format_header_text)
if(NOT _kbe_macos_header_text MATCHES "linux/sigar\\.h" OR
   NOT _kbe_macos_format_header_text MATCHES "linux/sigar_format\\.h")
    message(FATAL_ERROR "macOS sigar wrappers must reuse the shared public API declarations")
endif()

# 这是源码完整性门禁，不代表 Apple SDK 编译或 macOS 运行验证。
# This is a source-integrity gate and does not claim Apple SDK compilation or macOS runtime validation.
message(STATUS "Verified the macOS sigar source contract; native compilation remains a macOS acceptance requirement")
