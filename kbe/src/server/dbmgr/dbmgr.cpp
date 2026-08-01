/*
This source file is part of KBEngine
For the latest info, see http://www.kbengine.org/

Copyright (c) 2008-2018 KBEngine.

KBEngine is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

KBEngine is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.
 
You should have received a copy of the GNU Lesser General Public License
along with KBEngine.  If not, see <http://www.gnu.org/licenses/>.
*/


#include "dbmgr.h"
#include "dbmgr_interface.h"
#include "dbtasks.h"
#include "account_request_guard.h"
#include "server/interfaces_payload_guard.h"
#include "profile.h"
#include "interfaces_handler.h"
#include "sync_app_datas_handler.h"
#include "update_dblog_handler.h"
#include "db_mysql/kbe_table_mysql.h"
#include "network/common.h"
#include "network/tcp_packet.h"
#include "network/udp_packet.h"
#include "network/message_handler.h"
#include "thread/threadpool.h"
#include "server/component_routing_guard.h"
#include "server/components.h"
#include "server/asyncio_helper.h"
#include "server/plugin_runtime.h"
#include "server/telnet_server.h"
#include "db_interface/db_interface.h"
#include "db_mysql/db_interface_mysql.h"
#include "entitydef/scriptdef_module.h"

#include "baseapp/baseapp_interface.h"
#include "cellapp/cellapp_interface.h"
#include "baseappmgr/baseappmgr_interface.h"
#include "cellappmgr/cellappmgr_interface.h"
#include "loginapp/loginapp_interface.h"

