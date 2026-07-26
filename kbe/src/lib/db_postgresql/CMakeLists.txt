# PostgreSQL 后端保持独立静态库，和其他数据库适配层的工程边界一致。
# Keep PostgreSQL as an independent static library, matching the boundary used by other database adapters.
set(SRCS
	common.cpp
	db_exception_postgresql.cpp
	db_interface_postgresql.cpp
	db_transaction.cpp
	entity_sqlstatement_mapping.cpp
	entity_table_postgresql.cpp
	kbe_table_postgresql.cpp
	sqlstatement.cpp
)

add_library(db_postgresql STATIC ${SRCS})

# CMake 使用标准 FindPostgreSQL；Windows 工程由 vcpkg manifest 提供同一套 libpq。
# CMake uses standard FindPostgreSQL, while the Windows project obtains the same libpq API from the vcpkg manifest.
find_package(PostgreSQL REQUIRED)

target_link_libraries(db_postgresql PUBLIC
	fmt::fmt
	PostgreSQL::PostgreSQL
	python_interface
)
