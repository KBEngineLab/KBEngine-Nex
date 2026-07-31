if(NOT DEFINED KBE_SOURCE_ROOT OR NOT IS_DIRECTORY "${KBE_SOURCE_ROOT}")
    message(FATAL_ERROR "KBE_SOURCE_ROOT must identify the KBEngine source tree")
endif()

set(_kbe_sensitive_sources
    "lib/common/blowfish.cpp"
    "lib/server/serverapp.cpp"
    "lib/client_lib/clientobjectbase.cpp"
    "server/baseapp/baseapp.cpp"
    "server/dbmgr/dbmgr.cpp"
    "server/dbmgr/dbtasks.cpp"
    "server/loginapp/loginapp.cpp"
    "server/loginapp/http_cb_handler.cpp"
	"server/tools/interfaces/interfaces.cpp"
	"server/dbmgr/interfaces_handler.cpp"
    "lib/db_mysql/kbe_table_mysql.cpp"
    "lib/db_mongodb/kbe_table_mongodb.cpp"
    "lib/db_redis/kbe_table_redis.cpp"
)

set(_kbe_sensitive_text "")
foreach(_kbe_relative_path IN LISTS _kbe_sensitive_sources)
    set(_kbe_source "${KBE_SOURCE_ROOT}/${_kbe_relative_path}")
    if(NOT EXISTS "${_kbe_source}")
        message(FATAL_ERROR "Security source contract input is missing: ${_kbe_source}")
    endif()
    file(READ "${_kbe_source}" _kbe_source_text)
    string(APPEND _kbe_sensitive_text "\n${_kbe_source_text}")
endforeach()

# These literals previously logged bearer/session or account-verification secrets.
# 这些文本曾将 bearer/session 或账户验证码写入日志，后续调试不得恢复。
set(_kbe_forbidden_sensitive_literals
    "Using Blowfish key:"
    "encryptedKey={}"
    "logoutBaseapp: key={}"
    "reloginBaseapp: accountName={}, key={}"
    "uuid={}"
    "rndUUID={}"
    "code_={}"
    "scode={}"
    "activateAccount({})"
    "bindEMail({})"
    "resetpassword({})"
    "code({}) username"
    "code = {}"
	"invalid email={}"
	"email={}, failedcode"
	"datas={2}"
	"datas={3}"
	"extra datas = '{}'"
	", datas={}"
)

foreach(_kbe_forbidden IN LISTS _kbe_forbidden_sensitive_literals)
    string(FIND "${_kbe_sensitive_text}" "${_kbe_forbidden}" _kbe_forbidden_position)
    if(NOT _kbe_forbidden_position EQUAL -1)
        message(FATAL_ERROR "Sensitive log contract regressed: ${_kbe_forbidden}")
    endif()
endforeach()

file(READ "${KBE_SOURCE_ROOT}/server/baseapp/baseapp.cpp" _kbe_baseapp)
file(READ "${KBE_SOURCE_ROOT}/server/baseappmgr/baseappmgr.cpp" _kbe_baseappmgr)
file(READ "${KBE_SOURCE_ROOT}/server/cellapp/cellapp.cpp" _kbe_cellapp)
file(READ "${KBE_SOURCE_ROOT}/server/cellappmgr/cellappmgr.cpp" _kbe_cellappmgr)
file(READ "${KBE_SOURCE_ROOT}/server/dbmgr/dbmgr.cpp" _kbe_dbmgr)
file(READ "${KBE_SOURCE_ROOT}/server/dbmgr/interfaces_handler.cpp" _kbe_interfaces_handler)
file(READ "${KBE_SOURCE_ROOT}/server/loginapp/loginapp.cpp" _kbe_loginapp)
file(READ "${KBE_SOURCE_ROOT}/lib/server/entity_app.h" _kbe_entity_app)
file(READ "${KBE_SOURCE_ROOT}/lib/server/serverapp.cpp" _kbe_serverapp)
file(READ "${KBE_SOURCE_ROOT}/lib/server/serverapp.h" _kbe_serverapp_header)
file(READ "${KBE_SOURCE_ROOT}/lib/server/globaldata_server.cpp" _kbe_globaldata_server)
file(READ "${KBE_SOURCE_ROOT}/lib/server/globaldata_client.cpp" _kbe_globaldata_client)
file(READ "${KBE_SOURCE_ROOT}/lib/db_interface/db_interface.cpp" _kbe_db_interface)

