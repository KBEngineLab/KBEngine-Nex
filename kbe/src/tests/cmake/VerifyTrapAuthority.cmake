if(NOT DEFINED KBE_SOURCE_ROOT OR NOT IS_DIRECTORY "${KBE_SOURCE_ROOT}")
    message(FATAL_ERROR "KBE_SOURCE_ROOT must identify the kbe/src directory")
endif()

file(READ "${KBE_SOURCE_ROOT}/server/cellapp/trap_trigger.cpp" _trap_trigger)

string(FIND "${_trap_trigger}" "void TrapTrigger::onEnter" _enter_begin)
string(FIND "${_trap_trigger}" "void TrapTrigger::onLeave" _leave_begin)
string(LENGTH "${_trap_trigger}" _source_length)
if(_enter_begin EQUAL -1 OR _leave_begin EQUAL -1 OR
        NOT _enter_begin LESS _leave_begin OR NOT _leave_begin LESS _source_length)
    message(FATAL_ERROR "Cannot isolate TrapTrigger authority handlers")
endif()

math(EXPR _enter_length "${_leave_begin} - ${_enter_begin}")
math(EXPR _leave_length "${_source_length} - ${_leave_begin}")
string(SUBSTRING "${_trap_trigger}" ${_enter_begin} ${_enter_length} _enter_body)
string(SUBSTRING "${_trap_trigger}" ${_leave_begin} ${_leave_length} _leave_body)

foreach(_handler_body IN ITEMS "${_enter_body}" "${_leave_body}")
    foreach(_required
            "pEntity == NULL"
            "pEntity->isDestroyed()"
            "!pEntity->isReal()")
        string(FIND "${_handler_body}" "${_required}" _required_pos)
        if(_required_pos EQUAL -1)
            message(FATAL_ERROR "TrapTrigger handler must reject non-authoritative callbacks: ${_required}")
        endif()
    endforeach()
endforeach()

message(STATUS "TRAP_AUTHORITY_CONTRACT_PASS")
