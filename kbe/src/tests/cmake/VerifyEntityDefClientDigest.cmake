function(require_source_fragment path fragment description)
    file(READ "${path}" source_text)
    string(FIND "${source_text}" "${fragment}" fragment_offset)
    if(fragment_offset EQUAL -1)
        message(FATAL_ERROR "Missing ${description} in ${path}: ${fragment}")
    endif()
endfunction()

set(entitydef_source "${KBE_SOURCE_ROOT}/lib/entitydef/entitydef.cpp")
set(dbmgr_source "${KBE_SOURCE_ROOT}/server/dbmgr/dbmgr.cpp")
set(bots_source "${KBE_SOURCE_ROOT}/server/tools/bots/bots.cpp")
set(entity_app_source "${KBE_SOURCE_ROOT}/lib/server/entity_app.h")

# The login digest is configuration-sensitive because aliases change wire widths.
# 登录摘要必须包含别名开关，因为这些开关会改变线协议字段宽度。
require_source_fragment("${entitydef_source}"
    "const uint8 entityAliasEnabled = entityAliasID() ? 1 : 0;"
    "entity alias switch in the client digest")
require_source_fragment("${entitydef_source}"
    "const uint8 entityDefAliasEnabled = entitydefAliasID() ? 1 : 0;"
    "EntityDef alias switch in the client digest")

foreach(runtime_source IN ITEMS "${dbmgr_source}" "${bots_source}" "${entity_app_source}")
    require_source_fragment("${runtime_source}"
        "EntityDef::entityAliasID(ServerConfig::getSingleton().getCellApp().aliasEntityID);"
        "entity alias configuration before EntityDef initialization")
    require_source_fragment("${runtime_source}"
        "EntityDef::entitydefAliasID(ServerConfig::getSingleton().getCellApp().entitydefAliasID);"
        "EntityDef alias configuration before EntityDef initialization")
endforeach()
