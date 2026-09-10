if(NOT DEFINED KBE_SOURCE_ROOT OR NOT IS_DIRECTORY "${KBE_SOURCE_ROOT}")
    message(FATAL_ERROR "KBE_SOURCE_ROOT must identify the KBEngine source tree")
endif()

set(_kbe_sdk_template_root "${KBE_SOURCE_ROOT}/../res/sdk_templates/client")
set(_kbe_gdscript_generator
    "${KBE_SOURCE_ROOT}/server/tools/kbcmd/client_sdk_gdscript.cpp")

set(_kbe_required_sources
    "${_kbe_sdk_template_root}/typescript/MemoryStream.ts"
    "${_kbe_sdk_template_root}/typescript/KBEngine.ts"
    "${_kbe_sdk_template_root}/cxx/KBEngine.cpp"
    "${_kbe_sdk_template_root}/gdscript/KBEngine.gd"
    "${_kbe_gdscript_generator}"
)
foreach(_kbe_source IN LISTS _kbe_required_sources)
    if(NOT EXISTS "${_kbe_source}")
        message(FATAL_ERROR "Client SDK init-property contract input is missing: ${_kbe_source}")
    endif()
endforeach()

file(READ "${_kbe_sdk_template_root}/typescript/MemoryStream.ts" _kbe_typescript_stream)
file(READ "${_kbe_sdk_template_root}/typescript/KBEngine.ts" _kbe_typescript_app)
file(READ "${_kbe_sdk_template_root}/cxx/KBEngine.cpp" _kbe_cxx_app)
file(READ "${_kbe_sdk_template_root}/gdscript/KBEngine.gd" _kbe_gdscript_app)
file(READ "${_kbe_gdscript_generator}" _kbe_gdscript_generator_source)

function(kbe_require_literal _text_variable _literal _description)
    string(FIND "${${_text_variable}}" "${_literal}" _kbe_literal_position)
    if(_kbe_literal_position EQUAL -1)
        message(FATAL_ERROR "${_description}: missing '${_literal}'")
    endif()
endfunction()

function(kbe_forbid_literal _text_variable _literal _description)
    string(FIND "${${_text_variable}}" "${_literal}" _kbe_literal_position)
    if(NOT _kbe_literal_position EQUAL -1)
        message(FATAL_ERROR "${_description}: forbidden '${_literal}'")
    endif()
endfunction()

# EnterWorld 前可能连续收到多条初始化属性消息。缓存必须只保留一个规范化的 eid 头，
# 并按网络线序追加每条消息的剩余 payload；否则后续属性会被丢弃或解析成伪 eid。
# Multiple initialization-property messages can precede EnterWorld. The cache must keep one
# normalized eid header and append every remaining payload in wire order, or later properties
# are either dropped or parsed as a synthetic eid.
kbe_require_literal(_kbe_typescript_stream "Append(data: ArrayBuffer): void"
    "TypeScript MemoryStream must support payload concatenation")
kbe_require_literal(_kbe_typescript_stream "this.wpos += bytes.byteLength;"
    "TypeScript MemoryStream append must advance the write cursor")
kbe_require_literal(_kbe_typescript_app "if (eid === 0)"
    "TypeScript optimized updates must reject unresolved aliases")
kbe_require_literal(_kbe_typescript_app "entityStream.Append(stream.GetBuffer());"
    "TypeScript must append repeated pre-EnterWorld payloads")
kbe_require_literal(_kbe_typescript_app "tempStream.WriteInt32(eid);"
    "TypeScript cached updates must write a normalized eid header")
kbe_require_literal(_kbe_typescript_app "tempStream.Append(stream.GetBuffer());"
    "TypeScript first cached update must copy only the unread payload")
kbe_forbid_literal(_kbe_typescript_app "tempStream.rpos = stream.rpos - 4;"
    "TypeScript must not reconstruct normal-message headers from the source cursor")
