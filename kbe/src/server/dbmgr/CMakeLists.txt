
# 收集源文件
set(SRCS
	buffered_dbtasks.cpp
	dbmgr.cpp
	dbmgr_interface.cpp
	dbtasks.cpp
	entity_component.cpp
	interfaces_handler.cpp
	main.cpp
	profile.cpp
	sync_app_datas_handler.cpp
	update_dblog_handler.cpp
)

# 创建可执行程序
add_executable(dbmgr ${SRCS})



# link_directories(${KBE_ROOT}/kbe/src/build/vcpkg_installed/x64-linux/lib)

# 链接依赖的库
target_link_libraries(dbmgr
    PRIVATE
		db_interface
		db_mongodb
		db_mysql
		db_postgresql
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
include_directories(
    ${KBE_ROOT}/kbe/src/lib/dependencies/hiredis
)

target_compile_definitions(dbmgr PUBLIC  LOG4CXX_STATIC)

# ---------------- Python ----------------
target_link_libraries(dbmgr PRIVATE python_interface)
if(NOT APPLE)
    target_link_options(dbmgr PRIVATE -Wl,-export-dynamic)
endif()

# ---------------- OpenSSL ----------------
target_link_libraries(dbmgr PRIVATE KBEOpenSSL::SSL)


# 设置可执行文件输出目录
set_target_properties(dbmgr PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${KBE_ROOT}/kbe/bin/server"
)