namespace KBEngine{

namespace
{
Components::ComponentInfos* findBoundComponentSource(Network::Channel* pChannel,
	COMPONENT_TYPE componentType, COMPONENT_ID componentID)
{
	if (pChannel == NULL || pChannel->isExternal() || pChannel->isDestroyed() ||
		componentID == 0)
	{
		return NULL;
	}

	Components::ComponentInfos* sourceInfos =
		Components::getSingleton().findComponent(componentType, componentID);
	return Security::isBoundComponentSource(componentID, sourceInfos, pChannel) ?
		sourceInfos : NULL;
}

Components::ComponentInfos* findBoundBaseappSource(Network::Channel* pChannel,
	COMPONENT_ID componentID)
{
	return findBoundComponentSource(pChannel, BASEAPP_TYPE, componentID);
}

bool isExpectedIngressSource(COMPONENT_TYPE expectedType, Network::Channel* pChannel,
	const char* operation)
{
	if (Components::getSingleton().isExpectedComponentChannel(expectedType, pChannel))
		return true;

	WARNING_MSG(fmt::format("{}: rejected sourceType={}, addr={}.\n",
		operation, COMPONENT_NAME_EX(expectedType),
		pChannel != NULL ? pChannel->c_str() : "none"));
	return false;
}

InterfacesHandler* findInterfacesHandlerOrWarn(const char* operation)
{
	InterfacesHandler* pHandler = Dbmgr::getSingleton().findBestInterfacesHandler();
	if (pHandler == NULL)
	{
		ERROR_MSG(fmt::format("{}: no initialized Interfaces handler is available.\n",
			operation));
	}

	return pHandler;
}

bool validateRawDatabaseCommandStream(MemoryStream& s)
{
	const size_t payloadPosition = s.rpos();
	const size_t fixedHeaderSize = sizeof(ENTITY_ID) + sizeof(uint16) +
		sizeof(COMPONENT_ID) + sizeof(COMPONENT_TYPE) + sizeof(CALLBACK_ID) + sizeof(ArraySize);
	if (s.length() < fixedHeaderSize)
		return false;

	ENTITY_ID entityID = 0;
	uint16 dbInterfaceIndex = 0;
	COMPONENT_ID componentID = 0;
	COMPONENT_TYPE componentType = UNKNOWN_COMPONENT_TYPE;
	CALLBACK_ID callbackID = 0;
	ArraySize commandSize = 0;
	s >> entityID >> dbInterfaceIndex >> componentID >> componentType >> callbackID >> commandSize;
	const bool valid = static_cast<size_t>(commandSize) <= s.length();
	s.rpos(payloadPosition);
	return valid;
}

bool hasRemainingBytes(MemoryStream& s, size_t required)
{
	return s.length() >= required;
}

bool isAllowedRawDatabaseSource(Network::Channel* pChannel,
	COMPONENT_TYPE componentType, COMPONENT_ID componentID)
{
	if (pChannel == NULL)
		return componentType == DBMGR_TYPE && componentID == g_componentID;

	if (componentType != BASEAPP_TYPE && componentType != CELLAPP_TYPE &&
		componentType != INTERFACES_TYPE)
	{
		return false;
	}

	return findBoundComponentSource(pChannel, componentType, componentID) != NULL;
}

bool validateEntityScriptType(const char* operation, ENTITY_SCRIPT_UID scriptType)
{
	// 网络入口使用无日志范围检查，避免无效 UID 先触发 EntityDef 的错误日志。
	// Network ingress uses a silent range check so invalid UIDs do not first trigger EntityDef error logging.
	const EntityDef::SCRIPT_MODULES& modules = EntityDef::getScriptModules();
	if (scriptType > 0 && scriptType <= modules.size() && modules[scriptType - 1] != NULL)
		return true;

	WARNING_MSG(fmt::format("{}: rejected entity script type ID={}.\n",
		operation, scriptType));
	return false;
}

bool validateEntityScriptType(const char* operation, const std::string& scriptType)
{
	const EntityDef::SCRIPT_MODULES& modules = EntityDef::getScriptModules();
	for (EntityDef::SCRIPT_MODULES::const_iterator iter = modules.begin();
		iter != modules.end(); ++iter)
	{
		if (*iter != NULL && scriptType == (*iter)->getName())
			return true;
	}

	// 名称来自网络载荷，仅记录长度以防止控制字符或超长值污染日志。
	// The name comes from the network payload; log only its size to prevent control-character or oversized log injection.
	WARNING_MSG(fmt::format("{}: rejected entity script type, nameSize={}.\n",
		operation, scriptType.size()));
	return false;
}

bool validateEntityAutoLoadRange(const char* operation, ENTITY_ID start, ENTITY_ID end)
{
	// BaseApp 按固定 32 条分页；限制范围可避免负数转换、错误 LIMIT 和超大数据库扫描。
	// BaseApp pages in fixed batches of 32; bound the range to prevent signed conversion, invalid LIMIT, and huge scans.
	const ENTITY_ID maxBatchSize = 32;
	if (start >= 0 && end > start && end - start <= maxBatchSize)
		return true;

	WARNING_MSG(fmt::format("{}: rejected entity auto-load range, start={}, end={}.\n",
		operation, start, end));
	return false;
}

bool resolveDbInterfaceName(const char* operation, uint16 dbInterfaceIndex, std::string& dbInterfaceName)
{
	const char* configuredName = g_kbeSrvConfig.dbInterfaceIndex2dbInterfaceName(dbInterfaceIndex);
	if (configuredName == NULL || configuredName[0] == '\0')
	{
		WARNING_MSG(fmt::format("{}: rejected dbInterfaceIndex={}.\n",
			operation, dbInterfaceIndex));
		return false;
	}

	dbInterfaceName.assign(configuredName);
	return true;
}
}

ServerConfig g_serverConfig;
KBE_SINGLETON_INIT(Dbmgr);

//-------------------------------------------------------------------------------------
Dbmgr::Dbmgr(Network::EventDispatcher& dispatcher, 
			 Network::NetworkInterface& ninterface, 
			 COMPONENT_TYPE componentType,
			 COMPONENT_ID componentID):
	PythonApp(dispatcher, ninterface, componentType, componentID),
	loopCheckTimerHandle_(),
	mainProcessTimer_(),
	idServer_(1, 1024),
	pGlobalData_(NULL),
	pBaseAppData_(NULL),
	pCellAppData_(NULL),
	bufferedDBTasksMaps_(),
	numWrittenEntity_(0),
	numRemovedEntity_(0),
	numQueryEntity_(0),
	numExecuteRawDatabaseCommand_(0),
	numCreatedAccount_(0),
	pInterfacesHandlers_(),
	pSyncAppDatasHandler_(NULL),
	pUpdateDBServerLogHandler_(NULL),
	pTelnetServer_(NULL),
	loseBaseappts_()
{
	KBEngine::Network::MessageHandlers::pMainMessageHandlers = &DbmgrInterface::messageHandlers;
}

//-------------------------------------------------------------------------------------
Dbmgr::~Dbmgr()
{
	loopCheckTimerHandle_.cancel();
	mainProcessTimer_.cancel();
	KBEngine::sleep(300);

	for (std::vector<InterfacesHandler*>::iterator iter = pInterfacesHandlers_.begin(); iter != pInterfacesHandlers_.end(); ++iter)
	{
		SAFE_RELEASE((*iter));
	}
}

//-------------------------------------------------------------------------------------
ShutdownHandler::CAN_SHUTDOWN_STATE Dbmgr::canShutdown()
{
	if (getEntryScript().get() && PyObject_HasAttrString(getEntryScript().get(), "onReadyForShutDown") > 0)
	{
		// 所有脚本都加载完毕
		PyObject* pyResult = PyObject_CallMethod(getEntryScript().get(),
			const_cast<char*>("onReadyForShutDown"),
			const_cast<char*>(""));

		if (pyResult != NULL)
		{
			bool isReady = (pyResult == Py_True);
			Py_DECREF(pyResult);

			if (!isReady)
				return ShutdownHandler::CAN_SHUTDOWN_STATE_USER_FALSE;
		}
		else
		{
			SCRIPT_ERROR_CHECK();
			return ShutdownHandler::CAN_SHUTDOWN_STATE_USER_FALSE;
		}
	}

	KBEUnordered_map<std::string, Buffered_DBTasks>::iterator bditer = bufferedDBTasksMaps_.begin();
	for (; bditer != bufferedDBTasksMaps_.end(); ++bditer)
	{
		if (bditer->second.size() > 0)
		{
			thread::ThreadPool* pThreadPool = DBUtil::pThreadPool(bditer->first);
			KBE_ASSERT(pThreadPool);

			INFO_MSG(fmt::format("Dbmgr::canShutdown(): Wait for the task to complete, dbInterface={}, tasks{}=[{}], threads={}/{}, threadpoolDestroyed={}!\n",
				bditer->first, bditer->second.size(), bditer->second.getTasksinfos(), (pThreadPool->currentThreadCount() - pThreadPool->currentFreeThreadCount()),
				pThreadPool->currentThreadCount(), pThreadPool->isDestroyed()));

			return ShutdownHandler::CAN_SHUTDOWN_STATE_FALSE;
		}
	}

	Components::COMPONENTS& cellapp_components = Components::getSingleton().getComponents(CELLAPP_TYPE);
	if (cellapp_components.size() > 0)
	{
		std::string s;
		for (size_t i = 0; i<cellapp_components.size(); ++i)
		{
			s += fmt::format("{}, ", cellapp_components[i].cid);
		}

		INFO_MSG(fmt::format("Dbmgr::canShutdown(): Waiting for cellapp[{}] destruction!\n",
			s));

		return ShutdownHandler::CAN_SHUTDOWN_STATE_FALSE;
	}

	Components::COMPONENTS& baseapp_components = Components::getSingleton().getComponents(BASEAPP_TYPE);
	if (baseapp_components.size() > 0)
	{
		std::string s;
		for (size_t i = 0; i<baseapp_components.size(); ++i)
		{
			s += fmt::format("{}, ", baseapp_components[i].cid);
		}

		INFO_MSG(fmt::format("Dbmgr::canShutdown(): Waiting for baseapp[{}] destruction!\n",
			s));

		return ShutdownHandler::CAN_SHUTDOWN_STATE_FALSE;
	}

	return ShutdownHandler::CAN_SHUTDOWN_STATE_TRUE;
}

//-------------------------------------------------------------------------------------	
void Dbmgr::onShutdownBegin()
{
	PythonApp::onShutdownBegin();

	// 通知脚本
	SCOPED_PROFILE(SCRIPTCALL_PROFILE);
	SCRIPT_OBJECT_CALL_ARGS0(getEntryScript().get(), const_cast<char*>("onDBMgrShutDown"), false);
}

//-------------------------------------------------------------------------------------	
void Dbmgr::onShutdownEnd()
{
	PythonApp::onShutdownEnd();
}

//-------------------------------------------------------------------------------------
bool Dbmgr::initializeWatcher()
{
	WATCH_OBJECT("numWrittenEntity", numWrittenEntity_);
	WATCH_OBJECT("numRemovedEntity", numRemovedEntity_);
	WATCH_OBJECT("numQueryEntity", numQueryEntity_);
	WATCH_OBJECT("numExecuteRawDatabaseCommand", numExecuteRawDatabaseCommand_);
	WATCH_OBJECT("numCreatedAccount", numCreatedAccount_);

	KBEUnordered_map<std::string, Buffered_DBTasks>::iterator bditer = bufferedDBTasksMaps_.begin();
	for (; bditer != bufferedDBTasksMaps_.end(); ++bditer)
	{
		WATCH_OBJECT(fmt::format("DBThreadPool/{}/dbid_tasksSize", bditer->first).c_str(), &bditer->second, &Buffered_DBTasks::dbid_tasksSize);
		WATCH_OBJECT(fmt::format("DBThreadPool/{}/entityid_tasksSize", bditer->first).c_str(), &bditer->second, &Buffered_DBTasks::entityid_tasksSize);
		WATCH_OBJECT(fmt::format("DBThreadPool/{}/printBuffered_dbid", bditer->first).c_str(), &bditer->second, &Buffered_DBTasks::printBuffered_dbid);
		WATCH_OBJECT(fmt::format("DBThreadPool/{}/printBuffered_entityID", bditer->first).c_str(), &bditer->second, &Buffered_DBTasks::printBuffered_entityID);
	}

	return ServerApp::initializeWatcher() && DBUtil::initializeWatcher();
}

//-------------------------------------------------------------------------------------
bool Dbmgr::run()
{
	return PythonApp::run();
}

//-------------------------------------------------------------------------------------
void Dbmgr::handleTimeout(TimerHandle handle, void * arg)
{
	PythonApp::handleTimeout(handle, arg);

	switch (reinterpret_cast<uintptr>(arg))
	{
		case TIMEOUT_TICK:
			this->handleMainTick();
			break;
		case TIMEOUT_CHECK_STATUS:
			this->handleCheckStatusTick();
			break;
		default:
			break;
	}
}

//-------------------------------------------------------------------------------------
void Dbmgr::handleMainTick()
{
	AUTO_SCOPED_PROFILE("mainTick");
	
	// time_t t = ::time(NULL);
	// static int kbeTime = 0;
	// DEBUG_MSG(fmt::format("Dbmgr::handleGameTick[{}]:{}\n", t, ++kbeTime));
	
	threadPool_.onMainThreadTick();
	// DBMgr 本地 Python 数据库回调在组件空闲时也必须按时释放。
	// Local Python database callbacks must expire even while no new callback traffic arrives.
	pyCallbackMgr_.tick();
	DBUtil::handleMainTick();
	networkInterface().processChannels(&DbmgrInterface::messageHandlers);
}

//-------------------------------------------------------------------------------------
void Dbmgr::handleCheckStatusTick()
{
	// 检查丢失的组件进程，如果在一段时间之内仍然无法发现，需要清理数据库中entitylog
	if (loseBaseappts_.size() > 0)
	{
		std::map<COMPONENT_ID, uint64>::iterator iter = loseBaseappts_.begin();
		for (; iter != loseBaseappts_.end();)
		{
			if (timestamp() > iter->second)
			{
				Components::ComponentInfos* cinfo = Components::getSingleton().findComponent(iter->first);
				if (!cinfo)
				{
					ENGINE_COMPONENT_INFO& dbcfg = g_kbeSrvConfig.getDBMgr();
					std::vector<DBInterfaceInfo>::iterator dbinfo_iter = dbcfg.dbInterfaceInfos.begin();
					for (; dbinfo_iter != dbcfg.dbInterfaceInfos.end(); ++dbinfo_iter)
					{
						std::string dbInterfaceName = dbinfo_iter->name;

						DBUtil::pThreadPool(dbInterfaceName)->
							addTask(new DBTaskEraseBaseappEntityLog(iter->first));
					}
				}

				loseBaseappts_.erase(iter++);
			}
			else
			{
				++iter;
			}
		}
	}
}

//-------------------------------------------------------------------------------------
bool Dbmgr::initializeBegin()
{
	idServer_.set_range_step(g_kbeSrvConfig.getDBMgr().ids_increasing_range);
	return true;
}

//-------------------------------------------------------------------------------------
bool Dbmgr::inInitialize()
{
	// 初始化所有扩展模块
	// assets/scripts/
	if (!PythonApp::inInitialize())
		return false;

	std::vector<PyTypeObject*>	scriptBaseTypes;
	if(!EntityDef::initialize(scriptBaseTypes, componentType_)){
		return false;
	}

	return true;
}

//-------------------------------------------------------------------------------------
bool Dbmgr::initializeEnd()
{
	if (!PythonApp::initializeEnd())
		return false;

	// 添加一个timer， 每秒检查一些状态
	loopCheckTimerHandle_ = this->dispatcher().addTimer(1000000, this,
							reinterpret_cast<void *>(TIMEOUT_CHECK_STATUS));

	mainProcessTimer_ = this->dispatcher().addTimer(1000000 / 50, this,
							reinterpret_cast<void *>(TIMEOUT_TICK));

	// 添加globalData, baseAppData, cellAppData支持
	pGlobalData_ = new GlobalDataServer(GlobalDataServer::GLOBAL_DATA);
	pBaseAppData_ = new GlobalDataServer(GlobalDataServer::BASEAPP_DATA);
	pCellAppData_ = new GlobalDataServer(GlobalDataServer::CELLAPP_DATA);
	pGlobalData_->addConcernComponentType(CELLAPP_TYPE);
	pGlobalData_->addConcernComponentType(BASEAPP_TYPE);
	pBaseAppData_->addConcernComponentType(BASEAPP_TYPE);
	pCellAppData_->addConcernComponentType(CELLAPP_TYPE);

	INFO_MSG(fmt::format("Dbmgr::initializeEnd: digest({})\n", 
		EntityDef::md5().getDigestStr()));
	
	pTelnetServer_ = new TelnetServer(&this->dispatcher(), &this->networkInterface());
	pTelnetServer_->pScript(&this->getScript());

	bool ret = pTelnetServer_->start(g_kbeSrvConfig.getDBMgr().telnet_passwd,
		g_kbeSrvConfig.getDBMgr().telnet_deflayer,
		g_kbeSrvConfig.getDBMgr().telnet_port);

	Components::getSingleton().extraData4(pTelnetServer_->port());
	
	if (!ret || !initInterfacesHandler() || !initDB())
		return false;

	// ready 回调可能立即执行数据库操作，因此只能在所有数据库接口成功初始化后公开服务状态。
	// The ready callback may issue database operations immediately, so service readiness is exposed only after every database interface initializes successfully.
	{
		SCOPED_PROFILE(SCRIPTCALL_PROFILE);

		PyObject* pyResult = PyObject_CallMethod(getEntryScript().get(),
			const_cast<char*>("onDBMgrReady"), const_cast<char*>(""));

		if (pyResult)
		{
			// 与其他应用 ready 回调一致，允许脚本返回 coroutine 交给主循环调度。
			// Match the other application-ready callbacks by submitting a returned coroutine to the main-loop scheduler.
			AsyncioHelper::submitCoroutine(pyResult);
			Py_DECREF(pyResult);
		}
		else
		{
			SCRIPT_ERROR_CHECK();
		}
	}

	return PluginRuntime::instance().onComponentReady(true);
}

//-------------------------------------------------------------------------------------
bool Dbmgr::installPyModules()
{
	return PythonApp::installPyModules();
}

//-------------------------------------------------------------------------------------
bool Dbmgr::uninstallPyModules()
{
	return PythonApp::uninstallPyModules();
}

//-------------------------------------------------------------------------------------		
void Dbmgr::onInstallPyModules()
{
	PyObject * module = getScript().getModule();

	for (int i = 0; i < SERVER_ERR_MAX; i++)
	{
		if(PyModule_AddIntConstant(module, SERVER_ERR_STR[i], i))
		{
			ERROR_MSG( fmt::format("Dbmgr::onInstallPyModules: Unable to set KBEngine.{}.\n", SERVER_ERR_STR[i]));
		}
	}

	APPEND_SCRIPT_MODULE_METHOD(module,		executeRawDatabaseCommand,		__py_executeRawDatabaseCommand,		METH_VARARGS,	0);
}

//-------------------------------------------------------------------------------------		
bool Dbmgr::initInterfacesHandler()
{
	std::vector< Network::Address > addresses = g_kbeSrvConfig.interfacesAddrs();
	std::string type = addresses.size() == 0 ? "dbmgr" : "interfaces";

	if (type == "dbmgr")
	{
		InterfacesHandler* pInterfacesHandler = InterfacesHandlerFactory::create(type);

		INFO_MSG(fmt::format("Dbmgr::initInterfacesHandler: interfaces addr({}), accountType:({}), chargeType:({}).\n",
			Network::Address::NONE.c_str(),
			type,
			type));

		if (!pInterfacesHandler->initialize())
			return false;

		pInterfacesHandlers_.push_back(pInterfacesHandler);
	}
	else
	{
		std::vector< Network::Address >::iterator iter = addresses.begin();
		for (; iter != addresses.end(); ++iter)
		{
			InterfacesHandler* pInterfacesHandler = InterfacesHandlerFactory::create(type);

			const Network::Address& addr = (*iter);

			INFO_MSG(fmt::format("Dbmgr::initInterfacesHandler: interfaces addr({}), accountType:({}), chargeType:({}).\n",
				addr.c_str(),
				type,
				type));

			((InterfacesHandler_Interfaces*)pInterfacesHandler)->setAddr(addr);

			if (!pInterfacesHandler->initialize())
				return false;

			pInterfacesHandlers_.push_back(pInterfacesHandler);
		}
	}

	return pInterfacesHandlers_.size() > 0;
}

//-------------------------------------------------------------------------------------		
bool Dbmgr::initDB()
{
	ScriptDefModule* pModule = EntityDef::findScriptModule(DBUtil::accountScriptName());
	if(pModule == NULL)
	{
		ERROR_MSG(fmt::format("Dbmgr::initDB(): not found account script[{}]!\n", 
			DBUtil::accountScriptName()));

		return false;
	}

	ENGINE_COMPONENT_INFO& dbcfg = g_kbeSrvConfig.getDBMgr();
	if (dbcfg.dbInterfaceInfos.size() == 0)
	{
		ERROR_MSG(fmt::format("DBUtil::initialize: not found dbInterface! (kbengine[_defs].xml->dbmgr->databaseInterfaces)\n"));
		return false;
	}

	if (!DBUtil::initialize())
	{
		ERROR_MSG("Dbmgr::initDB(): can't initialize dbInterface!\n");
		return false;
	}

	bool hasDefaultInterface = false;

	std::vector<DBInterfaceInfo>::iterator dbinfo_iter = dbcfg.dbInterfaceInfos.begin();
	for (; dbinfo_iter != dbcfg.dbInterfaceInfos.end(); ++dbinfo_iter)
	{
		Buffered_DBTasks buffered_DBTasks;
		bufferedDBTasksMaps_.insert(std::make_pair((*dbinfo_iter).name, buffered_DBTasks));
		BUFFERED_DBTASKS_MAP::iterator buffered_DBTasks_iter = bufferedDBTasksMaps_.find((*dbinfo_iter).name);
		buffered_DBTasks_iter->second.dbInterfaceName((*dbinfo_iter).name);
	}

	for (dbinfo_iter = dbcfg.dbInterfaceInfos.begin(); dbinfo_iter != dbcfg.dbInterfaceInfos.end(); ++dbinfo_iter)
	{
		DBInterface* pDBInterface = DBUtil::createInterface((*dbinfo_iter).name);
		if(pDBInterface == NULL)
		{
			ERROR_MSG("Dbmgr::initDB(): can't create dbInterface!\n");
			return false;
		}

		bool ret = DBUtil::initInterface(pDBInterface);
		pDBInterface->detach();
		SAFE_RELEASE(pDBInterface);

		if(!ret)
			return false;

		if (std::string("default") == (*dbinfo_iter).name)
			hasDefaultInterface = true;
	}

	if (!hasDefaultInterface)
	{
		ERROR_MSG("Dbmgr::initDB(): \"default\" dbInterface was not found! (kbengine[_defs].xml->dbmgr->databaseInterfaces)\n");
		return false;
	}

	if(pUpdateDBServerLogHandler_ == NULL)
		pUpdateDBServerLogHandler_ = new UpdateDBServerLogHandler();

	return true;
}

//-------------------------------------------------------------------------------------
void Dbmgr::finalise()
{
	SAFE_RELEASE(pUpdateDBServerLogHandler_);
	
	SAFE_RELEASE(pGlobalData_);
	SAFE_RELEASE(pBaseAppData_);
	SAFE_RELEASE(pCellAppData_);

	if (pTelnetServer_)
	{
		pTelnetServer_->stop();
		SAFE_RELEASE(pTelnetServer_);
	}

	DBUtil::finalise();
	PythonApp::finalise();
}

//-------------------------------------------------------------------------------------
InterfacesHandler* Dbmgr::findBestInterfacesHandler()
{
	if (pInterfacesHandlers_.size() == 0)
		return NULL;

	static size_t i = 0;
	
	return pInterfacesHandlers_[i++ % pInterfacesHandlers_.size()];
}

//-------------------------------------------------------------------------------------
void Dbmgr::onReqAllocEntityID(Network::Channel* pChannel, COMPONENT_ORDER componentType, COMPONENT_ID componentID)
{
	KBEngine::COMPONENT_TYPE ct = static_cast<KBEngine::COMPONENT_TYPE>(componentType);
	if ((ct != BASEAPP_TYPE && ct != CELLAPP_TYPE) ||
		findBoundComponentSource(pChannel, ct, componentID) == NULL)
	{
		WARNING_MSG(fmt::format("Dbmgr::onReqAllocEntityID: rejected componentType={}, componentID={}, addr={}.\n",
			componentType, componentID, pChannel != NULL ? pChannel->c_str() : "none"));
		return;
	}

	// 获取一个id段 并传输给IDClient
	std::pair<ENTITY_ID, ENTITY_ID> idRange = idServer_.allocRange();
	Network::Bundle* pBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);

