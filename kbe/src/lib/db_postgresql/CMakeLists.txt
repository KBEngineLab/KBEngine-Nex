add_library(db_postgresql STATIC
	common.cpp
	db_exception_postgresql.cpp
	db_interface_postgresql.cpp
	db_transaction.cpp
	entity_sqlstatement_mapping.cpp
	entity_table_postgresql.cpp
	kbe_table_postgresql.cpp
	sqlstatement.cpp
)

# CMake 使用标准 FindPostgreSQL；Windows 工程由 vcpkg manifest 提供同一套 libpq。
# CMake uses standard FindPostgreSQL, while the Windows project obtains the same libpq API from the vcpkg manifest.
find_package(PostgreSQL REQUIRED)

kbe_configure_target(db_postgresql)
target_link_libraries(db_postgresql PUBLIC
	db_interface
	entitydef
	network
	PostgreSQL::PostgreSQL
	server
	KBE::PythonRuntime
)
