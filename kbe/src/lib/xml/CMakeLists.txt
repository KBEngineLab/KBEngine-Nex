add_library(xml STATIC
    xml.cpp
    ../dependencies/tinyxml/tinystr.cpp
    ../dependencies/tinyxml/tinyxml.cpp
    ../dependencies/tinyxml/tinyxmlerror.cpp
    ../dependencies/tinyxml/tinyxmlparser.cpp
)

kbe_configure_target(xml)
target_link_libraries(xml PUBLIC common)