	if(ct == BASEAPP_TYPE)
		(*pBundle).newMessage(BaseappInterface::onReqAllocEntityID);
	else	
		(*pBundle).newMessage(CellappInterface::onReqAllocEntityID);

	(*pBundle) << idRange.first;
	(*pBundle) << idRange.second;
	pChannel->send(pBundle);
}

//-------------------------------------------------------------------------------------
void Dbmgr::onRegisterNewApp(Network::Channel* pChannel, int32 uid, std::string& username, 
						COMPONENT_TYPE componentType, COMPONENT_ID componentID, COMPONENT_ORDER globalorderID, COMPONENT_ORDER grouporderID,
						uint32 intaddr, uint16 intport, uint32 extaddr, uint16 extport, std::string& extaddrEx)
{
	if(pChannel->isExternal())
		return;

	if (!isGameServerComponentType(componentType) ||
		!ServerApp::registerNewApp(pChannel, uid, username, componentType, componentID,
			globalorderID, grouporderID, intaddr, intport, extaddr, extport, extaddrEx))
	{
		WARNING_MSG(fmt::format("Dbmgr::onRegisterNewApp: rejected registration componentType={}, componentID={}, uid={}, addr={}.\n",
			componentType, componentID, uid, pChannel != NULL ? pChannel->c_str() : "none"));
		return;
	}

	KBEngine::COMPONENT_TYPE tcomponentType = (KBEngine::COMPONENT_TYPE)componentType;
	
	COMPONENT_ORDER startGroupOrder = 1;
	COMPONENT_ORDER startGlobalOrder = Components::getSingleton().getGlobalOrderLog()[getUserUID()];

	if(grouporderID > 0)
		startGroupOrder = grouporderID;

	if(globalorderID > 0)
		startGlobalOrder = globalorderID;

	if(pSyncAppDatasHandler_ == NULL)
		pSyncAppDatasHandler_ = new SyncAppDatasHandler(this->networkInterface());

	// 下一步:
	// 如果是连接到dbmgr则需要等待接收app初始信息
	// 例如：初始会分配entityID段以及这个app启动的顺序信息（是否第一个baseapp启动）
	if(tcomponentType == BASEAPP_TYPE || 
		tcomponentType == CELLAPP_TYPE || 
		tcomponentType == LOGINAPP_TYPE)
	{
		switch(tcomponentType)
		{
		case BASEAPP_TYPE:
			{
				if(grouporderID <= 0)
					startGroupOrder = Components::getSingleton().getBaseappGroupOrderLog()[getUserUID()];
			}
			break;
		case CELLAPP_TYPE:
			{
				if(grouporderID <= 0)
					startGroupOrder = Components::getSingleton().getCellappGroupOrderLog()[getUserUID()];
			}
			break;
		case LOGINAPP_TYPE:
			if(grouporderID <= 0)
				startGroupOrder = Components::getSingleton().getLoginappGroupOrderLog()[getUserUID()];

			break;
		default:
			break;
		}
	}

	pSyncAppDatasHandler_->pushApp(componentID, startGroupOrder, startGlobalOrder);

	// 如果是baseapp或者cellapp则将自己注册到所有其他baseapp和cellapp
	if(tcomponentType == BASEAPP_TYPE || 
		tcomponentType == CELLAPP_TYPE)
	{
		KBEngine::COMPONENT_TYPE broadcastCpTypes[2] = {BASEAPP_TYPE, CELLAPP_TYPE};
		for(int idx = 0; idx < 2; ++idx)
		{
			Components::COMPONENTS& cts = Components::getSingleton().getComponents(broadcastCpTypes[idx]);
			Components::COMPONENTS::iterator fiter = cts.begin();
			for(; fiter != cts.end(); ++fiter)
			{
				if((*fiter).cid == componentID)
					continue;

				// 组件记录可能处于连接建立或断连清理窗口，注册广播不能因此终止 DBMgr。
				// Component records can outlive a usable channel during connect or cleanup; registration broadcast must remain available.
				if ((*fiter).pChannel == NULL || (*fiter).pChannel->isDestroyed())
				{
					WARNING_MSG(fmt::format(
						"Dbmgr::onRegisterNewApp: skipped unavailable broadcast target, componentType={}, componentID={}.\n",
						broadcastCpTypes[idx], (*fiter).cid));
					continue;
				}

				Network::Bundle* pBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
				ENTITTAPP_COMMON_NETWORK_MESSAGE(broadcastCpTypes[idx], (*pBundle), onGetEntityAppFromDbmgr);
				
				if(tcomponentType == BASEAPP_TYPE)
				{
					BaseappInterface::onGetEntityAppFromDbmgrArgs11::staticAddToBundle((*pBundle), 
						uid, username, componentType, componentID, startGlobalOrder, startGroupOrder,
							intaddr, intport, extaddr, extport, g_kbeSrvConfig.getConfig().externalAddress);
				}
				else
				{
					CellappInterface::onGetEntityAppFromDbmgrArgs11::staticAddToBundle((*pBundle), 
						uid, username, componentType, componentID, startGlobalOrder, startGroupOrder,
							intaddr, intport, extaddr, extport, g_kbeSrvConfig.getConfig().externalAddress);
				}
				
				(*fiter).pChannel->send(pBundle);
			}
		}
	}
}

