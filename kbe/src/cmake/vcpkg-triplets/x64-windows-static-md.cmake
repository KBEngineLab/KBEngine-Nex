set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_PROVIDED_FORTRAN ON)

# Python 的原生扩展必须共享同一动态解释器；其余依赖继续静态链接以控制部署面。
# Native Python extensions require one shared interpreter; other dependencies remain static to keep deployment contained.
if(PORT STREQUAL "python3")
    set(VCPKG_LIBRARY_LINKAGE dynamic)
    set(VCPKG_BUILD_TYPE release)
endif()