# Network-derived component IDs must use the fail-closed guard instead of a
# nullable dereference, assertion, or map insertion. 网络组件 ID 必须经过拒绝式守卫，
# 不能直接解引用、断言或通过 operator[] 创建幽灵组件。
set(_kbe_forbidden_routing_literals
    "findComponent(componentID)->pChannel"
    "KBE_ASSERT(false && \"Baseappmgr::forwardMessage"
    "KBE_ASSERT(cinfos != NULL && cinfos->pChannel != NULL)"
    "baseapps_[componentID]"
    "cellapps_[componentID]"
	"KBE_ASSERT(entityDBID > 0)"
	"KBE_ASSERT(false && \"dataType error!"
	"KBE_ASSERT(false && \"Cellapp::onCreateCellEntityFromBaseapp"
	"KBE_ASSERT(e->baseEntityCall() != NULL && !e->hasWitness())"
	"KBE_ASSERT(clientEntityCall != Py_None)"
	"DBUtil::pThreadPool(g_kbeSrvConfig.dbInterfaceIndex2dbInterfaceName(dbInterfaceIndex))->"
	"bufferedDBTasksMaps_[g_kbeSrvConfig.dbInterfaceIndex2dbInterfaceName(dbInterfaceIndex)]"
	"findBestInterfacesHandler()->"
	"KBE_ASSERT(pInterfacesChannel)"
	"KBE_ASSERT(addr_ != Network::Address::NONE)"
	"Dbmgr::accountBindMail: username={}"
)
foreach(_kbe_forbidden IN LISTS _kbe_forbidden_routing_literals)
	string(FIND "${_kbe_baseapp}\n${_kbe_baseappmgr}\n${_kbe_cellapp}\n${_kbe_cellappmgr}\n${_kbe_dbmgr}\n${_kbe_interfaces_handler}"
        "${_kbe_forbidden}" _kbe_forbidden_position)
    if(NOT _kbe_forbidden_position EQUAL -1)
        message(FATAL_ERROR "Component routing contract regressed: ${_kbe_forbidden}")
    endif()
endforeach()

foreach(_kbe_required IN ITEMS
	"GlobalDataServer::broadcastDataChanged: skipped unavailable"
	"GlobalDataClient::onDataChanged: skipped unavailable"
)
	string(FIND "${_kbe_globaldata_server}\n${_kbe_globaldata_client}"
		"${_kbe_required}" _kbe_required_position)
	if(_kbe_required_position EQUAL -1)
		message(FATAL_ERROR "GlobalData unavailable-channel guard is missing: ${_kbe_required}")
	endif()
endforeach()

foreach(_kbe_required IN ITEMS
	"validateSystemTables"
	"DBUtil::initInterface: missing system table"
	"KBE_TABLE_PERFIX \"_accountinfos\""
	"KBE_TABLE_PERFIX \"_entitylog\""
	"KBE_TABLE_PERFIX \"_email_verification\""
	"KBE_TABLE_PERFIX \"_serverlog\""
)
	string(FIND "${_kbe_db_interface}" "${_kbe_required}" _kbe_required_position)
	if(_kbe_required_position EQUAL -1)
		message(FATAL_ERROR "DB system-table initialization guard is missing: ${_kbe_required}")
	endif()
endforeach()

