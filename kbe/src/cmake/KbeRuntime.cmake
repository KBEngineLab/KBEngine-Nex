include_guard(GLOBAL)

function(kbe_configure_runtime)
    set(KBE_RUNTIME_BIN_DIR "${KBE_SERVER_RUNTIME_DIR}" PARENT_SCOPE)
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
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${KBE_SERVER_RUNTIME_DIR}"
    )
    # 默认输出目录已经保存 log4j 配置；自定义输出目录时才复制，避免源和目标为同一文件。
    # The default output already owns the log4j configuration; copy only for custom output directories to avoid a source-to-self operation.
    set(_kbe_log4j_source "${CMAKE_SOURCE_DIR}/../bin/server/log4j.properties")
    cmake_path(NORMAL_PATH _kbe_log4j_source)
    set(_kbe_log4j_destination "${KBE_SERVER_RUNTIME_DIR}/log4j.properties")
    cmake_path(NORMAL_PATH _kbe_log4j_destination)
    if(NOT _kbe_log4j_source STREQUAL _kbe_log4j_destination)
        list(APPEND _kbe_runtime_commands
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${_kbe_log4j_source}"
                "${KBE_SERVER_RUNTIME_DIR}"
        )
    endif()
    foreach(_kbe_runtime_file IN LISTS _kbe_python_runtime_files)
        if(NOT EXISTS "${_kbe_runtime_file}")
            message(FATAL_ERROR "Python runtime file does not exist: ${_kbe_runtime_file}")
        endif()
        list(APPEND _kbe_runtime_commands
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${_kbe_runtime_file}"
                "${KBE_SERVER_RUNTIME_DIR}"
        )
    endforeach()

    # 运行库部署必须是可执行目标的前置依赖，保证单独构建任一组件也能直接启动，而不必额外构建聚合目标。
    # Runtime deployment must precede executable targets so an individually built component can start without requiring a separate aggregate build.
    add_custom_target(kbe_python_runtime_files
        ${_kbe_runtime_commands}
        VERBATIM
    )

    set(_kbe_runtime_targets
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
    foreach(_kbe_runtime_target IN LISTS _kbe_runtime_targets)
        if(NOT TARGET "${_kbe_runtime_target}")
            message(FATAL_ERROR
                "Runtime executable target does not exist: ${_kbe_runtime_target}")
        endif()

        # 动态库复制由共享目标执行一次，标准库使用 stamp 增量部署；并行构建不会同时写入同一运行时文件。
        # A shared target copies dynamic libraries once while the stamped standard-library target deploys incrementally, avoiding concurrent writes during parallel builds.
        add_dependencies("${_kbe_runtime_target}"
            kbe_python_runtime_files
            kbe_python_stdlib
        )
    endforeach()

    # 聚合目标每次构建都重新部署所选配置，即使该配置的链接产物本身已经是最新状态。
    # The aggregate target redeploys the selected configuration on every build even when its link artifacts are already up to date.
    set(_kbe_server_deploy_commands
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${KBE_SERVER_RUNTIME_DIR}"
    )
    foreach(_kbe_runtime_target IN LISTS _kbe_runtime_targets)
        list(APPEND _kbe_server_deploy_commands
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:${_kbe_runtime_target}>"
                "${KBE_SERVER_RUNTIME_DIR}"
        )
    endforeach()
    add_custom_command(TARGET kbe_servers POST_BUILD
        ${_kbe_server_deploy_commands}
        VERBATIM
    )

    # 聚合目标保留为完整运行时入口，但部署正确性不再依赖调用方必须记住构建该目标。
    # The aggregate target remains the complete runtime entry point, while deployment correctness no longer depends on callers remembering to build it.
    add_custom_target(kbe_runtime
        DEPENDS
            kbe_servers
            kbe_python_runtime_files
            kbe_python_stdlib
    )

    set(KBE_RUNTIME_BIN_DIR "${KBE_SERVER_RUNTIME_DIR}" CACHE INTERNAL "CMake server runtime binary directory" FORCE)
    set(KBE_RUNTIME_PYTHON_STDLIB "${_kbe_python_scripts_dir}/Lib/os.py" CACHE INTERNAL "Python standard-library runtime probe" FORCE)
    set(KBE_RUNTIME_PYTHON_EXTENSION "${_kbe_python_extension_probe}" CACHE INTERNAL "Python extension runtime probe" FORCE)
endfunction()
