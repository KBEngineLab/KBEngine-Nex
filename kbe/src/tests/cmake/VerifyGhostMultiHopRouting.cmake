if(NOT DEFINED KBE_SOURCE_ROOT OR NOT IS_DIRECTORY "${KBE_SOURCE_ROOT}")
    message(FATAL_ERROR "KBE_SOURCE_ROOT must identify the kbe/src directory")
endif()

file(READ "${KBE_SOURCE_ROOT}/server/cellapp/cellapp.cpp" _cellapp)
file(READ "${KBE_SOURCE_ROOT}/server/cellapp/ghost_manager.cpp" _ghost_manager)
string(FIND "${_cellapp}" "void Cellapp::onRemoteRealMethodCall" _handler_begin)
string(FIND "${_cellapp}" "void Cellapp::onUpdateGhostVolatileData" _handler_end)
if(_handler_begin EQUAL -1 OR _handler_end EQUAL -1 OR NOT _handler_begin LESS _handler_end)
    message(FATAL_ERROR "Cannot isolate Cellapp::onRemoteRealMethodCall")
endif()
math(EXPR _handler_length "${_handler_end} - ${_handler_begin}")
string(SUBSTRING "${_cellapp}" ${_handler_begin} ${_handler_length} _handler)

# 旧 Ghost 的 real RPC 必须沿当前 realCell/route 继续转发；只有到达 real 后才执行。
# Real RPCs from old Ghosts must follow the current realCell/route and execute only on the real Entity.
foreach(_required
        "forwardRemoteRealMethodCall(gm, entityID, sourceInfos->cid, targetCell, s)"
        "if (!entity->isReal())"
        "forwardRemoteRealMethodCall(gm, entityID, sourceInfos->cid, entity->realCell(), s)"
        "entity->onRemoteRealMethodCall(s)")
    string(FIND "${_handler}" "${_required}" _required_pos)
    if(_required_pos EQUAL -1)
        message(FATAL_ERROR "Ghost multi-hop routing is missing: ${_required}")
    endif()
endforeach()

string(FIND "${_handler}" "isCurrentGhostPeer" _single_peer_guard)
if(NOT _single_peer_guard EQUAL -1)
    message(FATAL_ERROR "Remote real RPC routing must not reject authenticated older Ghost generations")
endif()

foreach(_required
        "targetCellID == g_componentID"
        "targetCellID == sourceCellID"
        "pushRouteMessage(entityID, targetCellID, pForwardBundle)")
    string(FIND "${_cellapp}" "${_required}" _required_pos)
    if(_required_pos EQUAL -1)
        message(FATAL_ERROR "Ghost forwarding loop guard is missing: ${_required}")
    endif()
endforeach()

# 路由必须覆盖内部 Channel 的延迟窗口，并在有效查询时续期；固定 5 秒会让低频旧 Ghost 在
# 下一次 RPC 到达前失去转发目标。
# Routes cover the internal Channel delay window and renew on valid lookup; a fixed
# five-second expiry loses the target before a low-frequency old Ghost calls again.
foreach(_required
        "GHOST_ROUTE_IDLE_TIMEOUT_SECONDS = 10 * 60"
        "iter->second.lastTime = timestamp();"
        "GHOST_ROUTE_IDLE_TIMEOUT_SECONDS * stampsPerSecond()")
    string(FIND "${_ghost_manager}" "${_required}" _required_pos)
    if(_required_pos EQUAL -1)
        message(FATAL_ERROR "Ghost route retention contract is missing: ${_required}")
    endif()
endforeach()

message(STATUS "GHOST_MULTIHOP_ROUTING_CONTRACT_PASS")
