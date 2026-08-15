if(NOT DEFINED KBE_SOURCE_ROOT OR NOT IS_DIRECTORY "${KBE_SOURCE_ROOT}")
    message(FATAL_ERROR "KBE_SOURCE_ROOT must identify the source directory")
endif()

set(_entity_app "${KBE_SOURCE_ROOT}/lib/server/entity_app.h")
set(_network_interface "${KBE_SOURCE_ROOT}/lib/network/network_interface.cpp")
set(_baseapp "${KBE_SOURCE_ROOT}/server/baseapp/baseapp.cpp")
set(_proxy "${KBE_SOURCE_ROOT}/server/baseapp/proxy.cpp")
set(_pending_login "${KBE_SOURCE_ROOT}/lib/server/pendingLoginmgr.h")
file(READ "${_entity_app}" _entity_app_source)
file(READ "${_network_interface}" _network_interface_source)
file(READ "${_baseapp}" _baseapp_source)
file(READ "${_proxy}" _proxy_source)
file(READ "${_pending_login}" _pending_login_source)

# 客户端 EntityCall 禁止恢复为地址裸查找，否则同机 TCP/UDP 临时端口重号会串入内部连接。
# Client EntityCalls must never regress to address-only lookup because local TCP/UDP ephemeral ports can collide.
string(FIND "${_entity_app_source}"
    "findExternalChannel(\n\t\t\tentitycall.addr(), entitycall.id())" _identity_lookup)
string(FIND "${_entity_app_source}"
    "findChannel(entitycall.addr())" _address_only_lookup)
if(_identity_lookup EQUAL -1 OR NOT _address_only_lookup EQUAL -1)
    message(FATAL_ERROR "EntityCall client routing must use external channel and Proxy identity")
endif()

# 身份查询必须显式覆盖两个传输命名空间，并同时验证 external 与 proxyID。
# Identity lookup must inspect both transport namespaces and validate external scope plus proxyID.
foreach(_required
        "findChannel(addr, protocol)"
        "PROTOCOL_TCP"
        "PROTOCOL_UDP"
        "candidate->isExternal()"
        "candidate->proxyID() != proxyID")
    string(FIND "${_network_interface_source}" "${_required}" _required_pos)
    if(_required_pos EQUAL -1)
        message(FATAL_ERROR "NetworkInterface external identity lookup is missing: ${_required}")
    endif()
endforeach()

# 异步 DB 查询必须保留登录 Channel 的传输类型和生命周期，禁止在回调时退回地址裸查找。
# Async DB queries must retain the login Channel transport and lifetime instead of falling back to address-only callback lookup.
foreach(_required
        "clientProtocolType"
        "clientChannelEpoch")
    string(FIND "${_pending_login_source}" "${_required}" _required_pos)
    if(_required_pos EQUAL -1)
        message(FATAL_ERROR "Pending login Channel identity is missing: ${_required}")
    endif()
endforeach()
foreach(_required
        "ptinfos->clientProtocolType = pChannel->protocoltype()"
        "ptinfos->clientChannelEpoch = pChannel->sessionEpoch()"
        "ptinfos->addr, ptinfos->clientProtocolType"
        "pClientChannel->sessionEpoch() != ptinfos->clientChannelEpoch")
    string(FIND "${_baseapp_source}" "${_required}" _required_pos)
    if(_required_pos EQUAL -1)
        message(FATAL_ERROR "BaseApp pending login Channel identity is missing: ${_required}")
    endif()
endforeach()
string(FIND "${_baseapp_source}"
    "findChannel(ptinfos->addr);" _pending_login_address_only_lookup)
if(NOT _pending_login_address_only_lookup EQUAL -1)
    message(FATAL_ERROR "BaseApp DB callback must not restore a pending client Channel by address alone")
endif()

# 首次绑定必须使用登录处理器已经持有的 Channel；此时 proxyID 尚未建立，严格身份查询必然返回空。
# Bootstrap binding must use the login handler's explicit Channel because strict identity lookup cannot succeed before proxyID is established.
foreach(_required
        "createClientProxies(Proxy* pEntity, Network::Channel* pChannel, bool reload,"
        "pChannel->addr() == pEntity->clientEntityCall()->addr()"
        "!addressMatched"
        "expectedPreviousProxyID > 0 && boundProxyID == expectedPreviousProxyID"
        "pChannel->proxyID(pEntity->id())"
        "createClientProxies(pEntity, pChannel, true)"
        "createClientProxies(pEntity, pClientChannel)")
    string(FIND "${_baseapp_source}" "${_required}" _required_pos)
    if(_required_pos EQUAL -1)
        message(FATAL_ERROR "BaseApp client-channel bootstrap is missing: ${_required}")
    endif()
endforeach()

# giveClientTo 必须证明当前绑定属于源 Proxy，不能把任意已绑定 Channel 覆盖给目标 Proxy。
# giveClientTo must prove that the current binding belongs to its source Proxy before replacing the owner.
foreach(_required
        "proxy->onGiveClientTo(lpChannel, this->id())"
        "createClientProxies(this, lpChannel, false, previousProxyID)")
    string(FIND "${_proxy_source}" "${_required}" _required_pos)
    if(_required_pos EQUAL -1)
        message(FATAL_ERROR "Proxy client ownership transfer is missing: ${_required}")
    endif()
endforeach()

string(FIND "${_baseapp_source}"
    "pEntity->clientEntityCall()->getChannel();\n\tpChannel->proxyID" _implicit_bootstrap_lookup)
if(NOT _implicit_bootstrap_lookup EQUAL -1)
    message(FATAL_ERROR "BaseApp client bootstrap must not depend on a pre-bound EntityCall lookup")
endif()

message(STATUS "ENTITYCALL_TRANSPORT_ROUTING_CONTRACT_PASS")
