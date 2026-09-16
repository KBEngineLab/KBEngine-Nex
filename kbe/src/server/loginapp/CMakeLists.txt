
# 收集源文件
set(SRCS
	http_cb_handler.cpp
	loginapp.cpp
	loginapp_interface.cpp
	main.cpp
	profile.cpp
)

# 创建可执行程序
add_executable(loginapp ${SRCS})


# 链接依赖的库
target_link_libraries(loginapp
    PRIVATE
        server
        resmgr
        network
        thread
        pyscript
        common
        helper
        math
        g3dlite
        log4cxx
		jwsmtp
)

target_compile_definitions(loginapp PUBLIC  LOG4CXX_STATIC)


# ---------------- Python ----------------
target_link_libraries(loginapp PRIVATE python_interface)
if(NOT APPLE)
    target_link_options(loginapp PRIVATE -Wl,-export-dynamic)
endif()

# ---------------- OpenSSL ----------------
target_link_libraries(loginapp PRIVATE KBEOpenSSL::SSL)


# 设置可执行文件输出目录
set_target_properties(loginapp PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${KBE_ROOT}/kbe/bin/server"
)