foreach(_kbe_required IN ITEMS
	"bool registerNewApp(Network::Channel* pChannel"
	"ServerApp::registerNewApp: rejected uid="
	"ServerApp::registerNewApp: rejected componentID conflict"
	"ServerApp::registerNewApp: rejected live binding replacement"
	"rejected unbound registration"
)
	string(FIND "${_kbe_serverapp_header}\n${_kbe_serverapp}"
		"${_kbe_required}" _kbe_required_position)
	if(_kbe_required_position EQUAL -1)
		message(FATAL_ERROR "Component registration guard is missing: ${_kbe_required}")
	endif()
endforeach()

foreach(_kbe_required IN ITEMS
	"!ServerApp::registerNewApp"
	"Dbmgr::onRegisterNewApp: rejected registration"
)
	string(FIND "${_kbe_dbmgr}" "${_kbe_required}" _kbe_required_position)
	if(_kbe_required_position EQUAL -1)
		message(FATAL_ERROR "DBMgr registration result guard is missing: ${_kbe_required}")
	endif()
endforeach()

foreach(_kbe_required IN ITEMS
	"Baseapp::onDbmgrInitCompleted: rejected non-DBMgr source"
	"Baseapp::onBroadcastBaseAppDataChanged: rejected non-DBMgr source"
	"Baseapp::registerPendingLogin: rejected non-BaseAppMgr source"
	"Cellapp::onDbmgrInitCompleted: rejected non-DBMgr source"
	"Cellapp::onBroadcastCellAppDataChanged: rejected non-DBMgr source"
	"Loginapp::onDbmgrInitCompleted: rejected non-DBMgr source"
	"EntityApp::onBroadcastGlobalDataChanged: rejected non-DBMgr source"
)
	string(FIND "${_kbe_baseapp}\n${_kbe_cellapp}\n${_kbe_loginapp}\n${_kbe_entity_app}"
		"${_kbe_required}" _kbe_required_position)
	if(_kbe_required_position EQUAL -1)
		message(FATAL_ERROR "Initialization or global-data source guard is missing: ${_kbe_required}")
	endif()
endforeach()

foreach(_kbe_required IN ITEMS
	"ServerApp::reqKillServer: rejected componentType="
	"ServerApp::onAppActiveTick: rejected componentType="
	"void ServerApp::reqCloseServer(Network::Channel* pChannel, MemoryStream& s)"
	"void ServerApp::queryWatcher(Network::Channel* pChannel, MemoryStream& s)"
	"Security::isBoundComponentSource(componentID, sourceInfos, pChannel)"
)
	string(FIND "${_kbe_serverapp}" "${_kbe_required}" _kbe_required_position)
	if(_kbe_required_position EQUAL -1)
		message(FATAL_ERROR "Server control source guard is missing: ${_kbe_required}")
	endif()
endforeach()

foreach(_kbe_required IN ITEMS
	"Dbmgr::removeEntity: rejected componentID="
	"Dbmgr::entityAutoLoad: rejected dbInterfaceIndex="
	"Dbmgr::queryEntity: rejected componentID="
	"findBoundBaseappSource(pChannel, componentID)"
	"Dbmgr::onEntityOffline: rejected componentID="
	"rejected under-consumed message"
	"bindClientStateForCreatedEntity"
	"Security::isValidPersistentEntityID"
	"Security::isValidDatabaseQueryMode"
	"validateBaseappEntityCreationSource"
	"{}: rejected componentID="
	"\"Cellapp::onCreateCellEntityFromBaseapp\""
	"Cellapp::_onCreateCellEntityFromBaseapp: rejected unavailable space"
	"Cellappmgr::reqCreateCellEntityInNewSpace: rejected unbound BaseApp"
	"Cellappmgr::reqRestoreSpaceInCell: rejected unbound BaseApp"
	"findOrReconnectChannel"
	"Interfaces Channel unavailable after reconnect"
	"reconnect: rejected missing Interfaces address"
	"isExpectedIngressSource(LOGINAPP_TYPE"
	"isExpectedIngressSource(BASEAPP_TYPE"
	"isExpectedIngressSource(INTERFACES_TYPE"
	"Dbmgr::onReqAllocEntityID: rejected componentType="
	"Dbmgr::queryAccount: rejected componentID="
	"Dbmgr::executeRawDatabaseCommand: rejected componentType="
	"Dbmgr::syncEntityStreamTemplate"
	"Dbmgr::syncEntityStreamTemplate: rejected missing account table"
	"isAllowedRawDatabaseSource"
)
	string(FIND "${_kbe_cellapp}\n${_kbe_cellappmgr}\n${_kbe_dbmgr}\n${_kbe_interfaces_handler}"
		"${_kbe_required}" _kbe_required_position)
	if(_kbe_required_position EQUAL -1)
		message(FATAL_ERROR "Database or Cell creation ingress guard is missing: ${_kbe_required}")
	endif()
