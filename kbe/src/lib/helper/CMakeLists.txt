add_library(helper STATIC
    crashhandler.cpp
    debug_helper.cpp
    debug_option.cpp
    eventhistory_stats.cpp
    profile.cpp
    profile_handler.cpp
    profiler.cpp
    script_loglevel.cpp
    sys_info.cpp
    watch_pools.cpp
    watcher.cpp
)

kbe_configure_target(helper)
target_link_libraries(helper PUBLIC common thread fmt::fmt log4cxx sigar)
