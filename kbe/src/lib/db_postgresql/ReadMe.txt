PostgreSQL 数据库后端。

依赖 libpq，包声明放在 src/vcpkg.json。
CMake 通过 find_package(PostgreSQL) 接入，VS 工程走 vcpkg manifest。
