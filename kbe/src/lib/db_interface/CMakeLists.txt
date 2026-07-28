add_library(db_interface STATIC
    db_interface.cpp
    db_tasks.cpp
    db_threadpool.cpp
    entity_table.cpp
    kbe_tables.cpp
)

kbe_configure_target(db_interface)

# 数据库工厂在此层引用各后端，但静态库不反向链接后端，避免与后端继承接口形成目标环。
# The database factory references every backend here, but this static library does not link them back, avoiding target cycles with adapters that inherit the interface.
target_link_libraries(db_interface PUBLIC
    common
    entitydef
    helper
    network
    pyscript
    server
    thread
    KBE::PythonRuntime
)
