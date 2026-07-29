include_guard(GLOBAL)

function(kbe_configure_runtime)
    set(KBE_RUNTIME_BIN_DIR "${KBE_CMAKE_OUTPUT_ROOT}/bin/$<CONFIG>" PARENT_SCOPE)
    set(_kbe_python_scripts_dir "${CMAKE_SOURCE_DIR}/../res/scripts/common")
    set(_kbe_stdlib_stamp "${KBE_CMAKE_OUTPUT_ROOT}/runtime/python-stdlib-${KBE_PYTHON_TRIPLET}.stamp")

    if(WIN32)
        set(_kbe_python_stdlib_source "${KBE_PYTHON_ROOT}/tools/python3/Lib")
        set(_kbe_python_extensions_source "${KBE_PYTHON_ROOT}/tools/python3/DLLs")
        set(_kbe_python_runtime_files
            "${KBE_PYTHON_ROOT}/bin/python3.dll"
            "${KBE_PYTHON_ROOT}/bin/python${_kbe_python_abi}.dll"
        )
        set(_kbe_python_extension_probe "${_kbe_python_extensions_source}/_asyncio.pyd")
    else()
        set(_kbe_python_stdlib_source "${KBE_PYTHON_ROOT}/lib/python${KBE_PYTHON_VERSION}")
        set(_kbe_python_extensions_source "${_kbe_python_stdlib_source}/lib-dynload")
        file(GLOB _kbe_python_runtime_files CONFIGURE_DEPENDS
            "${KBE_PYTHON_ROOT}/lib/libpython${KBE_PYTHON_VERSION}*"
        )
        set(_kbe_python_extension_probe "${_kbe_python_extensions_source}/_asyncio*")
    endif()

    if(NOT IS_DIRECTORY "${_kbe_python_stdlib_source}")
        message(FATAL_ERROR "Python standard library directory does not exist: ${_kbe_python_stdlib_source}")
    endif()
    if(NOT IS_DIRECTORY "${_kbe_python_extensions_source}")
        message(FATAL_ERROR "Python extension directory does not exist: ${_kbe_python_extensions_source}")
    endif()
    if(NOT _kbe_python_runtime_files)
        message(FATAL_ERROR "Python runtime libraries were not found under ${KBE_PYTHON_ROOT}")
    endif()

    # 标准库部署与配置无关，使用 stamp 避免 Debug、Release 每次构建都复制数千个文件。
    # Standard-library deployment is configuration-independent; a stamp avoids copying thousands of files on every Debug and Release build.
    add_custom_command(
        OUTPUT "${_kbe_stdlib_stamp}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${_kbe_python_scripts_dir}/Lib"
            "${_kbe_python_scripts_dir}/DLLs"
            "${_kbe_python_scripts_dir}/lib-dynload"
            "${KBE_CMAKE_OUTPUT_ROOT}/runtime"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${_kbe_python_stdlib_source}"
            "${_kbe_python_scripts_dir}/Lib"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${_kbe_python_extensions_source}"
            "$<IF:$<BOOL:${WIN32}>,${_kbe_python_scripts_dir}/DLLs,${_kbe_python_scripts_dir}/lib-dynload>"
        COMMAND "${CMAKE_COMMAND}" -E touch "${_kbe_stdlib_stamp}"
        DEPENDS
            "${_kbe_python_stdlib_source}/os.py"
            "${KBE_PYTHON_INCLUDE_DIR}/Python.h"
        VERBATIM
    )
    add_custom_target(kbe_python_stdlib DEPENDS "${_kbe_stdlib_stamp}")

    set(_kbe_runtime_commands
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${KBE_CMAKE_OUTPUT_ROOT}/bin/$<CONFIG>"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_SOURCE_DIR}/../bin/server/log4j.properties"
            "${KBE_CMAKE_OUTPUT_ROOT}/bin/$<CONFIG>"
    )
    foreach(_kbe_runtime_file IN LISTS _kbe_python_runtime_files)
        if(NOT EXISTS "${_kbe_runtime_file}")
            message(FATAL_ERROR "Python runtime file does not exist: ${_kbe_runtime_file}")
        endif()
        list(APPEND _kbe_runtime_commands
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${_kbe_runtime_file}"
                "${KBE_CMAKE_OUTPUT_ROOT}/bin/$<CONFIG>"
        )
    endforeach()

    # 动态库按配置复制到可执行文件旁；copy_if_different 保持普通增量构建为常数级开销。
    # Runtime libraries are copied beside each configuration's executables; copy_if_different keeps normal incremental builds constant-cost.
    add_custom_target(kbe_runtime
        ${_kbe_runtime_commands}
        DEPENDS
            kbe_servers
            kbe_python_stdlib
        VERBATIM
    )

    set(KBE_RUNTIME_BIN_DIR "${KBE_CMAKE_OUTPUT_ROOT}/bin/$<CONFIG>" CACHE INTERNAL "CMake server runtime binary directory" FORCE)
    set(KBE_RUNTIME_PYTHON_STDLIB "${_kbe_python_scripts_dir}/Lib/os.py" CACHE INTERNAL "Python standard-library runtime probe" FORCE)
    set(KBE_RUNTIME_PYTHON_EXTENSION "${_kbe_python_extension_probe}" CACHE INTERNAL "Python extension runtime probe" FORCE)
endfunction()
