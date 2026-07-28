find_package(hiredis CONFIG REQUIRED)

add_library(db_redis STATIC
    common.cpp
    db_exception.cpp
    db_interface_redis.cpp
    db_transaction.cpp
    entity_table_redis.cpp
    kbe_table_redis.cpp
    redis_watcher.cpp
)

kbe_configure_target(db_redis)
target_link_libraries(db_redis PUBLIC
    db_interface
    entitydef
    network
    server
    hiredis::hiredis
    KBE::PythonRuntime
)
