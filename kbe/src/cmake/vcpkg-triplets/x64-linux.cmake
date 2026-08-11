set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)

# 动态 libpython 为任意第三方扩展提供完整符号表，同时避免整个依赖图改成共享库。
# Shared libpython exposes the complete API to arbitrary extensions without making the entire dependency graph shared.
if(PORT STREQUAL "python3")
    set(VCPKG_LIBRARY_LINKAGE dynamic)
    set(VCPKG_BUILD_TYPE release)
endif()
