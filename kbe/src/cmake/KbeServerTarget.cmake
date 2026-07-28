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

        if(NOT APPLE)
            # Linux 服务端保留 Python 的 libutil、时钟运行库和主程序符号导出语义。
            # Linux servers preserve Python's libutil, the clock runtime library, and main-program symbol export semantics.
            find_library(KBE_UTIL_LIBRARY NAMES util REQUIRED)
            find_library(KBE_RT_LIBRARY NAMES rt REQUIRED)
            target_link_libraries(${target_name} PRIVATE
                ${KBE_UTIL_LIBRARY}
                ${KBE_RT_LIBRARY}
            )
            target_link_options(${target_name} PRIVATE "LINKER:--export-dynamic")
        endif()
    endif()
endfunction()
