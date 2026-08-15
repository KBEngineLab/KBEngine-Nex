if(NOT DEFINED KBE_SOURCE_ROOT OR NOT EXISTS "${KBE_SOURCE_ROOT}")
    message(FATAL_ERROR "KBE_SOURCE_ROOT must identify the kbe/src directory")
endif()

file(READ "${KBE_SOURCE_ROOT}/server/baseapp/entity.cpp" _kbe_base_entity)
file(READ "${KBE_SOURCE_ROOT}/server/baseapp/entity_messages_forward_handler.h" _kbe_forward_header)
file(READ "${KBE_SOURCE_ROOT}/server/cellapp/cellapp.cpp" _kbe_cellapp)
file(READ "${KBE_SOURCE_ROOT}/server/cellapp/entity.cpp" _kbe_cell_entity)
file(READ "${KBE_SOURCE_ROOT}/server/cellapp/space.cpp" _kbe_space)

# End 先到时保存的路由必须在 forwarding task 超时释放后继续存在；迟到或重复 Start
# 只能幂等消费/恢复，不能再以断言终止 BaseApp。
# The End-first route must outlive a timed-out forwarding task. Late or duplicate Start
# messages are consumed/recovered idempotently and must never terminate BaseApp via assertion.
string(FIND "${_kbe_base_entity}" "void Entity::onMigrationCellappStart" _kbe_start_begin)
string(FIND "${_kbe_base_entity}" "void Entity::onMigrationCellappEnd" _kbe_start_end)
if(_kbe_start_begin EQUAL -1 OR _kbe_start_end EQUAL -1 OR NOT _kbe_start_begin LESS _kbe_start_end)
	message(FATAL_ERROR "Cannot isolate Base Entity migration Start handler")
endif()
math(EXPR _kbe_start_length "${_kbe_start_end} - ${_kbe_start_begin}")
string(SUBSTRING "${_kbe_base_entity}" ${_kbe_start_begin} ${_kbe_start_length} _kbe_start_body)
foreach(_required
		"deferredMigrationSourceCellAppID_ != sourceCellAppID"
		"deferredMigrationTargetCellAppID_ != targetCellAppID"
		"if (pBufferedSendToClientMessages_)"
		"pBufferedSendToClientMessages_->startForward();"
		"sameCommitPending || routeAlreadyCommitted"
		"onMigrationCellappOver(deferredSourceCellAppID, deferredTargetCellAppID);")
	string(FIND "${_kbe_start_body}" "${_required}" _kbe_required_pos)
	if(_kbe_required_pos EQUAL -1)
		message(FATAL_ERROR "BaseApp late migration Start recovery is missing: ${_required}")
	endif()
endforeach()
string(FIND "${_kbe_start_body}" "KBE_ASSERT(pBufferedSendToClientMessages_)" _kbe_fatal_late_start)
if(NOT _kbe_fatal_late_start EQUAL -1)
	message(FATAL_ERROR "Late migration Start must not assert after its forwarding task timed out")
endif()

foreach(_required
		"deferredMigrationSourceCellAppID_ = sourceCellAppID;"
		"deferredMigrationTargetCellAppID_ = targetCellAppID;"
		"hasFlags(ENTITY_FLAGS_TELEPORT_STOP) && pBufferedSendToClientMessages_ == NULL"
		"onMigrationCellappOver(deferredMigrationSourceCellAppID_,")
	string(FIND "${_kbe_base_entity}" "${_required}" _kbe_required_pos)
	if(_kbe_required_pos EQUAL -1)
		message(FATAL_ERROR "BaseApp must retain the End-first migration route: ${_required}")
	endif()
endforeach()

# BaseApp 必须保留 Source 身份，先请求 Target 提交时验证；验证成功后才允许切换路由和确认双端。
# BaseApp must retain Source identity, request commit-time Target validation, and switch routing only after success.
string(FIND "${_kbe_forward_header}" "sourceCellappID_" _kbe_source_state)
string(FIND "${_kbe_base_entity}" "void Entity::onMigrationCellappOver" _kbe_over_begin)
string(FIND "${_kbe_base_entity}" "void Entity::onMigrationCellappPrepared" _kbe_over_end)
if(_kbe_over_begin EQUAL -1 OR _kbe_over_end EQUAL -1 OR NOT _kbe_over_begin LESS _kbe_over_end)
    message(FATAL_ERROR "Cannot isolate Base Entity migration prepare phase")