//-------------------------------------------------------------------------------------
void Dbmgr::onGlobalDataClientLogon(Network::Channel* pChannel, COMPONENT_TYPE componentType)
{
	if ((componentType != BASEAPP_TYPE && componentType != CELLAPP_TYPE) ||
		!Components::getSingleton().isExpectedComponentChannel(componentType, pChannel))
	{
		WARNING_MSG(fmt::format("Dbmgr::onGlobalDataClientLogon: rejected componentType={}, addr={}.\n",
			componentType, pChannel->c_str()));
		return;
	}

	if(BASEAPP_TYPE == componentType)
	{
		pBaseAppData_->onGlobalDataClientLogon(pChannel, componentType);
		pGlobalData_->onGlobalDataClientLogon(pChannel, componentType);
	}
	else if(CELLAPP_TYPE == componentType)
	{
		pGlobalData_->onGlobalDataClientLogon(pChannel, componentType);
		pCellAppData_->onGlobalDataClientLogon(pChannel, componentType);
	}
	else
	{
		ERROR_MSG(fmt::format("Dbmgr::onGlobalDataClientLogon: nonsupport {}!\n",
			COMPONENT_NAME_EX(componentType)));
	}
}

//-------------------------------------------------------------------------------------
void Dbmgr::onBroadcastGlobalDataChanged(Network::Channel* pChannel, KBEngine::MemoryStream& s)
{
	uint8 dataType;
	std::string key, value;
	bool isDelete;
	COMPONENT_TYPE componentType;
	
	s >> dataType;
	s >> isDelete;

	s.readBlob(key);

	if(!isDelete)
	{
		s.readBlob(value);
	}

	s >> componentType;

	if ((componentType != BASEAPP_TYPE && componentType != CELLAPP_TYPE) ||
		!Components::getSingleton().isExpectedComponentChannel(componentType, pChannel))
	{
		WARNING_MSG(fmt::format("Dbmgr::onBroadcastGlobalDataChanged: rejected componentType={}, dataType={}, addr={}.\n",
			componentType, dataType, pChannel->c_str()));
		return;
	}

	switch(dataType)
	{
	case GlobalDataServer::GLOBAL_DATA:
		if(isDelete)
			pGlobalData_->del(pChannel, componentType, key);
		else
			pGlobalData_->write(pChannel, componentType, key, value);
		break;
	case GlobalDataServer::BASEAPP_DATA:
		if(isDelete)
			pBaseAppData_->del(pChannel, componentType, key);
		else
			pBaseAppData_->write(pChannel, componentType, key, value);
		break;
	case GlobalDataServer::CELLAPP_DATA:
		if(isDelete)
			pCellAppData_->del(pChannel, componentType, key);
		else
			pCellAppData_->write(pChannel, componentType, key, value);
		break;
	default:
		WARNING_MSG(fmt::format("Dbmgr::onBroadcastGlobalDataChanged: rejected dataType={}, addr={}.\n",
			dataType, pChannel->c_str()));
		break;
	};
}

//-------------------------------------------------------------------------------------
void Dbmgr::reqCreateAccount(Network::Channel* pChannel, KBEngine::MemoryStream& s)
{
	if (!isExpectedIngressSource(LOGINAPP_TYPE, pChannel, "Dbmgr::reqCreateAccount"))
	{
		s.done();
		return;
	}

	if (!AccountRequestGuard::validateCreateAccountStream(s))
	{
		WARNING_MSG(fmt::format("Dbmgr::reqCreateAccount: rejected malformed or oversized payload, remaining={}.\n",
			s.length()));
		s.done();
		return;
	}

	std::string registerName, password, datas;
	uint8 uatype = 0;

	s >> registerName >> password >> uatype;
	s.readBlob(datas);

	if (!AccountRequestGuard::isValidAccountName(registerName) ||
		!AccountRequestGuard::isValidPassword(password) ||
		!AccountRequestGuard::isValidAccountData(datas) ||
		!AccountRequestGuard::isValidAccountType(uatype))
	{
		WARNING_MSG(fmt::format("Dbmgr::reqCreateAccount: rejected fields, registerNameSize={}, passwordSize={}, datasSize={}, accountType={}.\n",
			registerName.size(), password.size(), datas.size(), uatype));
		return;
	}

	InterfacesHandler* pHandler = findInterfacesHandlerOrWarn("Dbmgr::reqCreateAccount");
	if (pHandler != NULL &&
		pHandler->createAccount(pChannel, registerName, password, datas, ACCOUNT_TYPE(uatype)))
	{
		numCreatedAccount_++;
	}
}

//-------------------------------------------------------------------------------------
void Dbmgr::onCreateAccountCBFromInterfaces(Network::Channel* pChannel, KBEngine::MemoryStream& s)
{
	if (!isExpectedIngressSource(INTERFACES_TYPE, pChannel,
		"Dbmgr::onCreateAccountCBFromInterfaces"))
	{
		s.done();
		return;
	}
	if (!AccountRequestGuard::validateInterfacesCallbackStream(s))
	{
		WARNING_MSG(fmt::format("Dbmgr::onCreateAccountCBFromInterfaces: rejected malformed or oversized payload, remaining={}.\n",
			s.length()));
		s.done();
		return;
	}

	InterfacesHandler* pHandler =
		findInterfacesHandlerOrWarn("Dbmgr::onCreateAccountCBFromInterfaces");
	if (pHandler != NULL)
		pHandler->onCreateAccountCB(pChannel, s);
	else
		s.done();
}

//-------------------------------------------------------------------------------------
void Dbmgr::onAccountLogin(Network::Channel* pChannel, KBEngine::MemoryStream& s)
{
	if (!isExpectedIngressSource(LOGINAPP_TYPE, pChannel, "Dbmgr::onAccountLogin"))
	{
		s.done();
		return;
	}

	if (!AccountRequestGuard::validateLoginStream(s))
	{
		WARNING_MSG(fmt::format("Dbmgr::onAccountLogin: rejected malformed or oversized payload, remaining={}.\n",
			s.length()));
		s.done();
		return;
	}

	std::string loginName, password, datas;
	s >> loginName >> password;
	s.readBlob(datas);

	if (!AccountRequestGuard::isValidAccountName(loginName) ||
		!AccountRequestGuard::isValidPassword(password) ||
		!AccountRequestGuard::isValidAccountData(datas))
	{
		WARNING_MSG(fmt::format("Dbmgr::onAccountLogin: rejected fields, loginNameSize={}, passwordSize={}, datasSize={}.\n",
			loginName.size(), password.size(), datas.size()));
		return;
	}

	InterfacesHandler* pHandler = findInterfacesHandlerOrWarn("Dbmgr::onAccountLogin");
	if (pHandler != NULL)
		pHandler->loginAccount(pChannel, loginName, password, datas);
}

