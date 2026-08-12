add_library(entitydef STATIC
    common.cpp
    datatype.cpp
    datatypes.cpp
    detaillevel.cpp
    entity_component.cpp
    entity_component_call.cpp
    entity_call.cpp
	entity_events.cpp
    entitydef.cpp
    entitycallabstract.cpp
    fixedarray.cpp
    fixeddict.cpp
    method.cpp
	method_utype_allocator.h
    property.cpp
    remote_entity_method.cpp
    scriptdef_module.cpp
    volatileinfo.cpp
)

kbe_configure_target(entitydef)
target_link_libraries(entitydef PUBLIC common helper math pyscript resmgr xml KBE::PythonRuntime)
