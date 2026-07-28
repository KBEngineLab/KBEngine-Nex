add_library(resmgr STATIC
    resmgr.cpp
    resourceobject.cpp
    plugins/plugin_manifest.cpp
    plugins/plugin_manager.cpp
)

kbe_configure_target(resmgr)
target_link_libraries(resmgr PUBLIC common helper thread xml nlohmann_json::nlohmann_json)
