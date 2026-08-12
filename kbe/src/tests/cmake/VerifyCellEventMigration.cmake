if(NOT DEFINED KBE_SOURCE_ROOT OR NOT IS_DIRECTORY "${KBE_SOURCE_ROOT}")
    message(FATAL_ERROR "KBE_SOURCE_ROOT must identify the KBEngine source tree")
endif()

file(READ "${KBE_SOURCE_ROOT}/server/cellapp/entity.cpp" _kbe_cell_entity)
file(READ "${KBE_SOURCE_ROOT}/lib/entitydef/entity_events.h" _kbe_entity_events)
file(READ "${KBE_SOURCE_ROOT}/lib/entitydef/entity_events.cpp" _kbe_entity_events_source)

foreach(_kbe_required IN ITEMS
    "EntityEvents::addToStream(this, s);"
    "EntityEvents::createFromStream(this, s)"
)
    string(FIND "${_kbe_cell_entity}" "${_kbe_required}" _kbe_position)
    if(_kbe_position EQUAL -1)
        message(FATAL_ERROR "Cell Entity migration is missing: ${_kbe_required}")
    endif()
endforeach()

string(FIND "${_kbe_cell_entity}" "addTimersToStream(s);" _kbe_add_timers)
string(FIND "${_kbe_cell_entity}" "EntityEvents::addToStream(this, s);" _kbe_add_events)
string(FIND "${_kbe_cell_entity}" "pyCallbackMgr_.addToStream(s);" _kbe_add_callbacks)
if(NOT _kbe_add_timers LESS _kbe_add_events OR NOT _kbe_add_events LESS _kbe_add_callbacks)
    message(FATAL_ERROR "Cell event serialization order must match the migration stream contract")
endif()

string(FIND "${_kbe_cell_entity}" "createTimersFromStream(s);" _kbe_create_timers)
string(FIND "${_kbe_cell_entity}" "EntityEvents::createFromStream(this, s)" _kbe_create_events)
string(FIND "${_kbe_cell_entity}" "pyCallbackMgr_.createFromStream(s);" _kbe_create_callbacks)
if(NOT _kbe_create_timers LESS _kbe_create_events OR NOT _kbe_create_events LESS _kbe_create_callbacks)
    message(FATAL_ERROR "Cell event restoration order must match the migration stream contract")
endif()

# Only methods rebound from the destination Entity or its EntityComponent are portable across Cell processes.
# 只有从目标 Entity 或 EntityComponent 重新绑定的方法才能跨 Cell 进程安全恢复。
foreach(_kbe_required IN ITEMS
    "PyMethod_Check(callback)"
    "owner == entity"
    "PyObject_GetAttrString(owner, \"owner\")"
    "componentOwner == entity"
    "stringAttribute(owner, \"name\", description.ownerName)"
    "MAX_MIGRATED_SUBSCRIPTIONS"
    "stream.appendBlob(&eventStream)"
    "stream.readBlob(eventPayload)"
)
    string(FIND "${_kbe_entity_events_source}" "${_kbe_required}" _kbe_position)
    if(_kbe_position EQUAL -1)
        message(FATAL_ERROR "Cell event migration policy is missing: ${_kbe_required}")
    endif()
endforeach()

message(STATUS "CELL_EVENT_MIGRATION_CONTRACT_PASS")