endif()
math(EXPR _kbe_over_length "${_kbe_over_end} - ${_kbe_over_begin}")
string(SUBSTRING "${_kbe_base_entity}" ${_kbe_over_begin} ${_kbe_over_length} _kbe_over_body)
string(FIND "${_kbe_over_body}" "reqTeleportToCellAppPrepare" _kbe_prepare_send)
string(FIND "${_kbe_over_body}" "cellEntityCall()->componentID(targetCellAppID)" _kbe_early_route_switch)
string(FIND "${_kbe_over_body}" "reqTeleportToCellAppOver" _kbe_early_completion)
if(_kbe_source_state EQUAL -1 OR _kbe_prepare_send EQUAL -1 OR
		NOT _kbe_early_route_switch EQUAL -1 OR NOT _kbe_early_completion EQUAL -1)
	message(FATAL_ERROR "BaseApp prepare phase must retain Source without switching routing or releasing either peer")
endif()

string(FIND "${_kbe_base_entity}" "void Entity::commitMigrationCellapp" _kbe_commit_begin)
string(FIND "${_kbe_base_entity}" "void Entity::onBufferedForwardToCellappMessagesOver" _kbe_commit_end)
if(_kbe_commit_begin EQUAL -1 OR _kbe_commit_end EQUAL -1 OR NOT _kbe_commit_begin LESS _kbe_commit_end)
    message(FATAL_ERROR "Cannot isolate Base Entity migration commit phase")
endif()
math(EXPR _kbe_commit_length "${_kbe_commit_end} - ${_kbe_commit_begin}")
string(SUBSTRING "${_kbe_base_entity}" ${_kbe_commit_begin} ${_kbe_commit_length} _kbe_commit_body)
string(FIND "${_kbe_commit_body}" "cellEntityCall()->componentID(targetCellAppID)" _kbe_route_switch)
string(FIND "${_kbe_commit_body}" "reqTeleportToCellAppOver" _kbe_completion_send)
string(FIND "${_kbe_over_body}" "targetCellAppID, sourceCellAppID" _kbe_both_peers)
string(FIND "${_kbe_commit_body}" "targetCellAppID, sourceCellAppID" _kbe_both_peers)
string(FIND "${_kbe_commit_body}" "targetCellAppID == sourceCellAppID ? 1" _kbe_single_rollback)
if(_kbe_route_switch EQUAL -1 OR _kbe_completion_send EQUAL -1 OR
		_kbe_both_peers EQUAL -1 OR _kbe_single_rollback EQUAL -1 OR
		NOT _kbe_route_switch LESS _kbe_completion_send)
    message(FATAL_ERROR
        "BaseApp must switch routing only in commit and before confirming both Target and Source CellApps")
endif()

string(FIND "${_kbe_base_entity}" "if (!success)" _kbe_prepare_failure)
string(FIND "${_kbe_base_entity}" "Source is retained" _kbe_source_retained)
if(_kbe_prepare_failure EQUAL -1 OR _kbe_source_retained EQUAL -1)
	message(FATAL_ERROR "Target validation failure must retain Source without committing the stale route")
endif()

# 有 Base 的 Source Ghost 只能由 BaseApp 路由切换确认释放，Target 回调不能抢先销毁。
# A based Source Ghost is released only by BaseApp's routing confirmation, never by Target's callback.
string(FIND "${_kbe_cellapp}" "void Cellapp::reqTeleportToCellAppCB" _kbe_cb_begin)
string(FIND "${_kbe_cellapp}" "void Cellapp::reqTeleportToCellAppOver" _kbe_cb_end)
math(EXPR _kbe_cb_length "${_kbe_cb_end} - ${_kbe_cb_begin}")
string(SUBSTRING "${_kbe_cellapp}" ${_kbe_cb_begin} ${_kbe_cb_length} _kbe_cb_body)
string(REGEX MATCH
    "if \\(entityBaseappID == 0\\)[ \t\r\n]+destroyEntity\\(teleportEntityID, false\\);"
    _kbe_unbased_destroy "${_kbe_cb_body}")
