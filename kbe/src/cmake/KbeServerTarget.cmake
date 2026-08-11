include_guard(GLOBAL)

# 服务端进程共享 ABI、Python 和平台链接规则；组件目录只声明自己的源码、宏与附加依赖。
# Server processes share ABI, Python, and platform link rules; component directories declare only their sources, macros, and additional dependencies.
function(kbe_add_server_executable target_name component_definition)
    cmake_parse_arguments(KBE_SERVER_TARGET
        "NO_SERVER"
        ""
        "SOURCES;DEFINITIONS;LIBRARIES;WINDOWS_LIBRARIES"
        ${ARGN}
    )

    if(NOT KBE_SERVER_TARGET_SOURCES)
        message(FATAL_ERROR "${target_name} must declare SOURCES")
    endif()

    add_executable(${target_name} ${KBE_SERVER_TARGET_SOURCES})
    kbe_configure_target(${target_name})
    # 各配置必须保留独立链接产物，避免多配置生成器把较新的 Debug 文件误判为已完成的 Release 输出。
    # Configurations retain separate link artifacts so a multi-config generator cannot mistake a newer Debug file for a completed Release output.
    set_target_properties(${target_name} PROPERTIES
        DEBUG_POSTFIX ""
    )
    # 单目标构建完成后立即部署，满足 assets 启动脚本的固定目录契约。
    # Deploy immediately after an individual target links to satisfy the assets launchers' stable-directory contract.
    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${KBE_SERVER_RUNTIME_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:${target_name}>"
            "${KBE_SERVER_RUNTIME_DIR}"
        # 部署脚本不创建额外工程；文件锁会串行化多个服务端并行链接后的 Python 更新。
        # The script adds no project; its file lock serializes Python updates after parallel server links.
        COMMAND "${CMAKE_COMMAND}" -P "${KBE_PYTHON_DEPLOY_SCRIPT}"
        VERBATIM
    )
    target_compile_definitions(${target_name} PRIVATE
        KBE_SERVER
        ${component_definition}
        ${KBE_SERVER_TARGET_DEFINITIONS}
    )
    if(NOT KBE_SERVER_TARGET_NO_SERVER)
        target_link_libraries(${target_name} PRIVATE server)
    endif()
    target_link_libraries(${target_name} PRIVATE
        KBE::PythonRuntime
        ${KBE_SERVER_TARGET_LIBRARIES}
    )

    if(WIN32)
        # 这些系统库对应现有 VS 工程的服务端链接基线，额外系统库由具体组件追加。
        # These system libraries match the existing VS server link baseline; component-specific system libraries are appended locally.
        target_link_libraries(${target_name} PRIVATE
            secur32
            crypt32
            rpcrt4
            version
            wldap32
            netapi32
            ws2_32
            ${KBE_SERVER_TARGET_WINDOWS_LIBRARIES}
        )
        target_link_options(${target_name} PRIVATE /IGNORE:4049)
    else()
        target_link_libraries(${target_name} PRIVATE
            Threads::Threads
            ${CMAKE_DL_LIBS}
        )

        # 服务端发布目录可整体移动：动态 Python 只从可执行文件所在目录解析。
        # The server runtime can move as a unit because dynamic Python resolves only beside each executable.
        if(APPLE)
            set(_kbe_server_origin "@loader_path")
        else()
            set(_kbe_server_origin "$ORIGIN")
        endif()
        set_target_properties(${target_name} PROPERTIES
            BUILD_WITH_INSTALL_RPATH TRUE
            INSTALL_RPATH "${_kbe_server_origin}"
            INSTALL_RPATH_USE_LINK_PATH FALSE
        )

        if(NOT APPLE)
            # Linux 服务端仍显式链接历史代码使用的 libutil 与时钟运行库。
            # Linux servers continue to link libutil and the clock runtime used by legacy engine code.
            find_library(KBE_UTIL_LIBRARY NAMES util REQUIRED)
            find_library(KBE_RT_LIBRARY NAMES rt REQUIRED)
            target_link_libraries(${target_name} PRIVATE
                ${KBE_UTIL_LIBRARY}
                ${KBE_RT_LIBRARY}
            )
        endif()
    endif()
endfunction()
