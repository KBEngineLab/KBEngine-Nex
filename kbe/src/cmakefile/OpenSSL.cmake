# cmake/OpenSSL.cmake
# OpenSSL 统一走 vcpkg，避免和 libpq 带进来的 OpenSSL 版本混在一起。

find_package(OpenSSL REQUIRED)

# 继续保留 KBEOpenSSL::SSL 这个目标名，现有 CMake 子工程不用跟着改。
add_library(KBEOpenSSL::SSL INTERFACE IMPORTED)
set_target_properties(KBEOpenSSL::SSL PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIR}"
    INTERFACE_LINK_LIBRARIES "OpenSSL::SSL;OpenSSL::Crypto"
    INTERFACE_COMPILE_DEFINITIONS "USE_OPENSSL"
)
