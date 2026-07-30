if(NOT EXISTS "${KBE_MESSAGES_FIXED}")
    message(FATAL_ERROR "Fixed-message configuration is missing: ${KBE_MESSAGES_FIXED}")
endif()
if(NOT EXISTS "${KBE_WATCHER_CLIENT}")
    message(FATAL_ERROR "Watcher client is missing: ${KBE_WATCHER_CLIENT}")
endif()

file(READ "${KBE_MESSAGES_FIXED}" _messages_fixed)
file(READ "${KBE_WATCHER_CLIENT}" _watcher_client)

# Bots 的固定查询 ID 必须与远程客户端映射保持一致，否则压测只能启动却无法读取运行指标。
# The fixed Bots query ID must match the remote client mapping, otherwise a load run can start but cannot read runtime metrics.
string(REGEX MATCH "<Bots::queryWatcher>[\t\r\n ]*<id>([0-9]+)</id>" _bots_message "${_messages_fixed}")
if(NOT _bots_message)
    message(FATAL_ERROR "Bots::queryWatcher does not have a fixed protocol ID")
endif()
set(_bots_message_id "${CMAKE_MATCH_1}")

string(REGEX MATCH "Define\\.BOTS_TYPE[\t ]*:[\t ]*([0-9]+)" _bots_client "${_watcher_client}")
if(NOT _bots_client)
    message(FATAL_ERROR "pycommon.Watcher does not map BOTS_TYPE")
endif()
set(_bots_client_id "${CMAKE_MATCH_1}")

if(NOT _bots_message_id STREQUAL _bots_client_id)
    message(FATAL_ERROR
        "Bots watcher protocol mismatch: server=${_bots_message_id}, client=${_bots_client_id}")
endif()

# 不同组件的消息表允许复用低位 ID，但远程 Watcher 使用的固定 ID 必须只有这一处定义。
# Separate component message tables may reuse low IDs, but the remote Watcher fixed ID must have exactly one definition.
string(REGEX MATCHALL "<id>${_bots_message_id}</id>" _bots_fixed_id_tags "${_messages_fixed}")
list(LENGTH _bots_fixed_id_tags _bots_fixed_id_count)
if(NOT _bots_fixed_id_count EQUAL 1)
    message(FATAL_ERROR
        "Bots watcher protocol ID ${_bots_message_id} appears ${_bots_fixed_id_count} times")
endif()

message(STATUS "Bots watcher protocol ID ${_bots_message_id} is consistent and unique")
