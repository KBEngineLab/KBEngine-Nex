find_package(RecastNavigation CONFIG REQUIRED)

add_library(navigation STATIC
    navigation.cpp
    navigation_handle.cpp
    navigation_mesh_handle.cpp
    navigation_tile_handle.cpp
)

kbe_configure_target(navigation)
target_link_libraries(navigation PUBLIC
    common
    helper
    math
    resmgr
    thread
    tmxparser
    RecastNavigation::Detour
)
