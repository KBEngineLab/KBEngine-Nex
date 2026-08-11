if(NOT DEFINED KBE_SOURCE_ROOT OR NOT IS_DIRECTORY "${KBE_SOURCE_ROOT}")
    message(FATAL_ERROR "KBE_SOURCE_ROOT must identify the KBEngine source tree")
endif()
if(NOT DEFINED KBE_DEFAULT_CONFIG OR NOT EXISTS "${KBE_DEFAULT_CONFIG}")
    message(FATAL_ERROR "KBE_DEFAULT_CONFIG must identify kbengine_defaults.xml")
endif()

file(READ "${KBE_DEFAULT_CONFIG}" _kbe_default_config)
file(READ "${KBE_SOURCE_ROOT}/lib/server/serverconfig.h" _kbe_serverconfig_header)
file(READ "${KBE_SOURCE_ROOT}/lib/server/serverconfig.cpp" _kbe_serverconfig_source)
file(READ "${KBE_SOURCE_ROOT}/lib/db_mysql/db_interface_mysql.cpp" _kbe_mysql_interface)
file(READ "${KBE_SOURCE_ROOT}/lib/db_mysql/entity_table_mysql.cpp" _kbe_mysql_entity_table)
file(READ "${KBE_SOURCE_ROOT}/lib/db_mysql/sqlstatement.h" _kbe_mysql_statement)
file(READ "${KBE_SOURCE_ROOT}/lib/db_postgresql/db_interface_postgresql.cpp" _kbe_postgresql_interface)
file(READ "${KBE_SOURCE_ROOT}/lib/db_postgresql/entity_table_postgresql.cpp" _kbe_postgresql_entity_table)
file(READ "${KBE_SOURCE_ROOT}/lib/db_postgresql/sqlstatement.cpp" _kbe_postgresql_statement)

function(kbe_require_literal source_text required_literal description)
    string(FIND "${source_text}" "${required_literal}" _kbe_required_position)
    if(_kbe_required_position EQUAL -1)
        message(FATAL_ERROR "Database ID policy contract is missing ${description}: ${required_literal}")
    endif()
endfunction()

# Configuration owns one validated policy shared by both SQL backends.
# 配置层统一校验策略，避免MySQL与PostgreSQL各自解释字符串而产生语义分叉。
foreach(_kbe_required IN ITEMS
    "<idType> Default </idType>"
    "<autoIncrementInit> 1 </autoIncrementInit>"
)
    kbe_require_literal("${_kbe_default_config}" "${_kbe_required}" "default configuration")
endforeach()

foreach(_kbe_required IN ITEMS
    "DBID_TYPE_DEFAULT"
    "DBID_TYPE_UUID64"
    "uint64 db_autoIncrementInit"
)
    kbe_require_literal("${_kbe_serverconfig_header}" "${_kbe_required}" "typed configuration storage")
endforeach()

foreach(_kbe_required IN ITEMS
    "enterNode(interfaceNode, \"idType\")"
    "expected Default or UUID64"
    "enterNode(interfaceNode, \"autoIncrementInit\")"
    "expected a positive integer"
)
    kbe_require_literal("${_kbe_serverconfig_source}" "${_kbe_required}" "configuration validation")
endforeach()

# MySQL must keep database-generated IDs in Default mode and return the exact
# engine-generated value in UUID64 mode. MySQL在两种模式下必须分别回传数据库ID和引擎UUID64。
foreach(_kbe_required IN ITEMS
    "pDBInfo->db_idType == DBInterfaceInfo::DBID_TYPE_DEFAULT"
    "pDBInfo->db_autoIncrementInit"
)
    kbe_require_literal("${_kbe_mysql_interface}" "${_kbe_required}" "MySQL policy access")
endforeach()
kbe_require_literal("${_kbe_mysql_entity_table}" "AUTO_INCREMENT=%\" PRIu64" "MySQL configured sequence start")
foreach(_kbe_required IN ITEMS
    "writeDBID_ = static_cast<DBID>(KBEngine::genUUID64())"
    "dbid_ = writeDBID_"
)
    kbe_require_literal("${_kbe_mysql_statement}" "${_kbe_required}" "MySQL UUID64 insert")
endforeach()

# PostgreSQL already owns the UUID insert path; the policy lookup must no
# longer be hard-coded to automatic IDs. PostgreSQL复用既有UUID插入路径，但策略判断不能再固定为自增。
kbe_require_literal("${_kbe_postgresql_interface}"
    "pDBInfo->db_idType == DBInterfaceInfo::DBID_TYPE_DEFAULT"
    "PostgreSQL policy access")
kbe_require_literal("${_kbe_postgresql_entity_table}"
    "BIGINT NOT NULL PRIMARY KEY"
    "PostgreSQL UUID64 schema")
kbe_require_literal("${_kbe_postgresql_statement}"
    "KBEngine::genUUID64()"
    "PostgreSQL UUID64 insert")

message(STATUS "Database ID policy source contract verified")
