if(NOT DEFINED KBE_SOURCE_ROOT OR NOT IS_DIRECTORY "${KBE_SOURCE_ROOT}")
    message(FATAL_ERROR "KBE_SOURCE_ROOT must identify the KBEngine source tree")
endif()

set(_kbe_csharp_root "${KBE_SOURCE_ROOT}/../res/sdk_templates/client/csharp")
set(_kbe_app_file "${_kbe_csharp_root}/KBEngine.cs")
set(_kbe_provider_file "${_kbe_csharp_root}/NetworkProvider.cs")
set(_kbe_session_file "${_kbe_csharp_root}/NetworkSession.cs")
set(_kbe_tcp_file "${_kbe_csharp_root}/TcpNetworkProvider.cs")
set(_kbe_kcp_file "${_kbe_csharp_root}/KcpNetworkProvider.cs")
set(_kbe_args_file "${_kbe_csharp_root}/KBEngineArgs.cs")
set(_kbe_readme_file "${_kbe_csharp_root}/README.md")

foreach(_kbe_file IN ITEMS
        "${_kbe_app_file}"
        "${_kbe_provider_file}"
        "${_kbe_session_file}"
        "${_kbe_tcp_file}"
        "${_kbe_kcp_file}"
        "${_kbe_args_file}"
        "${_kbe_readme_file}")
    if(NOT EXISTS "${_kbe_file}")
        message(FATAL_ERROR "C# network Provider contract input is missing: ${_kbe_file}")
    endif()
endforeach()

file(READ "${_kbe_app_file}" _kbe_app)
file(READ "${_kbe_provider_file}" _kbe_provider)
file(READ "${_kbe_session_file}" _kbe_session)
file(READ "${_kbe_tcp_file}" _kbe_tcp)
file(READ "${_kbe_kcp_file}" _kbe_kcp)
file(READ "${_kbe_args_file}" _kbe_args)
file(READ "${_kbe_readme_file}" _kbe_readme)

function(kbe_require_literal _text_variable _literal _description)
    string(FIND "${${_text_variable}}" "${_literal}" _kbe_position)
    if(_kbe_position EQUAL -1)
        message(FATAL_ERROR "${_description}: missing '${_literal}'")
    endif()
endfunction()

function(kbe_forbid_literal _text_variable _literal _description)
    string(FIND "${${_text_variable}}" "${_literal}" _kbe_position)
    if(NOT _kbe_position EQUAL -1)
        message(FATAL_ERROR "${_description}: forbidden '${_literal}'")
    endif()
endfunction()

# 模式是稳定的产品选择，Provider 是外层扩展点；loginapp/baseapp 路由不得重新隐式回退。
# Modes are stable product choices while Providers are the outer extension point; role routing must not regain implicit fallback.
foreach(_kbe_mode IN ITEMS "TCP = 0" "KCP = 1" "CUSTOM = 2" "CUSTOM_ALL = 3")
    kbe_require_literal(_kbe_app "${_kbe_mode}" "C# network mode matrix is incomplete")
endforeach()
kbe_require_literal(_kbe_app "if (context.Role == NetworkConnectionRole.Loginapp)"
    "KCP and CUSTOM must route loginapp explicitly")
kbe_require_literal(_kbe_app "KCP baseapp connection requires a non-zero UDP port."
    "KCP must reject a missing UDP endpoint instead of falling back to TCP")
kbe_require_literal(_kbe_app "case NETWORK_TYPE.CUSTOM_ALL:"
    "CUSTOM_ALL routing is missing")
kbe_require_literal(_kbe_app "_args.customNetworkProviderFactory.Create(context)"
    "Custom connections must create a Provider through the configured factory")
kbe_require_literal(_kbe_args "public INetworkProviderFactory customNetworkProviderFactory = null;"
    "Custom Provider factory configuration is missing")

# Session 独占协议解析和 codec；内置 Provider 只在 Process 排空事件。
# The Session owns protocol parsing and codecs; built-in Providers drain queued events only from Process.
kbe_require_literal(_kbe_session "private readonly KBEMessageReader _messageReader;"
    "NetworkSession must own the KBE message reader")
kbe_require_literal(_kbe_session "_provider.Process();"
    "NetworkSession must advance the Provider from the SDK process loop")
kbe_require_literal(_kbe_provider "void Process();"
    "The Provider process entry point is missing")
kbe_require_literal(_kbe_tcp "EnqueueEvent(ProviderEvent.Connected());"
    "TCP worker completion must be queued")
kbe_require_literal(_kbe_tcp "DrainEvents();"
    "TCP Process must drain queued callbacks")
kbe_require_literal(_kbe_kcp "EnqueueEvent(ProviderEvent.Connected());"
    "KCP worker completion must be queued")
kbe_require_literal(_kbe_kcp "DrainEvents();"
    "KCP Process must drain queued callbacks")

foreach(_kbe_forbidden IN ITEMS
        "UNITY_WEB_SOCKET"
        "processMainThread"
        "platformTick"
        "SupportsMultiThreadMode")
    kbe_forbid_literal(_kbe_app "${_kbe_forbidden}" "KBEngineApp must retain one developer-controlled process loop")
    kbe_forbid_literal(_kbe_provider "${_kbe_forbidden}" "Provider must not choose the SDK threading model")
endforeach()

foreach(_kbe_legacy_file IN ITEMS
        "NetworkInterfaceBase.cs"
        "NetworkInterfaceTCP.cs"
        "NetworkInterfaceKCP.cs"
        "NetworkInterfaceUnityWS.cs"
        "MessageReaderBase.cs"
        "PacketReceiverBase.cs"
        "PacketSenderBase.cs")
    if(EXISTS "${_kbe_csharp_root}/${_kbe_legacy_file}")
        message(FATAL_ERROR "Legacy C# network layer must remain removed: ${_kbe_legacy_file}")
    endif()
endforeach()

kbe_require_literal(_kbe_readme "| `CUSTOM_ALL` | custom Provider | custom Provider |"
    "The public network mode table is not documented")
kbe_require_literal(_kbe_readme "isMultiThreads = false"
    "The developer-controlled single-thread integration is not documented")
kbe_require_literal(_kbe_readme "only enqueue results"
    "The Provider callback serialization contract is not documented")

message(STATUS "CSHARP_NETWORK_PROVIDER_CONTRACT_PASS")
