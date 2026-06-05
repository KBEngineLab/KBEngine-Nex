# ---------------- 源文件 ----------------
set(SRCS
	common.cpp
	db_exception.cpp
	db_transaction.cpp
	db_interface_mysql.cpp
	entity_table_mysql.cpp
	entity_sqlstatement_mapping.cpp
	kbe_table_mysql.cpp
)
# ---------------- 创建静态库 ----------------
add_library(db_mysql STATIC ${SRCS})

target_link_libraries(db_mysql PUBLIC 
	fmt::fmt
	python_interface
)

# MariaDB client library via vcpkg
find_package(unofficial-libmariadb CONFIG REQUIRED)
target_link_libraries(db_mysql PUBLIC unofficial::libmariadb)
