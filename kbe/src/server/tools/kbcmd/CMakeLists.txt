
# 收集源文件
set(SRCS
	client_sdk.cpp
	client_sdk_unity.cpp
	client_sdk_ue4.cpp
	client_sdk_ue5.cpp
	client_sdk_cxx.cpp
	client_sdk_typescript.cpp
	client_sdk_gdscript.cpp
	client_sdk_plugin.cpp
	client_sdk_csharp.cpp
	server_assets.cpp
	entity_component.cpp
	kbcmd.cpp
	kbcmd_interface.cpp
	profile.cpp
	main.cpp
)

# 创建可执行程序
add_executable(kbcmd ${SRCS})


# 链接依赖的库
target_link_libraries(kbcmd
    PRIVATE
        server
        resmgr
        entitydef
        network
        thread
        pyscript
        common
        helper
        math
        g3dlite
        log4cxx
)

target_compile_definitions(kbcmd PUBLIC  LOG4CXX_STATIC)


# ---------------- Python ----------------
target_link_libraries(kbcmd PRIVATE python_interface)
if(NOT APPLE)
    target_link_options(kbcmd PRIVATE -Wl,-export-dynamic)
endif()

# ---------------- OpenSSL ----------------
target_link_libraries(kbcmd PRIVATE KBEOpenSSL::SSL)


# 设置可执行文件输出目录
set_target_properties(kbcmd PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${KBE_ROOT}/kbe/bin/server"
)
