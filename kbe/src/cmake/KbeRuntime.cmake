include_guard(GLOBAL)

function(kbe_prepare_runtime)
    set(_kbe_python_scripts_dir "${CMAKE_SOURCE_DIR}/../res/scripts/common")
    if(WIN32)
        set(_kbe_python_extensions_destination "${_kbe_python_scripts_dir}/DLLs")
    else()
        set(_kbe_python_extensions_destination "${_kbe_python_scripts_dir}/lib-dynload")
    endif()

    if(NOT IS_DIRECTORY "${KBE_PYTHON_STDLIB_SOURCE}")
        message(FATAL_ERROR
            "Python standard library directory does not exist: ${KBE_PYTHON_STDLIB_SOURCE}")
    endif()
    if(NOT IS_DIRECTORY "${KBE_PYTHON_EXTENSIONS_SOURCE}")
        message(FATAL_ERROR
            "Python extension directory does not exist: ${KBE_PYTHON_EXTENSIONS_SOURCE}")
    endif()

    set(KBE_PYTHON_SCRIPTS_DIR "${_kbe_python_scripts_dir}")
    set(KBE_PYTHON_EXTENSIONS_DESTINATION "${_kbe_python_extensions_destination}")
    set(KBE_PYTHON_DEPLOY_STAMP
        "${KBE_CMAKE_OUTPUT_ROOT}/runtime/python-${VCPKG_TARGET_TRIPLET}.stamp")
    set(KBE_PYTHON_DEPLOY_LOCK
        "${KBE_CMAKE_OUTPUT_ROOT}/runtime/python-${VCPKG_TARGET_TRIPLET}.lock")
    set(KBE_PYTHON_MANIFEST "${CMAKE_SOURCE_DIR}/vcpkg.json")
    set(KBE_LOG4J_SOURCE "${CMAKE_SOURCE_DIR}/../bin/server/log4j.properties")
    set(KBE_PYTHON_DEPLOY_SCRIPT
        "${CMAKE_BINARY_DIR}/cmake/KbeDeployPythonRuntime.cmake"
        CACHE INTERNAL "Generated Python runtime deployment script" FORCE)
    configure_file(
        "${CMAKE_SOURCE_DIR}/cmake/KbeDeployPythonRuntime.cmake.in"
        "${KBE_PYTHON_DEPLOY_SCRIPT}"
        @ONLY
    )

    # 所有服务进程共享同一套 Python 运行时。将部署建模为唯一构建目标，Ninja 会在
    # 并行链接前只执行一次，避免每个 POST_BUILD 进程在大目录复制期间争抢文件锁。
    # All server processes share one Python runtime. Model deployment as a single build target
    # so Ninja executes it once before parallel links instead of contending in every POST_BUILD.
    add_custom_target(kbe_python_runtime_deploy
        COMMAND "${CMAKE_COMMAND}" -P "${KBE_PYTHON_DEPLOY_SCRIPT}"
        BYPRODUCTS "${KBE_PYTHON_DEPLOY_STAMP}"
        VERBATIM
    )

endfunction()

function(kbe_configure_runtime)
    set(_kbe_python_scripts_dir "${CMAKE_SOURCE_DIR}/../res/scripts/common")
    if(WIN32)
        set(_kbe_python_extensions_destination "${_kbe_python_scripts_dir}/DLLs")
    else()
        set(_kbe_python_extensions_destination "${_kbe_python_scripts_dir}/lib-dynload")
    endif()

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

    endforeach()

    # 聚合构建始终修复完整运行目录，即使所有服务端链接产物已经是最新状态。
    # Aggregate builds always repair the runtime layout even when every server link artifact is current.
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
    add_dependencies(kbe_servers kbe_python_runtime_deploy)

    set(_kbe_runtime_python_files)
    foreach(_kbe_python_runtime_file IN LISTS KBE_PYTHON_RUNTIME_FILES)
        cmake_path(GET _kbe_python_runtime_file FILENAME _kbe_python_runtime_name)
        list(APPEND _kbe_runtime_python_files
            "${KBE_SERVER_RUNTIME_DIR}/${_kbe_python_runtime_name}")
    endforeach()

    cmake_path(GET KBE_PYTHON_EXTENSION_PROBE FILENAME _kbe_python_extension_name)
    set(KBE_RUNTIME_BIN_DIR "${KBE_SERVER_RUNTIME_DIR}"
        CACHE INTERNAL "CMake server runtime binary directory" FORCE)
    set(KBE_RUNTIME_PYTHON_FILES "${_kbe_runtime_python_files}"
        CACHE INTERNAL "Python runtime libraries deployed beside server executables" FORCE)
    set(KBE_RUNTIME_PYTHON_STDLIB "${_kbe_python_scripts_dir}/Lib/os.py"
        CACHE INTERNAL "Python standard-library runtime probe" FORCE)
    set(KBE_RUNTIME_PYTHON_EXTENSION
        "${_kbe_python_extensions_destination}/${_kbe_python_extension_name}"
        CACHE INTERNAL "Python extension runtime probe" FORCE)
endfunction()
