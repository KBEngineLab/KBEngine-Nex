# MongoDB 适配层依赖 vcpkg 提供的 mongo-c-driver 2.x 配置目标。
# The MongoDB adapter consumes the mongo-c-driver 2.x config target provided by vcpkg.
find_package(mongoc CONFIG REQUIRED)

# 适配层源文件。
# Adapter source files.
set(SRCS
	common.cpp
	db_exception.cpp
	db_interface_mongodb.cpp
	db_transaction.cpp
	entity_table_mongodb.cpp
	kbe_table_mongodb.cpp
)
# 与其他数据库后端一致构建为静态库，并由 DBMgr 最终链接。
# Build a static library like the other database backends and link it into DBMgr.
add_library(db_mongodb STATIC ${SRCS})

target_link_libraries(db_mongodb PUBLIC
	fmt::fmt
	mongoc::static
	python_interface
)