if(NOT _kbe_unbased_destroy)
    message(FATAL_ERROR "Successful migration callbacks may immediately destroy only unbased Source entities")
endif()

# 双端确认必须校验本地角色；Source 只释放仍指向该 Target 的 Ghost，Target 只解锁 real。
# Completion must validate the local role: Source releases only the matching Ghost and Target unlocks only real.
string(FIND "${_kbe_cellapp}" "g_componentID == sourceCellAppID" _kbe_source_role)
string(FIND "${_kbe_cellapp}" "entity->realCell() != targetCellAppID" _kbe_source_route_guard)
string(FIND "${_kbe_cellapp}" "if (!entity->isReal())" _kbe_target_real_guard)
string(FIND "${_kbe_cellapp}" "if (sourceCellAppID == targetCellAppID)" _kbe_rollback_completion)
if(_kbe_source_role EQUAL -1 OR _kbe_source_route_guard EQUAL -1 OR
		_kbe_target_real_guard EQUAL -1 OR _kbe_rollback_completion EQUAL -1)
    message(FATAL_ERROR "CellApp migration completion must reject stale Source and Target confirmations")
endif()

# BaseApp 路由切换前已进入旧 Channel 的 self RPC 可以由 Source Ghost 转发，但跨实体调用仍要求 real Source。
# Self RPCs already queued on the old Channel before the BaseApp route switch may cross the Source Ghost,
# while cross-entity calls must still be authorized by a real Source.
string(FIND "${_kbe_cellapp}" "void Cellapp::onRemoteCallMethodFromClient" _kbe_client_rpc_begin)
string(FIND "${_kbe_cellapp}" "void Cellapp::onUpdateDataFromClient" _kbe_client_rpc_end)
math(EXPR _kbe_client_rpc_length "${_kbe_client_rpc_end} - ${_kbe_client_rpc_begin}")
string(SUBSTRING "${_kbe_cellapp}" ${_kbe_client_rpc_begin} ${_kbe_client_rpc_length} _kbe_client_rpc_body)
foreach(_required
        "sourceEntity == e && srcEntityID == targetID"
        "sourceEntity->isReal() || migrationSelfCall"
        "sourceEntity->baseEntityCall()->componentID() == sourceComponent->cid"
		"Security::isAuthorizedClientCellTarget"
		"e->migrationRelayCell()"
		"s.done();")
    string(FIND "${_kbe_client_rpc_body}" "${_required}" _kbe_required_pos)
    if(_kbe_required_pos EQUAL -1)
        message(FATAL_ERROR "Migration self-RPC authorization is missing: ${_required}")
    endif()
endforeach()
string(FIND "${_kbe_client_rpc_body}"
    "sourceEntity != NULL && sourceEntity->isReal() &&" _kbe_legacy_real_binding)
if(NOT _kbe_legacy_real_binding EQUAL -1)
    message(FATAL_ERROR "Source Ghost self-RPC must not be rejected before migration forwarding")
endif()

# Target 完成 Space/AOI 与脚本回调后才可通知 BaseApp 切换路由。
# Target may notify BaseApp to switch routing only after Space/AOI and script callbacks finish.
string(FIND "${_kbe_cellapp}" "void Cellapp::reqTeleportToCellApp(" _kbe_request_begin)
string(FIND "${_kbe_cellapp}" "void Cellapp::reqTeleportToCellAppCB" _kbe_request_end)
math(EXPR _kbe_request_length "${_kbe_request_end} - ${_kbe_request_begin}")
string(SUBSTRING "${_kbe_cellapp}" ${_kbe_request_begin} ${_kbe_request_length} _kbe_request_body)

