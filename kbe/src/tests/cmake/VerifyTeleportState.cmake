if(NOT DEFINED KBE_CELLAPP_ENTITY_SOURCE OR NOT EXISTS "${KBE_CELLAPP_ENTITY_SOURCE}")
    message(FATAL_ERROR "KBE_CELLAPP_ENTITY_SOURCE must identify cellapp/entity.cpp")
endif()

file(READ "${KBE_CELLAPP_ENTITY_SOURCE}" _kbe_entity_source)

string(FIND "${_kbe_entity_source}"
    "void Entity::teleportRefEntityCall" _kbe_teleport_begin)
string(FIND "${_kbe_entity_source}"
    "void Entity::onTeleportRefEntityCall" _kbe_teleport_end)
if(_kbe_teleport_begin EQUAL -1 OR _kbe_teleport_end EQUAL -1 OR
        NOT _kbe_teleport_begin LESS _kbe_teleport_end)
    message(FATAL_ERROR "Cannot isolate Entity::teleportRefEntityCall")
endif()

math(EXPR _kbe_teleport_length "${_kbe_teleport_end} - ${_kbe_teleport_begin}")
string(SUBSTRING "${_kbe_entity_source}" ${_kbe_teleport_begin}
    ${_kbe_teleport_length} _kbe_teleport_body)

# 重入拒绝必须终止当前调用，否则第二次迁移会销毁仍等待 BaseApp 完成确认的 Entity。
# Re-entrant rejection must terminate the call or a second migration can destroy an Entity still awaiting BaseApp completion.
string(FIND "${_kbe_teleport_body}"
    "if(hasFlags(ENTITY_FLAGS_TELEPORT_START))" _kbe_in_progress_guard)
string(REGEX MATCH
    "onTeleportFailure\\(\\);[ \t\r\n]+return;"
    _kbe_rejection_return "${_kbe_teleport_body}")
if(_kbe_in_progress_guard EQUAL -1 OR NOT _kbe_rejection_return)
    message(FATAL_ERROR
        "Cross-Cell teleport must return immediately after rejecting an in-progress migration")
endif()

# 回调通知不是根因日志；真实失败点必须自行保留具体诊断，避免每次拒绝重复计为服务器错误。
# Callback notification is not a root-cause log; failure sites retain specific diagnostics without duplicating every rejection as a server error.
string(REGEX MATCH
    "void Entity::onTeleportFailure\\(\\)[^{]*\\{[^}]*ERROR_MSG"
    _kbe_failure_error_log "${_kbe_entity_source}")
if(_kbe_failure_error_log)
    message(FATAL_ERROR "Entity::onTeleportFailure must not emit an unconditional ERROR")
endif()

message(STATUS "TELEPORT_STATE_CONTRACT_PASS")
