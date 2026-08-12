if(NOT DEFINED KBE_BOTS_SOURCE OR NOT EXISTS "${KBE_BOTS_SOURCE}")
    message(FATAL_ERROR "KBE_BOTS_SOURCE must identify server/tools/bots/bots.cpp")
endif()

file(READ "${KBE_BOTS_SOURCE}" _kbe_bots_source)

string(FIND "${_kbe_bots_source}" "bool Bots::initialize()" _kbe_initialize_begin)
string(FIND "${_kbe_bots_source}" "bool Bots::initializeWatcher()" _kbe_initialize_end)
if(_kbe_initialize_begin EQUAL -1 OR _kbe_initialize_end EQUAL -1 OR
        NOT _kbe_initialize_begin LESS _kbe_initialize_end)
    message(FATAL_ERROR "Cannot isolate Bots::initialize")
endif()

math(EXPR _kbe_initialize_length "${_kbe_initialize_end} - ${_kbe_initialize_begin}")
string(SUBSTRING "${_kbe_bots_source}" ${_kbe_initialize_begin}
    ${_kbe_initialize_length} _kbe_initialize_body)

# Machine visibility and internal heartbeats are one development-only policy.
# Machine 可见性与内部组件心跳必须由同一个开发模式策略控制，避免普通压测进程重新进入组件目录。
string(FIND "${_kbe_initialize_body}" "if (g_botsDevMode)" _kbe_dev_guard)
string(FIND "${_kbe_initialize_body}"
    "this->dispatcher().addTask(&Components::getSingleton());" _kbe_registration)
string(FIND "${_kbe_initialize_body}"
    "componentPublishingEnabled_ = true;" _kbe_registration_state)
string(FIND "${_kbe_initialize_body}"
    "pActiveReportHandler_->start" _kbe_active_report)

if(_kbe_dev_guard EQUAL -1 OR _kbe_registration EQUAL -1 OR
        _kbe_registration_state EQUAL -1 OR _kbe_active_report EQUAL -1 OR
        NOT _kbe_dev_guard LESS _kbe_registration OR
        NOT _kbe_registration LESS _kbe_registration_state OR
        NOT _kbe_registration_state LESS _kbe_active_report)
    message(FATAL_ERROR
        "Bots component registration and active reporting must remain in the --dev initialization path")
endif()

string(REGEX MATCHALL
    "addTask\\(&Components::getSingleton\\(\\)\\)"
    _kbe_registration_calls "${_kbe_bots_source}")
list(LENGTH _kbe_registration_calls _kbe_registration_call_count)
if(NOT _kbe_registration_call_count EQUAL 1)
    message(FATAL_ERROR
        "Bots must have exactly one Components registration call guarded by --dev")
endif()

string(REGEX MATCHALL
    "new BotsActiveReportHandler\\(this\\)"
    _kbe_active_report_constructions "${_kbe_bots_source}")
list(LENGTH _kbe_active_report_constructions _kbe_active_report_construction_count)
if(NOT _kbe_active_report_construction_count EQUAL 1)
    message(FATAL_ERROR
        "Bots must construct its component heartbeat publisher only in the --dev initialization path")
endif()

message(STATUS "BOTS_COMPONENT_VISIBILITY_CONTRACT_PASS")
