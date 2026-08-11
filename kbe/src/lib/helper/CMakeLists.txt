add_library(helper STATIC
    debug_helper.cpp
    debug_option.cpp
    eventhistory_stats.cpp
    profile.cpp
    profile_latency.cpp
    profile_handler.cpp
    profiler.cpp
    script_loglevel.cpp
    sys_info.cpp
    watch_pools.cpp
    watcher.cpp
)

# crash handler 依赖 Windows SEH/dbghelp，仅 Windows 编译。
# The crash handler depends on Windows SEH/dbghelp and is compiled on Windows only.
if(WIN32)
    target_sources(helper PRIVATE crashhandler.cpp)
endif()

kbe_configure_target(helper)
target_link_libraries(helper PUBLIC common thread fmt::fmt kbe_log4cxx sigar)
