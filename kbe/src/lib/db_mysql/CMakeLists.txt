find_package(unofficial-libmariadb CONFIG REQUIRED)

add_library(db_mysql STATIC
    common.cpp
    db_exception.cpp
    db_interface_mysql.cpp
    db_transaction.cpp
    entity_sqlstatement_mapping.cpp
    entity_table_mysql.cpp
    kbe_table_mysql.cpp
)

kbe_configure_target(db_mysql)
target_link_libraries(db_mysql PUBLIC
    db_interface
    entitydef
    network
    server
    unofficial::libmariadb
    KBE::PythonRuntime
)
