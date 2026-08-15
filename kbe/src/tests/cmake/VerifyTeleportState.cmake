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

# 本地目标实体消失后，陈旧 EntityCall 仍可能携带当前 CellApp ID。必须在 BaseApp 开始缓冲、
# Entity 转为 Ghost 之前失败，否则会形成 realCell 指向自身且永远无法投递的 Ghost。
# A stale EntityCall can retain the current CellApp ID after its local target disappears. Reject it
# before BaseApp buffering or Ghost conversion, otherwise realCell points to self and delivery stalls forever.
string(FIND "${_kbe_teleport_body}"
    "nearbyMBRef->componentID() == g_componentID" _kbe_stale_local_guard)
string(FIND "${_kbe_teleport_body}"
    "BaseappInterface::onMigrationCellappStart" _kbe_baseapp_migration_start)
string(FIND "${_kbe_teleport_body}"
    "onTeleportRefEntityCall(nearbyMBRef, pos, dir);" _kbe_begin_ghost_conversion)
string(FIND "${_kbe_teleport_body}"
    "if (!nearbyMBRef->isCellReal())" _kbe_remote_type_guard)
if(NOT _kbe_stale_local_guard EQUAL -1 AND NOT _kbe_remote_type_guard EQUAL -1 AND
        _kbe_stale_local_guard LESS _kbe_remote_type_guard)
    math(EXPR _kbe_stale_local_length
        "${_kbe_remote_type_guard} - ${_kbe_stale_local_guard}")
    string(SUBSTRING "${_kbe_teleport_body}" ${_kbe_stale_local_guard}
        ${_kbe_stale_local_length} _kbe_stale_local_body)
    string(FIND "${_kbe_stale_local_body}"
        "onTeleportFailure();" _kbe_stale_local_failure)
    string(FIND "${_kbe_stale_local_body}"
        "return;" _kbe_stale_local_return)
else()
    set(_kbe_stale_local_failure -1)
    set(_kbe_stale_local_return -1)
endif()
if(_kbe_stale_local_guard EQUAL -1 OR _kbe_stale_local_failure EQUAL -1 OR
        _kbe_stale_local_return EQUAL -1 OR _kbe_baseapp_migration_start EQUAL -1 OR
        _kbe_begin_ghost_conversion EQUAL -1 OR
        NOT _kbe_stale_local_guard LESS _kbe_baseapp_migration_start OR
        NOT _kbe_stale_local_guard LESS _kbe_begin_ghost_conversion)
    message(FATAL_ERROR
        "Cross-Cell teleport must reject stale local EntityCalls before migration state changes")
endif()

# changeToGhost 的底层不变量必须校验传入目标，而不是尚未更新的 realCell_ 成员。
# changeToGhost must validate its destination argument rather than the pre-transition realCell_ member.
string(FIND "${_kbe_entity_source}"
    "KBE_ASSERT(realCell > 0 && realCell != g_componentID);" _kbe_change_to_ghost_invariant)
if(_kbe_change_to_ghost_invariant EQUAL -1)
    message(FATAL_ERROR "Entity::changeToGhost must reject zero and self destinations")
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
