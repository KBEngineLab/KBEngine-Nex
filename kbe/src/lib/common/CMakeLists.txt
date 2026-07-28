add_library(common STATIC
    base64.cpp
    blowfish.cpp
    common.cpp
    kbekey.cpp
    kbeversion.cpp
    md5.cpp
    memorystream.cpp
    rsa.cpp
    sha1.cpp
    ssl.cpp
    strutil.cpp
    tasks.cpp
    timer.cpp
    timestamp.cpp
)

kbe_configure_target(common)
target_link_libraries(common PUBLIC fmt::fmt OpenSSL::SSL OpenSSL::Crypto)
