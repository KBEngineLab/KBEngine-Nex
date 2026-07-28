add_library(server STATIC
    asyncio_helper.cpp
    common.cpp
    component_active_report_handler.cpp
    components.cpp
    entity_app.cpp
    forward_messagebuffer.cpp
    globaldata_client.cpp
    globaldata_server.cpp
    idallocate.cpp
    id_component_querier.cpp
    machine_infos.cpp
    pendingLoginmgr.cpp
    plugin_runtime.cpp
    python_app.cpp
    py_file_descriptor.cpp
    script_timers.cpp
    sendmail_threadtasks.cpp
    serverapp.cpp
    serverconfig.cpp
    shutdowner.cpp
    signal_handler.cpp
    telnet_handler.cpp
    telnet_server.cpp
)

kbe_configure_target(server)
target_compile_definitions(server PRIVATE KBE_SERVER)
target_link_libraries(server PUBLIC
    common
    entitydef
    helper
    jwsmtp
    network
    pyscript
    resmgr
    thread
    xml
    KBE::PythonRuntime
)

if(WIN32)
    target_link_libraries(server PRIVATE netapi32 version)
endif()
