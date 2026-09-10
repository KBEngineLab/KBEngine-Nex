if(NOT DEFINED KBE_CELLAPP_SOURCE OR NOT EXISTS "${KBE_CELLAPP_SOURCE}")
    message(FATAL_ERROR "KBE_CELLAPP_SOURCE must identify cellapp.cpp")
endif()
if(NOT DEFINED KBE_FORWARD_HANDLER_SOURCE OR NOT EXISTS "${KBE_FORWARD_HANDLER_SOURCE}")
    message(FATAL_ERROR "KBE_FORWARD_HANDLER_SOURCE must identify the CellApp forward handler")
endif()
if(NOT DEFINED KBE_FORWARD_BUFFER_SOURCE OR NOT EXISTS "${KBE_FORWARD_BUFFER_SOURCE}")
    message(FATAL_ERROR "KBE_FORWARD_BUFFER_SOURCE must identify forward_messagebuffer.cpp")
endif()

file(READ "${KBE_CELLAPP_SOURCE}" _kbe_cellapp_source)
file(READ "${KBE_FORWARD_HANDLER_SOURCE}" _kbe_forward_handler_source)
file(READ "${KBE_FORWARD_BUFFER_SOURCE}" _kbe_forward_buffer_source)

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

# Binding a client EntityCall must not attach Witness yet. Witness::attach sends EnterWorld
# immediately, so doing it here can overtake BaseApp's initial Cell-property message.
# 绑定客户端 EntityCall 时不得立即绑定 Witness。Witness::attach 会立即发送
# EnterWorld，否则就可能超过 BaseApp 的 Cell 初始属性消息。
string(FIND "${_kbe_cellapp_source}"
    "bool bindClientEntityCallForCreatedEntity" _kbe_bind_client_start)
string(FIND "${_kbe_cellapp_source}"
    "Cellapp::Cellapp(" _kbe_bind_client_end)
if(_kbe_bind_client_start EQUAL -1 OR _kbe_bind_client_end EQUAL -1 OR
        NOT _kbe_bind_client_start LESS _kbe_bind_client_end)
    message(FATAL_ERROR "Cannot isolate initial client EntityCall binding helper")
endif()
math(EXPR _kbe_bind_client_length "${_kbe_bind_client_end} - ${_kbe_bind_client_start}")
string(SUBSTRING "${_kbe_cellapp_source}" ${_kbe_bind_client_start}
    ${_kbe_bind_client_length} _kbe_bind_client_body)
string(FIND "${_kbe_bind_client_body}" "setWitness(" _kbe_bind_client_witness)
if(NOT _kbe_bind_client_witness EQUAL -1)
    message(FATAL_ERROR "Initial client binding must defer Witness attachment")
endif()

# Each direct creation path must put onEntityGetCell on the CellApp-to-BaseApp FIFO before
# attaching Witness. The forwarded EnterWorld then flushes the already queued initial properties.
# 每条直接创建路径都必须先将 onEntityGetCell 排入 CellApp 到 BaseApp 的 FIFO，
# 再绑定 Witness；随后 EnterWorld 立即发送时会一并刷出已排队的初始属性。
string(FIND "${_kbe_cellapp_source}"
    "void Cellapp::onCreateCellEntityInNewSpaceFromBaseapp" _kbe_new_space_start)
string(FIND "${_kbe_cellapp_source}"
    "void Cellapp::onRestoreSpaceInCellFromBaseapp" _kbe_restore_space_start)
string(FIND "${_kbe_cellapp_source}"
    "void Cellapp::requestRestore" _kbe_restore_space_end)
if(_kbe_new_space_start EQUAL -1 OR _kbe_restore_space_start EQUAL -1 OR
        NOT _kbe_new_space_start LESS _kbe_restore_space_start OR
        _kbe_restore_space_end EQUAL -1 OR NOT _kbe_restore_space_start LESS _kbe_restore_space_end)
    message(FATAL_ERROR "Cannot isolate new/restore Space creation handlers")
endif()

math(EXPR _kbe_new_space_length "${_kbe_restore_space_start} - ${_kbe_new_space_start}")
string(SUBSTRING "${_kbe_cellapp_source}" ${_kbe_new_space_start}
    ${_kbe_new_space_length} _kbe_new_space_body)
string(FIND "${_kbe_new_space_body}" "space->addEntityToNode(e);" _kbe_new_space_ready)
if(_kbe_new_space_ready EQUAL -1)
    message(FATAL_ERROR "Cannot locate direct new Space entity setup")
endif()
string(SUBSTRING "${_kbe_new_space_body}" ${_kbe_new_space_ready}
    -1 _kbe_new_space_direct_body)
string(FIND "${_kbe_new_space_direct_body}"
    "BaseappInterface::onEntityGetCellArgs3::staticAddToBundle" _kbe_new_space_ack)
string(FIND "${_kbe_new_space_direct_body}"
    "e->onGetWitness(true);" _kbe_new_space_witness)
if(_kbe_new_space_ack EQUAL -1 OR _kbe_new_space_witness EQUAL -1 OR
        NOT _kbe_new_space_ack LESS _kbe_new_space_witness)
    message(FATAL_ERROR "New Space creation must acknowledge BaseApp before synchronizing and attaching Witness")
endif()

math(EXPR _kbe_restore_space_length "${_kbe_restore_space_end} - ${_kbe_restore_space_start}")
string(SUBSTRING "${_kbe_cellapp_source}" ${_kbe_restore_space_start}
    ${_kbe_restore_space_length} _kbe_restore_space_body)
string(FIND "${_kbe_restore_space_body}" "e->onRestore();" _kbe_restore_ready)
if(_kbe_restore_ready EQUAL -1)
    message(FATAL_ERROR "Cannot locate restored Space entity setup")
