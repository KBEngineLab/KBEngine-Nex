set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES x86_64)

# 动态 libpython 避免静态归档裁剪 Python C API 符号，并统一扩展模块加载模型。
# Shared libpython avoids static-archive symbol stripping and unifies the extension loading model.
if(PORT STREQUAL "python3")
    set(VCPKG_LIBRARY_LINKAGE dynamic)
    set(VCPKG_BUILD_TYPE release)
endif()
