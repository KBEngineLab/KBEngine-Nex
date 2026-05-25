# ---------------- 源文件 ----------------
set(SRCS
	common.cpp
	components.cpp
	component_active_report_handler.cpp
	entity_app.cpp
	forward_messagebuffer.cpp
	globaldata_client.cpp
	globaldata_server.cpp
	idallocate.cpp
	pendingLoginmgr.cpp
	py_file_descriptor.cpp
	python_app.cpp
	script_timers.cpp
	serverapp.cpp
	serverconfig.cpp
	machine_infos.cpp
	sendmail_threadtasks.cpp
	shutdowner.cpp
	signal_handler.cpp
	telnet_handler.cpp
	telnet_server.cpp
	python_app.cpp
	asyncio_helper.cpp
	id_component_querier.cpp
)
# ---------------- 创建静态库 ----------------
add_library(server STATIC ${SRCS})



target_link_libraries(server PRIVATE 
	fmt::fmt
	python_interface
	resmgr
)
target_link_libraries(server PRIVATE jwsmtp)


