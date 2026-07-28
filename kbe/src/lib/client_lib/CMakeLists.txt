add_library(client_lib STATIC
    client_interface.cpp
    clientapp.cpp
    clientobjectbase.cpp
    config.cpp
    entity.cpp
    entity_aspect.cpp
    entity_component.cpp
    event.cpp
    moveto_point_handler.cpp
    profile.cpp
    script_callbacks.cpp
)

kbe_configure_target(client_lib)
target_compile_definitions(client_lib PRIVATE KBE_CLIENT)
target_link_libraries(client_lib PUBLIC
    common
    entitydef
    helper
    math
    network
    pyscript
    resmgr
    server
    thread
    xml
    KBE::PythonRuntime
)
