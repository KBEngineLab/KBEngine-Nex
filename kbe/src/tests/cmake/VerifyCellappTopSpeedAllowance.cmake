if(NOT DEFINED KBE_CELLAPP_ENTITY_SOURCE OR NOT EXISTS "${KBE_CELLAPP_ENTITY_SOURCE}")
    message(FATAL_ERROR "KBE_CELLAPP_ENTITY_SOURCE must identify cellapp/entity.cpp")
endif()

if(NOT DEFINED KBE_CELLAPP_ENTITY_HEADER OR NOT EXISTS "${KBE_CELLAPP_ENTITY_HEADER}")
    message(FATAL_ERROR "KBE_CELLAPP_ENTITY_HEADER must identify cellapp/entity.h")
endif()

file(READ "${KBE_CELLAPP_ENTITY_SOURCE}" _kbe_entity_source)
file(READ "${KBE_CELLAPP_ENTITY_HEADER}" _kbe_entity_header)

string(FIND "${_kbe_entity_source}"
    "bool Entity::checkMoveForTopSpeed" _kbe_check_begin)
string(FIND "${_kbe_entity_source}"
    "void Entity::onUpdateDataFromClient" _kbe_check_end)
if(_kbe_check_begin EQUAL -1 OR _kbe_check_end EQUAL -1 OR
        NOT _kbe_check_begin LESS _kbe_check_end)
    message(FATAL_ERROR "Cannot isolate Entity::checkMoveForTopSpeed")
endif()

math(EXPR _kbe_check_length "${_kbe_check_end} - ${_kbe_check_begin}")
string(SUBSTRING "${_kbe_entity_source}" ${_kbe_check_begin}
    ${_kbe_check_length} _kbe_check_body)

# 短时额度必须由服务端经过 Tick 补充，并硬性限制为两个 Tick。
# The burst allowance must refill from elapsed server ticks and remain capped at two ticks.
foreach(_required IN ITEMS
        "static const GAME_TIME MAX_TOP_SPEED_ALLOWANCE_TICKS = 2;"
        "elapsedTicks = g_kbetime - lastTopSpeedCheckTick_;"
        "if (elapsedTicks == 0)"
        "elapsedTicks = 1;"
        "if (elapsedTicks > MAX_TOP_SPEED_ALLOWANCE_TICKS)"
        "elapsedTicks = MAX_TOP_SPEED_ALLOWANCE_TICKS;"
        "const GAME_TIME allowanceTicks = std::min("
        "elapsedTicks, MAX_TOP_SPEED_ALLOWANCE_TICKS"
        "topSpeed_ * MAX_TOP_SPEED_ALLOWANCE_TICKS"
        "topSpeedY_ * MAX_TOP_SPEED_ALLOWANCE_TICKS")
    string(FIND "${_kbe_check_body}" "${_required}" _required_pos)
    if(_required_pos EQUAL -1)
        message(FATAL_ERROR "Top-speed allowance contract is missing: ${_required}")
    endif()
endforeach()

# 窗口按服务端经过 Tick 而不是按上报次数计时，否则低频上报会被误判为持续超速。
# Window timing follows elapsed server ticks rather than report count, preventing false audits for low-rate reports.
foreach(_required IN ITEMS
        "remainingWindowTicks"
        "std::min(elapsedTicks, remainingWindowTicks)")
    string(FIND "${_kbe_check_body}" "${_required}" _required_pos)
    if(_required_pos EQUAL -1)
        message(FATAL_ERROR "Top-speed window timing contract is missing: ${_required}")
    endif()
endforeach()

# 额度必须按已接受的真实位移消耗；拒绝包不能进入同 Tick 或窗口累计。
# Accepted displacement must consume tokens, and rejected packets must not enter tick/window accounting.
foreach(_required IN ITEMS
        "topSpeedAllowance_ -= requiredXZ;"
        "topSpeedYAllowance_ -= requiredY;"
        "if (move)"
        "accumulatedMoveForTick_ += movement;")
    string(FIND "${_kbe_check_body}" "${_required}" _required_pos)
    if(_required_pos EQUAL -1)
        message(FATAL_ERROR "Top-speed consumption contract is missing: ${_required}")
    endif()
endforeach()

foreach(_field IN ITEMS
        "bool topSpeedCheckInitialized_;"
        "float topSpeedAllowance_;"
        "float topSpeedYAllowance_;")
    string(FIND "${_kbe_entity_header}" "${_field}" _field_pos)
    if(_field_pos EQUAL -1)
        message(FATAL_ERROR "Top-speed runtime state is missing: ${_field}")
    endif()
endforeach()

message(STATUS "CELLAPP_TOP_SPEED_ALLOWANCE_CONTRACT_PASS")