kbe_require_literal(_kbe_typescript_app "entity.callPropertysSetMethods();"
    "TypeScript initialization must dispatch to the generated property callback implementation")
kbe_forbid_literal(_kbe_typescript_app "entity.CallPropertysSetMethods();"
    "TypeScript initialization must not dispatch to the historical compatibility alias")
kbe_require_literal(_kbe_typescript_app "this.callPropertysSetMethods();"
    "TypeScript historical callback entry point must delegate to the generated implementation")

kbe_require_literal(_kbe_cxx_app "if (eid == 0)"
    "C++ optimized updates must reject unresolved aliases")
kbe_require_literal(_kbe_cxx_app
    "(*entityMessageFind)->append(stream.data(), stream.rpos(), stream.length());"
    "C++ must append repeated pre-EnterWorld payloads")
kbe_require_literal(_kbe_cxx_app "stream1->writeInt32(eid);"
    "C++ cached updates must write a normalized eid header")
kbe_require_literal(_kbe_cxx_app
    "stream1->append(stream.data(), stream.rpos(), stream.length());"
    "C++ first cached update must copy only the unread payload")
kbe_forbid_literal(_kbe_cxx_app "stream1->rpos(stream.rpos() - 4);"
    "C++ must not reconstruct normal-message headers from the source cursor")

kbe_require_literal(_kbe_gdscript_app "if _eid == 0:"
    "GDScript optimized updates must reject unresolved aliases")
kbe_require_literal(_kbe_gdscript_app
    "entityMessage.append(_stream.data(), _stream.rpos, _stream.length())"
    "GDScript must append repeated pre-EnterWorld payloads")
kbe_require_literal(_kbe_gdscript_app "stream1.writeInt32(_eid)"
    "GDScript cached updates must write a normalized eid header")
kbe_require_literal(_kbe_gdscript_app
    "stream1.append(_stream.data(), _stream.rpos, _stream.length())"
    "GDScript first cached update must copy only the unread payload")
kbe_forbid_literal(_kbe_gdscript_app "optimized update for entity"
    "GDScript must not drop optimized initialization payloads")

# 初始化反序列化只能填充数据。Base 属性在 __init__ 后、Cell 属性在 EnterWorld 后
# 才能响应运行期更新；显式补调还必须保留阶段和 OwnerOnly 约束。
# Initial deserialization may only populate state. Runtime Base updates become eligible after
# __init__, Cell updates after EnterWorld, and the explicit initial callback pass must retain
# both phase and OwnerOnly constraints.
kbe_require_literal(_kbe_gdscript_generator_source
    "const std::string lifecycleOwner = pEntityScriptDefModule->isComponentModule() ? \"owner.\" : \"self.\";"
    "GDScript components must use their owner lifecycle")
kbe_require_literal(_kbe_gdscript_generator_source "if self.inWorld:"
    "GDScript position updates must be gated by EnterWorld")
kbe_require_literal(_kbe_gdscript_generator_source "if {}inited:"
    "GDScript Base-property updates must be gated by initialization")
kbe_require_literal(_kbe_gdscript_generator_source "if {}inWorld:"
    "GDScript Cell-property updates must be gated by EnterWorld")
kbe_require_literal(_kbe_gdscript_generator_source
    "if {}inited and not {}inWorld:"
    "GDScript initial Base callbacks must run only before EnterWorld")
kbe_require_literal(_kbe_gdscript_generator_source
    "if not prop_{}.isOwnerOnly() or {}isPlayer():"
    "GDScript initial Cell callbacks must enforce OwnerOnly visibility")
kbe_forbid_literal(_kbe_gdscript_generator_source
    "sourcefileBody_ += fmt::format(\"\t\t\t\ton{}Changed(oldval_{})\n\", cap"
    "GDScript generated property deserialization must not call Changed unconditionally")

message(STATUS "CLIENT_SDK_INIT_PROPERTY_CONTRACT_PASS")
