add_library(entitydef STATIC
    common.cpp
    datatype.cpp
    datatypes.cpp
    detaillevel.cpp
    entity_component.cpp
    entity_component_call.cpp
    entity_call.cpp
    entitydef.cpp
    entitycallabstract.cpp
    fixedarray.cpp
    fixeddict.cpp
    method.cpp
    property.cpp
    remote_entity_method.cpp
    scriptdef_module.cpp
    volatileinfo.cpp
)

kbe_configure_target(entitydef)
target_link_libraries(entitydef PUBLIC common helper math pyscript resmgr xml KBE::PythonRuntime)
