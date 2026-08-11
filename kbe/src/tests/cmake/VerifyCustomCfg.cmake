if(NOT DEFINED KBE_SOURCE_ROOT OR NOT IS_DIRECTORY "${KBE_SOURCE_ROOT}")
    message(FATAL_ERROR "KBE_SOURCE_ROOT must identify the KBEngine source tree")
endif()
if(NOT DEFINED KBE_DEFAULT_CONFIG OR NOT EXISTS "${KBE_DEFAULT_CONFIG}")
    message(FATAL_ERROR "KBE_DEFAULT_CONFIG must identify kbengine_defaults.xml")
endif()

file(READ "${KBE_SOURCE_ROOT}/lib/server/serverconfig.h" _kbe_serverconfig_header)
file(READ "${KBE_SOURCE_ROOT}/lib/server/serverconfig.cpp" _kbe_serverconfig_source)
file(READ "${KBE_SOURCE_ROOT}/lib/server/python_app.h" _kbe_python_app_header)
file(READ "${KBE_SOURCE_ROOT}/lib/server/python_app.cpp" _kbe_python_app_source)
file(READ "${KBE_SOURCE_ROOT}/lib/server/entity_app.h" _kbe_entity_app_header)
file(READ "${KBE_DEFAULT_CONFIG}" _kbe_default_config)

function(kbe_require_literal source_text required_literal description)
    string(FIND "${source_text}" "${required_literal}" _kbe_required_position)
    if(_kbe_required_position EQUAL -1)
        message(FATAL_ERROR "customCfg contract is missing ${description}: ${required_literal}")
    endif()
endfunction()

# ServerConfig owns raw typed values so configuration loading remains independent
# from Python initialization. ServerConfig 保存原始类型和值，避免配置加载依赖 Python 初始化顺序。
foreach(_kbe_required IN ITEMS
    "struct CustomCfgItem"
    "std::map<std::string, CustomCfgItem> customCfg_"
    "customCfg(void) const"
)
    kbe_require_literal("${_kbe_serverconfig_header}" "${_kbe_required}" "ServerConfig storage")
endforeach()

foreach(_kbe_required IN ITEMS
    "getRootNode(\"customCfg\")"
    "Attribute(\"name\")"
    "Attribute(\"type\")"
    "customCfg_[item.name] = item"
)
    kbe_require_literal("${_kbe_serverconfig_source}" "${_kbe_required}" "XML parsing")
endforeach()

# The two application families must expose one implementation, otherwise type
# and default semantics can diverge. 两类组件必须共享实现，避免类型和缺省值语义分叉。
foreach(_kbe_required IN ITEMS
    "getCustomCfg, __py_getCustomCfg"
    "PyArg_ParseTuple(args, \"s|O\""
    "PyLong_FromString"
    "PyFloat_FromDouble"
    "PyImport_ImportModule(\"ast\")"
    "literal_eval"
)
    kbe_require_literal("${_kbe_python_app_source}" "${_kbe_required}" "PythonApp API")
endforeach()

kbe_require_literal("${_kbe_python_app_header}" "KBE_PYTHON_APP_H" "independent PythonApp include guard")
kbe_require_literal("${_kbe_entity_app_header}" "getCustomCfg" "EntityApp registration")
kbe_require_literal("${_kbe_entity_app_header}" "PythonApp::__py_getCustomCfg" "EntityApp forwarding")

foreach(_kbe_type IN ITEMS bool int float string dict list)
    kbe_require_literal("${_kbe_default_config}" "type=\"${_kbe_type}\"" "${_kbe_type} configuration example")
endforeach()

message(STATUS "customCfg source contract verified")
