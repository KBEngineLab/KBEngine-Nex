add_library(jwsmtp STATIC
    jwsmtp/jwsmtp/base64.cpp
    jwsmtp/jwsmtp/compat.cpp
    jwsmtp/jwsmtp/mailer.cpp
)

kbe_configure_target(jwsmtp)

# 消费方直接包含 jwsmtp.h，因此嵌套的上游头文件目录属于该库的公开接口。
# Consumers include jwsmtp.h directly, so the nested upstream header directory is part of this library's public interface.
target_include_directories(jwsmtp PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/jwsmtp/jwsmtp")

if(WIN32)
    target_link_libraries(jwsmtp PRIVATE ws2_32)
endif()