//-------------------------------------------------------------------------------------
void Dbmgr::onLoginAccountCBBFromInterfaces(Network::Channel* pChannel, KBEngine::MemoryStream& s)
{
	if (!isExpectedIngressSource(INTERFACES_TYPE, pChannel,
		"Dbmgr::onLoginAccountCBBFromInterfaces"))
	{
		s.done();
		return;
	}
	if (!AccountRequestGuard::validateInterfacesCallbackStream(s))
	{
		WARNING_MSG(fmt::format("Dbmgr::onLoginAccountCBBFromInterfaces: rejected malformed or oversized payload, remaining={}.\n",
			s.length()));
		s.done();
		return;
	}

	InterfacesHandler* pHandler =
		findInterfacesHandlerOrWarn("Dbmgr::onLoginAccountCBBFromInterfaces");
	if (pHandler != NULL)
		pHandler->onLoginAccountCB(pChannel, s);
	else
		s.done();
}

//-------------------------------------------------------------------------------------
void Dbmgr::queryAccount(Network::Channel* pChannel,
						 std::string& accountName, 
						 std::string& password,
						 bool needCheckPassword,
						 COMPONENT_ID componentID,
						 ENTITY_ID entityID,
						 DBID entityDBID, 
						 uint32 ip, 
						 uint16 port)
{
	if (findBoundBaseappSource(pChannel, componentID) == NULL || entityID <= 0)
	{
		WARNING_MSG(fmt::format("Dbmgr::queryAccount: rejected componentID={}, entityID={}, addr={}.\n",
			componentID, entityID, pChannel != NULL ? pChannel->c_str() : "none"));
		return;
	}

	if (!AccountRequestGuard::isValidAccountName(accountName) ||
		!AccountRequestGuard::isValidPassword(password))
	{
		WARNING_MSG(fmt::format("Dbmgr::queryAccount: rejected account fields, accountNameSize={}, passwordSize={}.\n",
			accountName.size(), password.size()));
		return;
	}

	Buffered_DBTasks* pBuffered_DBTasks = 
		findBufferedDBTask(Dbmgr::getSingleton().selectAccountDBInterfaceName(accountName));

	if (!pBuffered_DBTasks)
	{
		ERROR_MSG(fmt::format("Dbmgr::queryAccount: not found dbInterface({})!\n", 
			Dbmgr::getSingleton().selectAccountDBInterfaceName(accountName)));
		return;
	}

	pBuffered_DBTasks->addTask(new DBTaskQueryAccount(pChannel->addr(), accountName, password, needCheckPassword,
		componentID, entityID, entityDBID, ip, port));

	numQueryEntity_++;
}

//-------------------------------------------------------------------------------------
void Dbmgr::onAccountOnline(Network::Channel* pChannel, 
							std::string& accountName, 
							COMPONENT_ID componentID, 
							ENTITY_ID entityID)
{
	// bufferedDBTasks_.addTask(new DBTaskAccountOnline(pChannel->addr(), 
	//	accountName, componentID, entityID));
}

//-------------------------------------------------------------------------------------
void Dbmgr::onEntityOffline(Network::Channel* pChannel, DBID dbid, ENTITY_SCRIPT_UID sid,
	COMPONENT_ID componentID, uint16 dbInterfaceIndex)
{
	if (findBoundBaseappSource(pChannel, componentID) == NULL)
	{
		WARNING_MSG(fmt::format("Dbmgr::onEntityOffline: rejected componentID={}, dbid={}, addr={}.\n",
			componentID, dbid, pChannel != NULL ? pChannel->c_str() : "none"));
		return;
	}

	if (!Security::isValidPersistentEntityID(dbid))
	{
		WARNING_MSG(fmt::format("Dbmgr::onEntityOffline: rejected dbid={}, addr={}.\n",
			dbid, pChannel != NULL ? pChannel->c_str() : "none"));
		return;
	}

	if (!validateEntityScriptType("Dbmgr::onEntityOffline", sid))
		return;

	std::string dbInterfaceName;
	if (!resolveDbInterfaceName("Dbmgr::onEntityOffline", dbInterfaceIndex, dbInterfaceName))
		return;

	Buffered_DBTasks* pBuffered_DBTasks = findBufferedDBTask(dbInterfaceName);
	if (!pBuffered_DBTasks)
	{
		ERROR_MSG(fmt::format("Dbmgr::onEntityOffline: not found dbInterfaceIndex({})!\n", dbInterfaceIndex));
		return;
	}

	pBuffered_DBTasks->addTask(new DBTaskEntityOffline(pChannel->addr(), dbid, sid));
}

//-------------------------------------------------------------------------------------
void Dbmgr::executeRawDatabaseCommand(Network::Channel* pChannel,
									  KBEngine::MemoryStream& s)
{
	if (!validateRawDatabaseCommandStream(s))
	{
		WARNING_MSG(fmt::format("Dbmgr::executeRawDatabaseCommand: rejected truncated payload, remaining={}.\n",
			s.length()));
		s.done();
		return;
	}

	ENTITY_ID entityID = -1;
	s >> entityID;

	uint16 dbInterfaceIndex = 0;
	s >> dbInterfaceIndex;

	size_t sourcePayloadPosition = s.rpos();
	COMPONENT_ID sourceComponentID = 0;
	COMPONENT_TYPE sourceComponentType = UNKNOWN_COMPONENT_TYPE;
	s >> sourceComponentID >> sourceComponentType;
	s.rpos(sourcePayloadPosition);
	if (!isAllowedRawDatabaseSource(
		pChannel, sourceComponentType, sourceComponentID))
	{
		WARNING_MSG(fmt::format("Dbmgr::executeRawDatabaseCommand: rejected componentType={}, componentID={}, addr={}.\n",
			sourceComponentType, sourceComponentID,
			pChannel != NULL ? pChannel->c_str() : "local"));
		s.done();
		return;
	}

	std::string dbInterfaceName;
	if (!resolveDbInterfaceName("Dbmgr::executeRawDatabaseCommand", dbInterfaceIndex, dbInterfaceName))
	{
		s.done();
		return;
	}

	if (entityID == -1)
	{
		thread::ThreadPool* pThreadPool = DBUtil::pThreadPool(dbInterfaceName);
		if (!pThreadPool)
		{
			ERROR_MSG(fmt::format("Dbmgr::executeRawDatabaseCommand: not found pThreadPool(dbInterface={})!\n", dbInterfaceName));
			s.done();
			return;
		}

		pThreadPool->addTask(new DBTaskExecuteRawDatabaseCommand(pChannel ? pChannel->addr() : Network::Address::NONE, s));
	}
	else
	{
		Buffered_DBTasks* pBuffered_DBTasks = findBufferedDBTask(dbInterfaceName);
		if (!pBuffered_DBTasks)
		{
			ERROR_MSG(fmt::format("Dbmgr::executeRawDatabaseCommand: not found pBuffered_DBTasks(dbInterface={})!\n", dbInterfaceName));
			s.done();
			return;
		}

		pBuffered_DBTasks->addTask(new DBTaskExecuteRawDatabaseCommandByEntity(pChannel ? pChannel->addr() : Network::Address::NONE, s, entityID));
	}

	s.done();

	++numExecuteRawDatabaseCommand_;
}

//-------------------------------------------------------------------------------------
PyObject* Dbmgr::__py_executeRawDatabaseCommand(PyObject* self, PyObject* args)
{
	int argCount = (int)PyTuple_Size(args);
	PyObject* pycallback = NULL;
	PyObject* pyDBInterfaceName = NULL;
	int ret = 0;
	ENTITY_ID eid = -1;

	char* data = NULL;
	Py_ssize_t size;

	if (argCount == 4)
		ret = PyArg_ParseTuple(args, "s#|O|i|O", &data, &size, &pycallback, &eid, &pyDBInterfaceName);
	else if (argCount == 3)
		ret = PyArg_ParseTuple(args, "s#|O|i", &data, &size, &pycallback, &eid);
	else if (argCount == 2)
		ret = PyArg_ParseTuple(args, "s#|O", &data, &size, &pycallback);
	else if (argCount == 1)
		ret = PyArg_ParseTuple(args, "s#", &data, &size);

	if (!ret)
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::executeRawDatabaseCommand: args error!");
		PyErr_PrintEx(0);
		S_Return;
	}

	std::string dbInterfaceName = "default";
	if (pyDBInterfaceName)
	{
		dbInterfaceName = PyUnicode_AsUTF8AndSize(pyDBInterfaceName, NULL);

		if (!g_kbeSrvConfig.dbInterface(dbInterfaceName))
		{
			PyErr_Format(PyExc_TypeError, "KBEngine::executeRawDatabaseCommand: args4, incorrect dbInterfaceName(%s)!",
				dbInterfaceName.c_str());

			PyErr_PrintEx(0);
			S_Return;
		}
	}

	Dbmgr::getSingleton().executeRawDatabaseCommand(data, (uint32)size, pycallback, eid, dbInterfaceName);
	S_Return;
}