# Target 清空 Ghost 同步关系时必须单独保留迁移来源授权，不能把两种生命周期重新耦合。
# Clearing Target Ghost replication must retain migration relay authorization as separate state.
string(FIND "${_kbe_request_body}" "e->ghostCell(0);" _kbe_clear_ghost)
string(FIND "${_kbe_request_body}" "e->migrationRelayCell(ghostCell);" _kbe_retain_relay)
if(_kbe_clear_ghost EQUAL -1 OR _kbe_retain_relay EQUAL -1 OR
		NOT _kbe_clear_ghost LESS _kbe_retain_relay)
	message(FATAL_ERROR "Teleport Target must retain its authenticated RPC predecessor after clearing Ghost replication")
endif()

string(FIND "${_kbe_request_body}" "e->onTeleportSuccess(nearbyMBRef, space->id());" _kbe_target_ready)
string(FIND "${_kbe_request_body}" "BaseappInterface::onMigrationCellappEnd" _kbe_target_end)
if(_kbe_target_ready EQUAL -1 OR _kbe_target_end EQUAL -1 OR NOT _kbe_target_ready LESS _kbe_target_end)
    message(FATAL_ERROR "Target must finish migration initialization before notifying BaseApp")
endif()

# Target 必须在提交请求中重新验证 Entity、Space、迁移标记和认证来源。
# Target commit preparation must revalidate Entity, Space, migration flag, and authenticated predecessor.
string(FIND "${_kbe_cellapp}" "void Cellapp::reqTeleportToCellAppPrepare" _kbe_prepare_handler)
foreach(_required
		"entity->hasFlags(ENTITY_FLAGS_TELEPORT_START)"
		"entity->migrationRelayCell() == sourceCellAppID"
		"space != NULL && space->isGood()"
		"BaseappInterface::onMigrationCellappPrepared")
	string(FIND "${_kbe_cellapp}" "${_required}" _kbe_required_pos)
	if(_kbe_required_pos EQUAL -1)
		message(FATAL_ERROR "Target commit validation is missing: ${_required}")
	endif()
endforeach()

# 未提交 Target 和承载它的 Space 必须拒绝脚本定时销毁；关服 entityID=0 仍可强制清理。
# Uncommitted Target and owning Space reject script-timer destruction while entityID=0 shutdown cleanup remains available.
foreach(_required
		"pobj->hasFlags(ENTITY_FLAGS_TELEPORT_START)"
		"pSpace->hasPendingMigrationEntities()")
	string(FIND "${_kbe_cell_entity}" "${_required}" _kbe_required_pos)
	if(_kbe_required_pos EQUAL -1)
		message(FATAL_ERROR "Entity migration lifetime lease is missing: ${_required}")
	endif()
endforeach()
foreach(_required
		"bool Space::hasPendingMigrationEntities() const"
		"entityID != 0 && hasPendingMigrationEntities()")
	string(FIND "${_kbe_space}" "${_required}" _kbe_required_pos)
	if(_kbe_required_pos EQUAL -1)
		message(FATAL_ERROR "Space migration lifetime lease is missing: ${_required}")
	endif()
endforeach()

# Entity 销毁必须同步调用 Witness 的正常离开路径，且不再把可恢复的延迟 AOI 状态记为 ERROR。
# Entity destruction must synchronously use Witness's normal leave path without logging recoverable AOI lag as ERROR.
string(FIND "${_kbe_cell_entity}" "void Entity::onDestroy" _kbe_destroy_begin)
string(FIND "${_kbe_cell_entity}" "PyObject* Entity::__py_pyDestroyEntity" _kbe_destroy_end)
math(EXPR _kbe_destroy_length "${_kbe_destroy_end} - ${_kbe_destroy_begin}")
string(SUBSTRING "${_kbe_cell_entity}" ${_kbe_destroy_begin} ${_kbe_destroy_length} _kbe_destroy_body)
string(FIND "${_kbe_destroy_body}" "pWitness()->_onLeaveView(pEntityRef)" _kbe_aoi_detach)
string(FIND "${_kbe_destroy_body}" "witnesses_count({}/{}) != 0" _kbe_legacy_error)
if(_kbe_aoi_detach EQUAL -1 OR NOT _kbe_legacy_error EQUAL -1)
    message(FATAL_ERROR "Entity destruction must synchronously and idempotently detach observer relations")
endif()

message(STATUS "MIGRATION_RELEASE_BARRIER_CONTRACT_PASS")
