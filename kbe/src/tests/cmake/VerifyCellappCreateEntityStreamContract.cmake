if(NOT DEFINED KBE_CELLAPP_SOURCE OR NOT EXISTS "${KBE_CELLAPP_SOURCE}")
    message(FATAL_ERROR "KBE_CELLAPP_SOURCE must identify cellapp.cpp")
endif()

file(READ "${KBE_CELLAPP_SOURCE}" _kbe_cellapp_source)

string(FIND "${_kbe_cellapp_source}"
    "void Cellapp::onCreateCellEntityFromBaseapp" _kbe_function_start)
string(FIND "${_kbe_cellapp_source}"
    "void Cellapp::_onCreateCellEntityFromBaseapp" _kbe_function_end)
if(_kbe_function_start EQUAL -1 OR _kbe_function_end EQUAL -1 OR
        NOT _kbe_function_start LESS _kbe_function_end)
    message(FATAL_ERROR "Cannot isolate Cellapp::onCreateCellEntityFromBaseapp")
endif()

math(EXPR _kbe_function_length "${_kbe_function_end} - ${_kbe_function_start}")
string(SUBSTRING "${_kbe_cellapp_source}" ${_kbe_function_start}
    ${_kbe_function_length} _kbe_create_from_baseapp)

# 缓冲分支必须先复制剩余载荷，再结束原始网络流，避免 PacketReader 将正文当成下一条消息。
# The buffered branch must copy the remaining payload and then finish the original network stream.
string(FIND "${_kbe_create_from_baseapp}"
    "pCellData->append(s);" _kbe_buffer_copy)
string(FIND "${_kbe_create_from_baseapp}"
    "Network::Bundle* pBundle" _kbe_buffer_bundle)
if(NOT _kbe_buffer_copy EQUAL -1 AND NOT _kbe_buffer_bundle EQUAL -1 AND
        _kbe_buffer_copy LESS _kbe_buffer_bundle)
    math(EXPR _kbe_buffer_length "${_kbe_buffer_bundle} - ${_kbe_buffer_copy}")
    string(SUBSTRING "${_kbe_create_from_baseapp}" ${_kbe_buffer_copy}
        ${_kbe_buffer_length} _kbe_buffer_body)
    string(FIND "${_kbe_buffer_body}" "s.done();" _kbe_buffered_stream_done)
else()
    set(_kbe_buffered_stream_done -1)
endif()
if(_kbe_buffered_stream_done EQUAL -1)
    message(FATAL_ERROR "Buffered Cell entity creation must consume the original network stream")
endif()

# helper 会在 Space 销毁等生命周期竞态下提前返回；入口必须在调用后无条件闭合变量消息。
# The helper can return early on lifecycle races such as Space destruction; ingress must always close the variable message.
string(FIND "${_kbe_create_from_baseapp}"
    "_onCreateCellEntityFromBaseapp(entityType" _kbe_helper_dispatch)
if(NOT _kbe_helper_dispatch EQUAL -1)
    string(SUBSTRING "${_kbe_create_from_baseapp}" ${_kbe_helper_dispatch}
        -1 _kbe_after_helper_dispatch)
    string(FIND "${_kbe_after_helper_dispatch}" "s.done();" _kbe_direct_stream_done)
else()
    set(_kbe_direct_stream_done -1)
endif()
if(_kbe_direct_stream_done EQUAL -1)
    message(FATAL_ERROR "Direct Cell entity creation must consume the network stream after helper dispatch")
endif()

message(STATUS "CELLAPP_CREATE_ENTITY_STREAM_CONTRACT_PASS")