//-------------------------------------------------------------------------------------
void Dbmgr::executeRawDatabaseCommand(const char* datas, uint32 size, PyObject* pycallback, ENTITY_ID eid, const std::string& dbInterfaceName)
{
	if (datas == NULL)
	{
		ERROR_MSG("KBEngine::executeRawDatabaseCommand: execute error!\n");
		return;
	}

	int dbInterfaceIndex = g_kbeSrvConfig.dbInterfaceName2dbInterfaceIndex(dbInterfaceName);
	if (dbInterfaceIndex < 0)
	{
		ERROR_MSG(fmt::format("KBEngine::executeRawDatabaseCommand: not found dbInterface({})!\n",
			dbInterfaceName));

		return;
	}

	//INFO_MSG(fmt::format("KBEngine::executeRawDatabaseCommand{}:{}.\n", (eid > 0 ? fmt::format("(entityID={})", eid) : ""), datas));

	MemoryStream* pMemoryStream = MemoryStream::createPoolObject(OBJECTPOOL_POINT);
	(*pMemoryStream) << eid;
	(*pMemoryStream) << (uint16)dbInterfaceIndex;
	(*pMemoryStream) << componentID_ << componentType_;

	CALLBACK_ID callbackID = 0;

	if (pycallback && PyCallable_Check(pycallback))
		callbackID = callbackMgr().save(pycallback);

	(*pMemoryStream) << callbackID;
	(*pMemoryStream) << size;
	(*pMemoryStream).append(datas, size);
	executeRawDatabaseCommand(NULL, *pMemoryStream);
	MemoryStream::reclaimPoolObject(pMemoryStream);
}

//-------------------------------------------------------------------------------------
void Dbmgr::onExecuteRawDatabaseCommandCB(KBEngine::MemoryStream& s)
{
	std::string err;
	CALLBACK_ID callbackID = 0;
	uint32 nrows = 0;
	uint32 nfields = 0;
	uint64 affectedRows = 0;
	uint64 lastInsertID = 0;

	PyObject* pResultSet = NULL;
	PyObject* pAffectedRows = NULL;
	PyObject* pLastInsertID = NULL;
	PyObject* pErrorMsg = NULL;

	s >> callbackID;
	s >> err;

	if (err.size() <= 0)
	{
		s >> nfields;

		pErrorMsg = Py_None;
		Py_INCREF(pErrorMsg);

		if (nfields > 0)
		{
			pAffectedRows = Py_None;
			Py_INCREF(pAffectedRows);

			pLastInsertID = Py_None;
			Py_INCREF(pLastInsertID);

			s >> nrows;

			pResultSet = PyList_New(nrows);
			for (uint32 i = 0; i < nrows; ++i)
			{
				PyObject* pRow = PyList_New(nfields);
				for (uint32 j = 0; j < nfields; ++j)
				{
					std::string cell;
					s.readBlob(cell);

					PyObject* pCell = NULL;

					if (cell == "KBE_QUERY_DB_NULL")
					{
						Py_INCREF(Py_None);
						pCell = Py_None;
					}
					else
					{
						pCell = PyBytes_FromStringAndSize(cell.data(), cell.length());
					}

					PyList_SET_ITEM(pRow, j, pCell);
				}

				PyList_SET_ITEM(pResultSet, i, pRow);
			}
		}
		else
		{
			pResultSet = Py_None;
			Py_INCREF(pResultSet);

			pErrorMsg = Py_None;
			Py_INCREF(pErrorMsg);

			s >> affectedRows;

			pAffectedRows = PyLong_FromUnsignedLongLong(affectedRows);

			s >> lastInsertID;
			pLastInsertID = PyLong_FromUnsignedLongLong(lastInsertID);
		}
	}
	else
	{
		pResultSet = Py_None;
		Py_INCREF(pResultSet);

		pErrorMsg = PyUnicode_FromString(err.c_str());

		pAffectedRows = Py_None;
		Py_INCREF(pAffectedRows);

		pLastInsertID = Py_None;
		Py_INCREF(pLastInsertID);
	}

	s.done();

	//DEBUG_MSG(fmt::format("Cellapp::onExecuteRawDatabaseCommandCB: nrows={}, nfields={}, err={}.\n", 
	//	nrows, nfields, err.c_str()));

	if (callbackID > 0)
	{
		SCOPED_PROFILE(SCRIPTCALL_PROFILE);

		PyObjectPtr pyfunc = pyCallbackMgr_.take(callbackID);
		if (pyfunc != NULL)
		{
			PyObject* pyResult = PyObject_CallFunction(pyfunc.get(),
				const_cast<char*>("OOOO"),
				pResultSet, pAffectedRows, pLastInsertID, pErrorMsg);

			if (pyResult != NULL)
				Py_DECREF(pyResult);
			else
				SCRIPT_ERROR_CHECK();
		}
		else
		{
			ERROR_MSG(fmt::format("Cellapp::onExecuteRawDatabaseCommandCB: not found callback:{}.\n",
				callbackID));
		}
	}

	Py_XDECREF(pResultSet);
	Py_XDECREF(pAffectedRows);
	Py_XDECREF(pLastInsertID);
	Py_XDECREF(pErrorMsg);
}

//-------------------------------------------------------------------------------------
void Dbmgr::writeEntity(Network::Channel* pChannel,
						KBEngine::MemoryStream& s)
{
	if (!hasRemainingBytes(s, sizeof(COMPONENT_ID) + sizeof(ENTITY_ID) + sizeof(DBID) + sizeof(uint16)))
	{
		WARNING_MSG("Dbmgr::writeEntity: rejected incomplete fixed header.\n");
		s.done();
		return;
	}

	ENTITY_ID eid;
	DBID entityDBID;
	COMPONENT_ID componentID;
	uint16 dbInterfaceIndex;

	s >> componentID >> eid >> entityDBID >> dbInterfaceIndex;

	if (findBoundBaseappSource(pChannel, componentID) == NULL || eid <= 0 ||
		(entityDBID != 0 && !Security::isValidPersistentEntityID(entityDBID)))
	{
		WARNING_MSG(fmt::format("Dbmgr::writeEntity: rejected componentID={}, entityID={}, entityDBID={}, addr={}.\n",
			componentID, eid, entityDBID, pChannel->c_str()));
		s.done();
		return;
	}

	const size_t entityPayloadPosition = s.rpos();
	ENTITY_SCRIPT_UID sid = 0;
	CALLBACK_ID callbackID = 0;
	int8 shouldAutoLoad = -1;
	if (s.length() < sizeof(sid) + sizeof(callbackID) + sizeof(shouldAutoLoad))
	{
		WARNING_MSG("Dbmgr::writeEntity: rejected incomplete entity write header.\n");
		s.done();
		return;
	}

	s >> sid >> callbackID >> shouldAutoLoad;
	s.rpos(entityPayloadPosition);
	if (!validateEntityScriptType("Dbmgr::writeEntity", sid))
	{
		s.done();
		return;
	}

	if (shouldAutoLoad != -1 && shouldAutoLoad != 0 && shouldAutoLoad != 1)
	{
		WARNING_MSG(fmt::format("Dbmgr::writeEntity: rejected shouldAutoLoad={}.\n",
			shouldAutoLoad));
		s.done();
		return;
	}

	std::string dbInterfaceName;
	if (!resolveDbInterfaceName("Dbmgr::writeEntity", dbInterfaceIndex, dbInterfaceName))
	{
		s.done();
		return;
	}

	Buffered_DBTasks* pBuffered_DBTasks = findBufferedDBTask(dbInterfaceName);
	if (!pBuffered_DBTasks)
	{
		ERROR_MSG(fmt::format("Dbmgr::writeEntity: not found dbInterfaceIndex({})!\n", dbInterfaceIndex));
		s.done();
		return;
	}

	pBuffered_DBTasks->addTask(new DBTaskWriteEntity(pChannel->addr(), componentID, eid, entityDBID, s));
	s.done();

	++numWrittenEntity_;
}

//-------------------------------------------------------------------------------------
void Dbmgr::removeEntity(Network::Channel* pChannel, KBEngine::MemoryStream& s)
{
	if (!hasRemainingBytes(s, sizeof(uint16) + sizeof(COMPONENT_ID) + sizeof(ENTITY_ID) + sizeof(DBID)))
	{
		WARNING_MSG("Dbmgr::removeEntity: rejected incomplete fixed header.\n");
		s.done();
		return;
	}

	ENTITY_ID eid;
	DBID entityDBID;
	COMPONENT_ID componentID;
	uint16 dbInterfaceIndex;

	s >> dbInterfaceIndex >> componentID >> eid >> entityDBID;
	if (findBoundBaseappSource(pChannel, componentID) == NULL ||
		!Security::isValidPersistentEntityID(entityDBID) || eid <= 0)
	{
		WARNING_MSG(fmt::format("Dbmgr::removeEntity: rejected componentID={}, entityDBID={}, addr={}.\n",
			componentID, entityDBID, pChannel->c_str()));
		s.done();
		return;
	}

	const size_t entityPayloadPosition = s.rpos();
	ENTITY_SCRIPT_UID sid = 0;
	if (s.length() < sizeof(sid))
	{
		WARNING_MSG("Dbmgr::removeEntity: rejected missing entity script type.\n");
		s.done();
		return;
	}

	s >> sid;
	s.rpos(entityPayloadPosition);
	if (!validateEntityScriptType("Dbmgr::removeEntity", sid))
	{
		s.done();
		return;
	}

	std::string dbInterfaceName;
	if (!resolveDbInterfaceName("Dbmgr::removeEntity", dbInterfaceIndex, dbInterfaceName))
	{
		s.done();
		return;
	}

	Buffered_DBTasks* pBuffered_DBTasks = findBufferedDBTask(dbInterfaceName);
	if (!pBuffered_DBTasks)
	{
		ERROR_MSG(fmt::format("Dbmgr::removeEntity: not found dbInterfaceIndex({})!\n", dbInterfaceIndex));
		s.done();
		return;
	}

	pBuffered_DBTasks->addTask(new DBTaskRemoveEntity(pChannel->addr(),
		componentID, eid, entityDBID, s));

	s.done();

	++numRemovedEntity_;
}