endforeach()

foreach(_kbe_required IN ITEMS
    "Security::isBoundComponentSource"
    "isExpectedComponentChannel"
    "findComponentChannel(BASEAPP_TYPE"
    "findComponentChannel(CELLAPP_TYPE"
    "Security::isValidComponentMetric"
	"Baseappmgr::onRegisterNewApp: added registered BaseApp"
	"Cellappmgr::onRegisterNewApp: added registered CellApp"
)
    string(FIND "${_kbe_baseapp}\n${_kbe_baseappmgr}\n${_kbe_cellappmgr}"
        "${_kbe_required}" _kbe_required_position)
    if(_kbe_required_position EQUAL -1)
        message(FATAL_ERROR "Component routing guard is missing: ${_kbe_required}")
    endif()
endforeach()

foreach(_kbe_required IN ITEMS
    "onCreateEntityFromDBIDCallback: rejected non-DBMgr source"
    "onExecuteRawDatabaseCommandCB: rejected non-DBMgr source"
    "onQueryAccountCBFromDbmgr: rejected non-DBMgr source"
    "deleteEntityByDBIDCB: rejected non-DBMgr source"
    "lookUpEntityByDBIDCB: rejected non-DBMgr source"
)
    string(FIND "${_kbe_baseapp}\n${_kbe_cellapp}"
        "${_kbe_required}" _kbe_required_position)
    if(_kbe_required_position EQUAL -1)
        message(FATAL_ERROR "Callback source guard is missing: ${_kbe_required}")
    endif()
endforeach()

foreach(_kbe_required IN ITEMS
	"Baseapp::onGetEntityAppFromDbmgr: rejected non-DBMgr source"
	"Cellapp::onGetEntityAppFromDbmgr: rejected non-DBMgr source"
	"onGetEntityAppFromDbmgr: rejected componentType="
)
	string(FIND "${_kbe_baseapp}\n${_kbe_cellapp}"
		"${_kbe_required}" _kbe_required_position)
	if(_kbe_required_position EQUAL -1)
		message(FATAL_ERROR "EntityApp discovery source guard is missing: ${_kbe_required}")
	endif()
endforeach()

# Client account and Cell RPC identities must remain bound to authenticated
# runtime state rather than packet-carried entity IDs. 客户端账户与 Cell RPC
# 身份必须绑定认证后的运行时状态，不能回退为信任封包中的实体 ID。
foreach(_kbe_required IN ITEMS
    "Security::isBoundClientEntity"
    "sourceEntity->baseEntityCall()->componentID() == sourceComponent->cid"
	"Security::isAuthorizedClientCellTarget"
	"Security::isBoundBidirectionalComponentSource"
	"forwardEntityMessageToCellappFromClient: rejected unregistered"
	"forwardEntityMessageToCellappFromClient: rejected unbound"
)
    string(FIND "${_kbe_baseapp}\n${_kbe_cellapp}"
        "${_kbe_required}" _kbe_required_position)
    if(_kbe_required_position EQUAL -1)
        message(FATAL_ERROR "Client request authorization guard is missing: ${_kbe_required}")
    endif()
endforeach()

message(STATUS "Verified component routing and sensitive-log source contracts")
