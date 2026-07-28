find_package(ZLIB REQUIRED)

set(_kbe_tmxparser_sources
    TmxEllipse.cpp
    TmxImage.cpp
    TmxImageLayer.cpp
    TmxLayer.cpp
    TmxMap.cpp
    TmxObject.cpp
    TmxObjectGroup.cpp
    TmxPolygon.cpp
    TmxPolyline.cpp
    TmxPropertySet.cpp
    TmxTile.cpp
    TmxTileset.cpp
    TmxUtil.cpp
)

# Unix Make 构建使用解析器自带的 Base64 实现；Windows VS 工程由 common 提供同名符号，必须避免重复定义。
# Unix Make builds use the parser's bundled Base64 implementation; Windows VS builds resolve the same symbols from common and must avoid duplicate definitions.
if(NOT WIN32)
    list(APPEND _kbe_tmxparser_sources base64.cpp)
endif()

add_library(tmxparser STATIC ${_kbe_tmxparser_sources})
kbe_configure_target(tmxparser)
target_include_directories(tmxparser PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(tmxparser PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../tinyxml")
target_link_libraries(tmxparser PUBLIC xml ZLIB::ZLIB)

if(WIN32)
    target_link_libraries(tmxparser PUBLIC common)
endif()