//-------------------------------------------------------------------------------------
void Dbmgr::entityAutoLoad(Network::Channel* pChannel, KBEngine::MemoryStream& s)
{
	COMPONENT_ID componentID;
	ENTITY_SCRIPT_UID entityType;
	ENTITY_ID start;
	ENTITY_ID end;
	uint16 dbInterfaceIndex = 0;

	s >> dbInterfaceIndex >> componentID >> entityType >> start >> end;
	if (findBoundBaseappSource(pChannel, componentID) == NULL)
	{
		WARNING_MSG(fmt::format("Dbmgr::entityAutoLoad: rejected unbound componentID={}, addr={}.\n",
			componentID, pChannel->c_str()));
		s.done();
		return;
	}

	if (!validateEntityScriptType("Dbmgr::entityAutoLoad", entityType))
	{
		s.done();
		return;
	}

	if (!validateEntityAutoLoadRange("Dbmgr::entityAutoLoad", start, end))
	{
		s.done();
		return;
	}

	std::string dbInterfaceName;
	if (!resolveDbInterfaceName("Dbmgr::entityAutoLoad", dbInterfaceIndex, dbInterfaceName))
	{
		s.done();
		return;
	}

	thread::ThreadPool* pThreadPool = DBUtil::pThreadPool(dbInterfaceName);
	if (pThreadPool == NULL)
	{
		WARNING_MSG(fmt::format("Dbmgr::entityAutoLoad: rejected dbInterfaceIndex={}.\n",
			dbInterfaceIndex));
		s.done();
		return;
	}

	pThreadPool->addTask(new DBTaskEntityAutoLoad(pChannel->addr(), componentID, entityType, start, end));
	s.done();
}

//-------------------------------------------------------------------------------------
void Dbmgr::deleteEntityByDBID(Network::Channel* pChannel, KBEngine::MemoryStream& s)
{
	if (!hasRemainingBytes(s, sizeof(uint16) + sizeof(COMPONENT_ID) + sizeof(DBID) +
		sizeof(CALLBACK_ID) + sizeof(ENTITY_SCRIPT_UID)))
	{
		WARNING_MSG("Dbmgr::deleteEntityByDBID: rejected incomplete fixed header.\n");
		s.done();
		return;
	}

	COMPONENT_ID componentID;
	ENTITY_SCRIPT_UID sid;
	CALLBACK_ID callbackID = 0;
	DBID entityDBID;
	uint16 dbInterfaceIndex = 0;

	s >> dbInterfaceIndex >> componentID >> entityDBID >> callbackID >> sid;
	if (findBoundBaseappSource(pChannel, componentID) == NULL ||
		!Security::isValidPersistentEntityID(entityDBID) ||
		!validateEntityScriptType("Dbmgr::deleteEntityByDBID", sid))
	{
		WARNING_MSG(fmt::format("Dbmgr::deleteEntityByDBID: rejected componentID={}, entityDBID={}, addr={}.\n",
			componentID, entityDBID, pChannel->c_str()));
		s.done();
		return;
	}

	std::string dbInterfaceName;
	if (!resolveDbInterfaceName("Dbmgr::deleteEntityByDBID", dbInterfaceIndex, dbInterfaceName))
	{
		s.done();
		return;
	}

	thread::ThreadPool* pThreadPool = DBUtil::pThreadPool(dbInterfaceName);
	if (pThreadPool == NULL)
	{
		WARNING_MSG(fmt::format("Dbmgr::deleteEntityByDBID: rejected dbInterfaceIndex={}.\n",
			dbInterfaceIndex));
		s.done();
		return;
	}

	pThreadPool->addTask(new DBTaskDeleteEntityByDBID(
		pChannel->addr(), componentID, entityDBID, callbackID, sid));
	s.done();
}

//-------------------------------------------------------------------------------------
void Dbmgr::lookUpEntityByDBID(Network::Channel* pChannel, KBEngine::MemoryStream& s)
{
	if (!hasRemainingBytes(s, sizeof(uint16) + sizeof(COMPONENT_ID) + sizeof(DBID) +
		sizeof(CALLBACK_ID) + sizeof(ENTITY_SCRIPT_UID)))
	{
		WARNING_MSG("Dbmgr::lookUpEntityByDBID: rejected incomplete fixed header.\n");
		s.done();
		return;
	}

	COMPONENT_ID componentID;
	ENTITY_SCRIPT_UID sid;
	CALLBACK_ID callbackID = 0;
	DBID entityDBID;
	uint16 dbInterfaceIndex = 0;

	s >> dbInterfaceIndex >> componentID >> entityDBID >> callbackID >> sid;
	if (findBoundBaseappSource(pChannel, componentID) == NULL ||
		!Security::isValidPersistentEntityID(entityDBID) ||
		!validateEntityScriptType("Dbmgr::lookUpEntityByDBID", sid))
	{
		WARNING_MSG(fmt::format("Dbmgr::lookUpEntityByDBID: rejected componentID={}, entityDBID={}, addr={}.\n",
			componentID, entityDBID, pChannel->c_str()));
		s.done();
		return;
	}

	std::string dbInterfaceName;
	if (!resolveDbInterfaceName("Dbmgr::lookUpEntityByDBID", dbInterfaceIndex, dbInterfaceName))
	{
		s.done();
		return;
	}

	thread::ThreadPool* pThreadPool = DBUtil::pThreadPool(dbInterfaceName);
	if (pThreadPool == NULL)
	{
		WARNING_MSG(fmt::format("Dbmgr::lookUpEntityByDBID: rejected dbInterfaceIndex={}.\n",
			dbInterfaceIndex));
		s.done();
		return;
	}

	pThreadPool->addTask(new DBTaskLookUpEntityByDBID(
		pChannel->addr(), componentID, entityDBID, callbackID, sid));
	s.done();
}

//-------------------------------------------------------------------------------------
void Dbmgr::queryEntity(Network::Channel* pChannel, uint16 dbInterfaceIndex, COMPONENT_ID componentID, int8 queryMode, DBID dbid,
	std::string& entityType, CALLBACK_ID callbackID, ENTITY_ID entityID)
{
	// The callback target is carried in the payload, so bind it to the concrete
	// requesting BaseApp Channel before scheduling any database work.
	// 回调目标来自网络载荷，因此必须先绑定到实际发起请求的 BaseApp Channel，
	// 防止一个已注册 BaseApp 把查询结果路由到另一个 BaseApp。
	if (findBoundBaseappSource(pChannel, componentID) == NULL ||
		!Security::isValidPersistentEntityID(dbid) ||
		!Security::isValidDatabaseQueryMode(queryMode) || entityID <= 0 ||
		!validateEntityScriptType("Dbmgr::queryEntity", entityType))
	{
		WARNING_MSG(fmt::format("Dbmgr::queryEntity: rejected componentID={}, queryMode={}, dbid={}, entityID={}, addr={}.\n",
			componentID, queryMode, dbid, entityID, pChannel->c_str()));
		return;
	}

	std::string dbInterfaceName;
	if (!resolveDbInterfaceName("Dbmgr::queryEntity", dbInterfaceIndex, dbInterfaceName))
		return;

	Buffered_DBTasks* pBufferedDBTasks = findBufferedDBTask(dbInterfaceName);
	if (pBufferedDBTasks == NULL)
	{
		WARNING_MSG(fmt::format("Dbmgr::queryEntity: rejected dbInterfaceIndex={}.\n",
			dbInterfaceIndex));
		return;
	}

	pBufferedDBTasks->addTask(new DBTaskQueryEntity(
		pChannel->addr(), queryMode, entityType, dbid, componentID, callbackID, entityID));

	numQueryEntity_++;
}

//-------------------------------------------------------------------------------------
void Dbmgr::syncEntityStreamTemplate(Network::Channel* pChannel, KBEngine::MemoryStream& s)
{
	if (!isExpectedIngressSource(BASEAPP_TYPE, pChannel,
		"Dbmgr::syncEntityStreamTemplate"))
	{
		s.done();
		return;
	}

	size_t rpos = s.rpos();
	EntityTables::ENTITY_TABLES_MAP::iterator iter = EntityTables::sEntityTables.begin();
	for (; iter != EntityTables::sEntityTables.end(); ++iter)
	{
		KBEAccountTable* pTable =
			static_cast<KBEAccountTable*>(iter->second.findKBETable(KBE_TABLE_PERFIX "_accountinfos"));

		if (pTable == NULL)
		{
			ERROR_MSG(fmt::format("Dbmgr::syncEntityStreamTemplate: rejected missing account table, dbInterface={}.\n",
				iter->first));
			s.done();
			return;
		}
	}

	// Validate every interface first so one malformed backend cannot leave only a
	// subset of account templates updated.
	// 先验证全部数据库接口，避免某个后端缺表时只更新了部分账户模板。
	for (iter = EntityTables::sEntityTables.begin();
		iter != EntityTables::sEntityTables.end(); ++iter)
	{
		KBEAccountTable* pTable =
			static_cast<KBEAccountTable*>(iter->second.findKBETable(KBE_TABLE_PERFIX "_accountinfos"));

		s.rpos(rpos);
		pTable->accountDefMemoryStream(s);
	}

	s.done();
}

//-------------------------------------------------------------------------------------
void Dbmgr::charge(Network::Channel* pChannel, KBEngine::MemoryStream& s)
{
	if (!isExpectedIngressSource(BASEAPP_TYPE, pChannel, "Dbmgr::charge"))
	{
		s.done();
		return;
	}
	if (!InterfacesPayloadGuard::validateChargeRequestStream(s))
	{
		WARNING_MSG("Dbmgr::charge: rejected malformed or oversized payload.\n");
		s.done();
		return;
	}

	InterfacesHandler* pHandler = findInterfacesHandlerOrWarn("Dbmgr::charge");
	if (pHandler != NULL)
		pHandler->charge(pChannel, s);
	else
		s.done();
}

