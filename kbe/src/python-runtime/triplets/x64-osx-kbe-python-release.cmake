# KBEngine distributes one Release CPython runtime for every engine configuration.
# KBEngine 的所有构建配置共用一份 Release CPython runtime，避免生成不会被使用的 Debug 包。
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES x86_64)
set(VCPKG_BUILD_TYPE release)