endif()
string(SUBSTRING "${_kbe_restore_space_body}" ${_kbe_restore_ready}
    -1 _kbe_restore_direct_body)
string(FIND "${_kbe_restore_direct_body}"
    "BaseappInterface::onEntityGetCellArgs3::staticAddToBundle" _kbe_restore_ack)
string(FIND "${_kbe_restore_direct_body}"
    "e->setWitness(Witness::createPoolObject" _kbe_restore_witness)
if(_kbe_restore_ack EQUAL -1 OR _kbe_restore_witness EQUAL -1 OR
        NOT _kbe_restore_ack LESS _kbe_restore_witness)
    message(FATAL_ERROR "Restored Space creation must acknowledge BaseApp before attaching Witness")
endif()

string(FIND "${_kbe_cellapp_source}"
    "void Cellapp::_onCreateCellEntityFromBaseapp" _kbe_regular_create_start)
string(FIND "${_kbe_cellapp_source}"
    "void Cellapp::onDestroyCellEntityFromBaseapp" _kbe_regular_create_end)
if(_kbe_regular_create_start EQUAL -1 OR _kbe_regular_create_end EQUAL -1 OR
        NOT _kbe_regular_create_start LESS _kbe_regular_create_end)
    message(FATAL_ERROR "Cannot isolate regular Cell entity creation helper")
endif()
math(EXPR _kbe_regular_create_length
    "${_kbe_regular_create_end} - ${_kbe_regular_create_start}")
string(SUBSTRING "${_kbe_cellapp_source}" ${_kbe_regular_create_start}
    ${_kbe_regular_create_length} _kbe_regular_create_body)
string(FIND "${_kbe_regular_create_body}"
    "BaseappInterface::onEntityGetCellArgs3::staticAddToBundle" _kbe_regular_create_ack)
string(FIND "${_kbe_regular_create_body}"
    "e->onGetWitness(true);" _kbe_regular_create_witness)
if(_kbe_regular_create_ack EQUAL -1 OR _kbe_regular_create_witness EQUAL -1 OR
        NOT _kbe_regular_create_ack LESS _kbe_regular_create_witness)
    message(FATAL_ERROR "Regular Cell creation must acknowledge BaseApp before synchronizing and attaching Witness")
endif()

# When BaseApp discovery is delayed, ForwardComponent_MessageBuffer must send its saved
# onEntityGetCell bundle before the completion handler attaches Witness.
# BaseApp 延迟可用时，ForwardComponent_MessageBuffer 必须先发送已缓存的
# onEntityGetCell，然后才由完成回调绑定 Witness。
string(FIND "${_kbe_forward_buffer_source}"
    "bool ForwardComponent_MessageBuffer::process()" _kbe_forward_buffer_start)
string(FIND "${_kbe_forward_buffer_source}"
    "ForwardAnywhere_MessageBuffer::ForwardAnywhere_MessageBuffer" _kbe_forward_buffer_end)
if(_kbe_forward_buffer_start EQUAL -1 OR _kbe_forward_buffer_end EQUAL -1 OR
        NOT _kbe_forward_buffer_start LESS _kbe_forward_buffer_end)
    message(FATAL_ERROR "Cannot isolate ForwardComponent_MessageBuffer::process")
endif()
math(EXPR _kbe_forward_buffer_length
    "${_kbe_forward_buffer_end} - ${_kbe_forward_buffer_start}")
string(SUBSTRING "${_kbe_forward_buffer_source}" ${_kbe_forward_buffer_start}
    ${_kbe_forward_buffer_length} _kbe_forward_buffer_body)
string(FIND "${_kbe_forward_buffer_body}"
    "pChannel->send((*itervec)->pBundle);" _kbe_forward_buffer_send)
string(FIND "${_kbe_forward_buffer_body}"
    "pHandler->process();" _kbe_forward_buffer_handler)
if(_kbe_forward_buffer_send EQUAL -1 OR _kbe_forward_buffer_handler EQUAL -1 OR
        NOT _kbe_forward_buffer_send LESS _kbe_forward_buffer_handler)
    message(FATAL_ERROR "Forward buffer must send the saved bundle before its completion handler")
endif()

string(FIND "${_kbe_forward_handler_source}"
    "FMH_Baseapp_onEntityGetCellFrom_onCreateCellEntityInNewSpaceFromBaseapp::process()"
    _kbe_forward_handler_start)
string(FIND "${_kbe_forward_handler_source}"
    "FMH_Baseapp_onEntityGetCellFrom_onCreateCellEntityFromBaseapp::"
    _kbe_forward_handler_end)
if(_kbe_forward_handler_start EQUAL -1 OR _kbe_forward_handler_end EQUAL -1 OR
        NOT _kbe_forward_handler_start LESS _kbe_forward_handler_end)
    message(FATAL_ERROR "Cannot isolate delayed new Space completion handler")
endif()
math(EXPR _kbe_forward_handler_length
    "${_kbe_forward_handler_end} - ${_kbe_forward_handler_start}")
string(SUBSTRING "${_kbe_forward_handler_source}" ${_kbe_forward_handler_start}
    ${_kbe_forward_handler_length} _kbe_forward_handler_body)
string(FIND "${_kbe_forward_handler_body}"
    "_e->onGetWitness(true);" _kbe_forward_handler_witness)
if(_kbe_forward_handler_witness EQUAL -1)
    message(FATAL_ERROR "Delayed new Space creation must synchronize and attach Witness in the completion handler")
endif()

message(STATUS "CELLAPP_CREATE_ENTITY_STREAM_CONTRACT_PASS")