//-------------------------------------------------------------------------------------
void Dbmgr::onChargeCB(Network::Channel* pChannel, KBEngine::MemoryStream& s)
{
	if (!isExpectedIngressSource(INTERFACES_TYPE, pChannel, "Dbmgr::onChargeCB"))
	{
		s.done();
		return;
	}
	if (!InterfacesPayloadGuard::validateChargeCallbackStream(s))
	{
		WARNING_MSG("Dbmgr::onChargeCB: rejected malformed or oversized payload.\n");
		s.done();
		return;
	}

	InterfacesHandler* pHandler = findInterfacesHandlerOrWarn("Dbmgr::onChargeCB");
	if (pHandler != NULL)
		pHandler->onChargeCB(pChannel, s);
	else
		s.done();
}

//-------------------------------------------------------------------------------------
void Dbmgr::eraseClientReq(Network::Channel* pChannel, std::string& logkey)
{
	if (!isExpectedIngressSource(LOGINAPP_TYPE, pChannel, "Dbmgr::eraseClientReq"))
		return;
	if (!InterfacesPayloadGuard::isValidClientRequestKey(logkey))
	{
		WARNING_MSG(fmt::format("Dbmgr::eraseClientReq: rejected keySize={}.\n", logkey.size()));
		return;
	}

	std::vector<InterfacesHandler*>::iterator iter = pInterfacesHandlers_.begin();
	for (; iter != pInterfacesHandlers_.end(); ++iter)
		(*iter)->eraseClientReq(pChannel, logkey);
}

//-------------------------------------------------------------------------------------
void Dbmgr::accountActivate(Network::Channel* pChannel, std::string& scode)
{
	if (!isExpectedIngressSource(LOGINAPP_TYPE, pChannel, "Dbmgr::accountActivate"))
		return;
	if (!AccountRequestGuard::isValidVerificationCode(scode))
	{
		WARNING_MSG(fmt::format("Dbmgr::accountActivate: rejected codeSize={}.\n", scode.size()));
		return;
	}

	INFO_MSG("Dbmgr::accountActivate: request received.\n");
	InterfacesHandler* pHandler = findInterfacesHandlerOrWarn("Dbmgr::accountActivate");
	if (pHandler != NULL)
		pHandler->accountActivate(pChannel, scode);
}

//-------------------------------------------------------------------------------------
void Dbmgr::accountReqResetPassword(Network::Channel* pChannel, std::string& accountName)
{
	if (!isExpectedIngressSource(LOGINAPP_TYPE, pChannel,
		"Dbmgr::accountReqResetPassword"))
	{
		return;
	}
	if (!AccountRequestGuard::isValidAccountName(accountName))
	{
		WARNING_MSG(fmt::format("Dbmgr::accountReqResetPassword: rejected accountNameSize={}.\n",
			accountName.size()));
		return;
	}

	INFO_MSG(fmt::format("Dbmgr::accountReqResetPassword: accountNameSize={}.\n", accountName.size()));
	InterfacesHandler* pHandler = findInterfacesHandlerOrWarn("Dbmgr::accountReqResetPassword");
	if (pHandler != NULL)
		pHandler->accountReqResetPassword(pChannel, accountName);
}

//-------------------------------------------------------------------------------------
void Dbmgr::accountResetPassword(Network::Channel* pChannel, std::string& accountName, std::string& newpassword, std::string& code)
{
	if (!isExpectedIngressSource(LOGINAPP_TYPE, pChannel, "Dbmgr::accountResetPassword"))
		return;
	if (!AccountRequestGuard::isValidAccountName(accountName) ||
		!AccountRequestGuard::isValidPassword(newpassword) ||
		!AccountRequestGuard::isValidVerificationCode(code))
	{
		WARNING_MSG(fmt::format("Dbmgr::accountResetPassword: rejected fields, accountNameSize={}, newPasswordSize={}, codeSize={}.\n",
			accountName.size(), newpassword.size(), code.size()));
		return;
	}

	INFO_MSG(fmt::format("Dbmgr::accountResetPassword: accountNameSize={}.\n", accountName.size()));
	InterfacesHandler* pHandler = findInterfacesHandlerOrWarn("Dbmgr::accountResetPassword");
	if (pHandler != NULL)
		pHandler->accountResetPassword(pChannel, accountName, newpassword, code);
}

//-------------------------------------------------------------------------------------
void Dbmgr::accountReqBindMail(Network::Channel* pChannel, ENTITY_ID entityID, std::string& accountName,
							   std::string& password, std::string& email)
{
	if (!isExpectedIngressSource(BASEAPP_TYPE, pChannel, "Dbmgr::accountReqBindMail"))
		return;
	if (entityID <= 0 || !AccountRequestGuard::isValidAccountName(accountName) ||
		!AccountRequestGuard::isValidPassword(password) ||
		!AccountRequestGuard::isValidAccountName(email))
	{
		WARNING_MSG(fmt::format("Dbmgr::accountReqBindMail: rejected fields, entityID={}, accountNameSize={}, passwordSize={}, emailSize={}.\n",
			entityID, accountName.size(), password.size(), email.size()));
		return;
	}

	// This path handles both account identifiers and destination email addresses;
	// never persist either value in logs. 此路径同时处理账户标识和目标邮箱，日志不得保留其内容。
	INFO_MSG(fmt::format("Dbmgr::accountReqBindMail: accountNameSize={}, emailSize={}.\n",
		accountName.size(), email.size()));
	InterfacesHandler* pHandler = findInterfacesHandlerOrWarn("Dbmgr::accountReqBindMail");
	if (pHandler != NULL)
		pHandler->accountReqBindMail(pChannel, entityID, accountName, password, email);
}

//-------------------------------------------------------------------------------------
void Dbmgr::accountBindMail(Network::Channel* pChannel, std::string& username, std::string& scode)
{
	if (!isExpectedIngressSource(LOGINAPP_TYPE, pChannel, "Dbmgr::accountBindMail"))
		return;
	if (!AccountRequestGuard::isValidAccountName(username) ||
		!AccountRequestGuard::isValidVerificationCode(scode))
	{
		WARNING_MSG(fmt::format("Dbmgr::accountBindMail: rejected fields, usernameSize={}, codeSize={}.\n",
			username.size(), scode.size()));
		return;
	}

	INFO_MSG(fmt::format("Dbmgr::accountBindMail: usernameSize={}.\n", username.size()));
	InterfacesHandler* pHandler = findInterfacesHandlerOrWarn("Dbmgr::accountBindMail");
	if (pHandler != NULL)
		pHandler->accountBindMail(pChannel, username, scode);
}

//-------------------------------------------------------------------------------------
void Dbmgr::accountNewPassword(Network::Channel* pChannel, ENTITY_ID entityID, std::string& accountName,
							   std::string& password, std::string& newpassword)
{
	if (!isExpectedIngressSource(BASEAPP_TYPE, pChannel, "Dbmgr::accountNewPassword"))
		return;
	if (entityID <= 0 || !AccountRequestGuard::isValidAccountName(accountName) ||
		!AccountRequestGuard::isValidPassword(password) ||
		!AccountRequestGuard::isValidPassword(newpassword))
	{
		WARNING_MSG(fmt::format("Dbmgr::accountNewPassword: rejected fields, entityID={}, accountNameSize={}, oldPasswordSize={}, newPasswordSize={}.\n",
			entityID, accountName.size(), password.size(), newpassword.size()));
		return;
	}

	INFO_MSG(fmt::format("Dbmgr::accountNewPassword: accountNameSize={}.\n", accountName.size()));
	InterfacesHandler* pHandler = findInterfacesHandlerOrWarn("Dbmgr::accountNewPassword");
	if (pHandler != NULL)
		pHandler->accountNewPassword(pChannel, entityID, accountName, password, newpassword);
}

//-------------------------------------------------------------------------------------
std::string Dbmgr::selectAccountDBInterfaceName(const std::string& name)
{
	std::string dbInterfaceName = "default";

	// 把请求交由脚本处理
	SCOPED_PROFILE(SCRIPTCALL_PROFILE);
	PyObject* pyResult = PyObject_CallMethod(getEntryScript().get(),
		const_cast<char*>("onSelectAccountDBInterface"),
		const_cast<char*>("s"),
		name.c_str());

	if (pyResult != NULL)
	{
		dbInterfaceName = PyUnicode_AsUTF8AndSize(pyResult, NULL);
		Py_DECREF(pyResult);
	}
	else
	{
		SCRIPT_ERROR_CHECK();
	}

	if (dbInterfaceName == "" || g_kbeSrvConfig.dbInterface(dbInterfaceName) == NULL)
	{
		ERROR_MSG(fmt::format("Dbmgr::selectAccountDBInterfaceName: not found dbInterface({}), accountName={}.\n", dbInterfaceName, name));
		return "default";
	}

	return dbInterfaceName;
}

//-------------------------------------------------------------------------------------
void Dbmgr::onChannelDeregister(Network::Channel * pChannel)
{
	// 如果是app死亡了
	if (pChannel->isInternal())
	{
		Components::ComponentInfos* cinfo = Components::getSingleton().findComponent(pChannel);
		if (cinfo)
		{
			if (cinfo->componentType == BASEAPP_TYPE)
			{
				loseBaseappts_[cinfo->cid] = timestamp() + uint64(60 * stampsPerSecond());
				WARNING_MSG(fmt::format("Dbmgr::onChannelDeregister(): If the process cannot be resumed, the entitylog(baseapp={}) will be cleaned up after 60 seconds!\n", cinfo->cid));
			}
		}
	}

	ServerApp::onChannelDeregister(pChannel);
}

//-------------------------------------------------------------------------------------
}
