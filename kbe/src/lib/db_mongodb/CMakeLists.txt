# MongoDB 适配层依赖 vcpkg 提供的 mongo-c-driver 2.x 配置目标。
# The MongoDB adapter consumes the mongo-c-driver 2.x config target provided by vcpkg.
find_package(mongoc CONFIG REQUIRED)

add_library(db_mongodb STATIC
	common.cpp
	db_exception.cpp
	db_interface_mongodb.cpp
	db_transaction.cpp
	entity_table_mongodb.cpp
	kbe_table_mongodb.cpp
)

kbe_configure_target(db_mongodb)
target_link_libraries(db_mongodb PUBLIC
	db_interface
	entitydef
	mongoc::static
	network
	server
	KBE::PythonRuntime
)
