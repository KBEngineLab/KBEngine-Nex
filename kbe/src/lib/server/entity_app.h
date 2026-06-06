// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

#ifndef KBE_ENTITY_APP_H
#define KBE_ENTITY_APP_H

// common include
#include <sys/stat.h>
#include "pyscript/py_gc.h"
#include "pyscript/script.h"
#include "pyscript/pyprofile.h"
#include "pyscript/pyprofile_handler.h"
#include "common/common.h"
#include "common/timer.h"
#include "common/smartpointer.h"
#include "pyscript/pyobject_pointer.h"
#include "pyscript/pywatcher.h"
#include "helper/debug_helper.h"
#include "helper/debug_option.h"
#include "helper/script_loglevel.h"
#include "helper/profile.h"
#include "server/kbemain.h"
#include "server/script_timers.h"
#include "server/asyncio_helper.h"
#include "server/idallocate.h"
#include "server/python_app.h"
#include "server/serverconfig.h"
#include "server/globaldata_client.h"
#include "server/globaldata_server.h"
#include "resmgr/plugins/plugin_manager.h"
#include "server/callbackmgr.h"
#include "entitydef/entitydef.h"
#include "entitydef/entities.h"
#include "entitydef/entity_call.h"
#include "entitydef/entity_component.h"
#include "entitydef/scriptdef_module.h"
#include "network/message_handler.h"
#include "resmgr/resmgr.h"
#include "helper/console_helper.h"
#include "server/serverapp.h"
#include <algorithm>
#include <cctype>
#include <cstring>

#if KBE_PLATFORM == PLATFORM_WIN32
#pragma warning (disable : 4996)
#endif


namespace KBEngine{

/**
	EntityApp 热更在线实体后的统计信息。
	entitiesReloaded 统计已经切换到新脚本类型的在线 Entity 数量；
	onReloadCallbacks 统计成功调用脚本 onReload(fullReload) 的 Entity 数量。
*/
struct ReloadScriptEntityStats
{
	ReloadScriptEntityStats() :
		entitiesReloaded(0),
		onReloadCallbacks(0)
	{
	}

	uint32 entitiesReloaded;
	uint32 onReloadCallbacks;
};

namespace entityapp_plugins
{

static inline std::string normalizePath(std::string path)
{
	std::replace(path.begin(), path.end(), '\\', '/');
	return path;
}

static inline bool pathExists(const std::string& path)
{
	return access(path.c_str(), 0) == 0;
}

static inline std::string componentFolder(COMPONENT_TYPE componentType)
{
	if (componentType == BASEAPP_TYPE)
		return "base";
	if (componentType == CELLAPP_TYPE)
		return "cell";
	if (componentType == DBMGR_TYPE)
		return "db";
	if (componentType == INTERFACES_TYPE)
		return "interface";
	if (componentType == LOGINAPP_TYPE)
		return "login";
	if (componentType == LOGGER_TYPE)
		return "logger";
	if (componentType == BOTS_TYPE)
		return "bots";
	if (componentType == CLIENT_TYPE)
		return "client";
	return "";
}

static inline std::string safeModuleName(std::string value)
{
	for (std::string::iterator iter = value.begin(); iter != value.end(); ++iter)
	{
		if (!isalnum((unsigned char)*iter))
			*iter = '_';
	}
	return value;
}

static inline std::string entryPath(const PluginDescriptor& plugin, COMPONENT_TYPE componentType, const std::string& entry)
{
	if (entry.find('/') != std::string::npos || entry.find('\\') != std::string::npos)
		return normalizePath(plugin.rootPath + "/" + entry);

	return normalizePath(plugin.rootPath + "/" + componentFolder(componentType) + "/" + entry + ".py");
}

static inline void callEntry(PyObject* pyEntry, const std::string& eventName, const char* format, bool arg)
{
	if (PyObject_HasAttrString(pyEntry, eventName.c_str()) <= 0)
		return;

	PyObject* pyResult = NULL;
	if (format && strlen(format) > 0)
		pyResult = PyObject_CallMethod(pyEntry, const_cast<char*>(eventName.c_str()), const_cast<char*>(format), arg ? 1 : 0);
	else
		pyResult = PyObject_CallMethod(pyEntry, const_cast<char*>(eventName.c_str()), const_cast<char*>(""));

	if (pyResult)
		Py_DECREF(pyResult);
	else
		SCRIPT_ERROR_CHECK();
}

}

template<class E>
class EntityApp : public ServerApp
{
public:
	enum TimeOutType
	{
		TIMEOUT_GAME_TICK = TIMEOUT_SERVERAPP_MAX + 1,
		TIMEOUT_ENTITYAPP_MAX
	};

public:
	EntityApp(Network::EventDispatcher& dispatcher,
		Network::NetworkInterface& ninterface,
		COMPONENT_TYPE componentType,
		COMPONENT_ID componentID);

	~EntityApp();

	/**
		相关处理接口
	*/
	virtual void handleTimeout(TimerHandle handle, void * arg);
	virtual void handleGameTick();

	/**
		通过entityID寻找到对应的实例
	*/
	E* findEntity(ENTITY_ID entityID);

	/**
		通过entityID销毁一个entity
	*/
	virtual bool destroyEntity(ENTITY_ID entityID, bool callScript);

	/**
		由entityCall等来尝试获取一个entity的实例
		因为这个组件上不一定存在这个entity。
	*/
	PyObject* tryGetEntity(COMPONENT_ID componentID, ENTITY_ID eid);

	/**
		由entityCall来尝试获取一个channel的实例
	*/
	Network::Channel* findChannelByEntityCall(EntityCallAbstract& entityCall);

	KBEngine::script::Script& getScript(){ return script_; }
	PyObjectPtr getEntryScript(){ return entryScript_; }

	void registerScript(PyTypeObject*);
	int registerPyObjectToScript(const char* attrName, PyObject* pyObj);
	int unregisterPyObjectToScript(const char* attrName);

	bool installPyScript();
	virtual bool installPyModules();
	virtual void onInstallPyModules() {};
	virtual bool uninstallPyModules();
	bool uninstallPyScript();
	bool installPluginModules();
	void uninstallPluginModules();
	void dispatchPluginEvent(const std::string& eventName);
	void dispatchPluginEvent(const std::string& eventName, bool arg);
	bool installEntityDef();

	virtual bool initializeWatcher();

	virtual void finalise();
	virtual bool inInitialize();
	virtual bool initialize();

	virtual void onSignalled(int sigNum);

	Entities<E>* pEntities() const{ return pEntities_; }
	ArraySize entitiesSize() const { return (ArraySize)pEntities_->size(); }

	PY_CALLBACKMGR& callbackMgr(){ return pyCallbackMgr_; }

	EntityIDClient& idClient(){ return idClient_; }

	/**
		创建一个entity
	*/
	E* createEntity(const char* entityType, PyObject* params,
		bool isInitializeScript = true, ENTITY_ID eid = 0, bool initProperty = true);

	virtual E* onCreateEntity(PyObject* pyEntity, ScriptDefModule* sm, ENTITY_ID eid);

	/** 网络接口
		请求分配一个ENTITY_ID段的回调
	*/
	void onReqAllocEntityID(Network::Channel* pChannel, ENTITY_ID startID, ENTITY_ID endID);

	/** 网络接口
		dbmgr发送初始信息
		startID: 初始分配ENTITY_ID 段起始位置
		endID: 初始分配ENTITY_ID 段结束位置
		startGlobalOrder: 全局启动顺序 包括各种不同组件
		startGroupOrder: 组内启动顺序， 比如在所有baseapp中第几个启动。
	*/
	void onDbmgrInitCompleted(Network::Channel* pChannel,
		GAME_TIME gametime, ENTITY_ID startID, ENTITY_ID endID, COMPONENT_ORDER startGlobalOrder,
		COMPONENT_ORDER startGroupOrder, const std::string& digest);

	/** 网络接口
		dbmgr广播global数据的改变
	*/
	void onBroadcastGlobalDataChanged(Network::Channel* pChannel, KBEngine::MemoryStream& s);


	/** 网络接口
		请求执行一段python指令
	*/
	void onExecScriptCommand(Network::Channel* pChannel, KBEngine::MemoryStream& s);

	/**
		console请求开始profile
	*/
	virtual void startProfile_(Network::Channel* pChannel, std::string profileName, int8 profileType, uint32 timelen);

	/**
		允许脚本assert底层
	*/
	static PyObject* __py_assert(PyObject* self, PyObject* args);

	/**
		获取apps发布状态, 可在脚本中获取该值
	*/
	static PyObject* __py_getAppPublish(PyObject* self, PyObject* args);

	/**
		获取自定义配置参数。
		EntityApp与PythonApp暴露同一个KBEngine.getCustomCfg接口，这里只做入口转发，
		实际解析逻辑统一放在PythonApp中，避免baseapp/cellapp与其他PythonApp组件出现两套类型规则。
	*/
	static PyObject* __py_getCustomCfg(PyObject* self, PyObject* args);

	/**
		设置脚本输出类型前缀
	*/
	static PyObject* __py_setScriptLogType(PyObject* self, PyObject* args);

	/**
		获取watcher值
	*/
	static PyObject* __py_getWatcher(PyObject* self, PyObject* args);
	static PyObject* __py_getWatcherDir(PyObject* self, PyObject* args);

	/**
		重新导入所有的脚本
	*/
	virtual void reloadScript(bool fullReload);
	virtual void onReloadScript(bool fullReload);

	/**
		刷新当前 EntityApp 内所有在线 Entity，并发出 onReload(fullReload) 回调。
		该函数会先更新 Entity 本身的 Python __class__，再刷新 EntityCall/EntityComponent，
		最后才调用脚本层 onReload，确保脚本收到回调时已经处于新类实现上。
	*/
	ReloadScriptEntityStats reloadScriptEntitiesAndNotify(bool fullReload);

	/**
		通过相对路径获取资源的全路径
	*/
	static PyObject* __py_getResFullPath(PyObject* self, PyObject* args);

	/**
		通过相对路径判断资源是否存在
	*/
	static PyObject* __py_hasRes(PyObject* self, PyObject* args);

	/**
		open文件
	*/
	static PyObject* __py_kbeOpen(PyObject* self, PyObject* args);

	/**
		列出目录下所有文件
	*/
	static PyObject* __py_listPathRes(PyObject* self, PyObject* args);

	/**
		匹配相对路径获得全路径
	*/
	static PyObject* __py_matchPath(PyObject* self, PyObject* args);

	/**
		更新负载情况
	*/
	int tickPassedPercent(uint64 curr = timestamp());
	float getLoad() const { return load_; }
	void updateLoad();
	virtual void onUpdateLoad(){}
	virtual void calcLoad(float spareTime);
	uint64 checkTickPeriod();

protected:
	/**
		调用单个 Entity 的 onReload(fullReload)。
		fullReload=true 表示完整 reload，fullReload=false 表示逻辑 reload。
		没有 onReload 方法会静默跳过，不要求所有 Entity 实现。
	*/
	bool callEntityOnReload(E* entity, bool fullReload);

	KBEngine::script::Script								script_;
	std::vector<PyTypeObject*>								scriptBaseTypes_;

	PyObjectPtr												entryScript_;
	std::vector<PyObject*>									pluginEntryScripts_;

	EntityIDClient											idClient_;

	// 存储所有的entity的容器
	Entities<E>*											pEntities_;

	TimerHandle												gameTimer_;

	// globalData
	GlobalDataClient*										pGlobalData_;

	PY_CALLBACKMGR											pyCallbackMgr_;

	uint64													lastTimestamp_;

	// 进程当前负载
	float													load_;
};


template<class E>
EntityApp<E>::EntityApp(Network::EventDispatcher& dispatcher,
					 Network::NetworkInterface& ninterface,
					 COMPONENT_TYPE componentType,
					 COMPONENT_ID componentID):
ServerApp(dispatcher, ninterface, componentType, componentID),
script_(),
scriptBaseTypes_(),
entryScript_(),
idClient_(),
pEntities_(NULL),
gameTimer_(),
pGlobalData_(NULL),
pyCallbackMgr_(),
lastTimestamp_(timestamp()),
load_(0.f)
{
	ScriptTimers::initialize(*this);
	idClient_.pApp(this);

	// 初始化EntityDef模块获取entity实体函数地址
	EntityDef::setGetEntityFunc(std::bind(&EntityApp<E>::tryGetEntity, this,
		std::placeholders::_1, std::placeholders::_2));

	// 初始化entityCall模块获取channel函数地址
	EntityCallAbstract::setFindChannelFunc(std::bind(&EntityApp<E>::findChannelByEntityCall, this,
		std::placeholders::_1));
}

template<class E>
EntityApp<E>::~EntityApp()
{
}

template<class E>
bool EntityApp<E>::inInitialize()
{
	if(!installPyScript())
		return false;

	if(!installPyModules())
		return false;

	return installEntityDef();
}

template<class E>
bool EntityApp<E>::initialize()
{
	bool ret = ServerApp::initialize();
	if(ret)
	{
		gameTimer_ = this->dispatcher().addTimer(1000000 / g_kbeSrvConfig.gameUpdateHertz(), this,
								reinterpret_cast<void *>(TIMEOUT_GAME_TICK));

		// EntityApp类组件在主线程安装底层dispatcher timer，用来周期性推进协程。
		ret = AsyncioHelper::installTimer(this->dispatcher());
	}

	lastTimestamp_ = timestamp();
	return ret;
}

template<class E>
bool EntityApp<E>::initializeWatcher()
{
	WATCH_OBJECT("entitiesSize", this, &EntityApp<E>::entitiesSize);
	return ServerApp::initializeWatcher();
}

template<class E>
void EntityApp<E>::finalise(void)
{
	dispatchPluginEvent("onFini");
	uninstallPluginModules();

	// 先关闭asyncio，取消未完成协程，避免实体和组件卸载时仍被Task持有。
	AsyncioHelper::shutdown();

	// 再取消游戏timer并继续原有实体清理流程。
	gameTimer_.cancel();

	WATCH_FINALIZE;

	pyCallbackMgr_.finalise();
	ScriptTimers::finalise(*this);

	if(pEntities_)
		pEntities_->finalise();

	uninstallPyScript();

	ServerApp::finalise();
}

template<class E>
bool EntityApp<E>::installEntityDef()
{
	EntityDef::entityAliasID(ServerConfig::getSingleton().getCellApp().aliasEntityID);
	EntityDef::entitydefAliasID(ServerConfig::getSingleton().getCellApp().entitydefAliasID);

	if(!EntityDef::installScript(this->getScript().getModule()))
		return false;

	// 初始化所有扩展模块
	// assets/scripts/
	if(!EntityDef::initialize(scriptBaseTypes_, componentType_)){
		return false;
	}

	ENGINE_COMPONENT_INFO& dbcfg = g_kbeSrvConfig.getDBMgr();
	ScriptDefModule* pModule = EntityDef::findScriptModule(dbcfg.dbAccountEntityScriptType);
	if(pModule == NULL)
	{
		ERROR_MSG(fmt::format("EntityApp::installEntityDef(): not found account script[{}], defined(kbengine[_defs].xml->dbmgr->account_system->accountEntityScriptType and entities.xml)!\n",
			dbcfg.dbAccountEntityScriptType));

		return false;
	}

	return true;
}

template<class E>
int EntityApp<E>::registerPyObjectToScript(const char* attrName, PyObject* pyObj)
{
	return script_.registerToModule(attrName, pyObj);
}

template<class E>
int EntityApp<E>::unregisterPyObjectToScript(const char* attrName)
{
	return script_.unregisterToModule(attrName);
}

template<class E>
bool EntityApp<E>::installPyScript()
{
	if (Resmgr::getSingleton().respaths().size() <= 0 ||
		Resmgr::getSingleton().getPyUserResPath().size() == 0 ||
		Resmgr::getSingleton().getPySysResPath().size() == 0 ||
		Resmgr::getSingleton().getPyUserScriptsPath().size() == 0)
	{
		KBE_ASSERT(false && "EntityApp::installPyScript: KBE_RES_PATH error!\n");
		return false;
	}

	std::pair<std::wstring, std::wstring> pyPaths = getComponentPythonPaths(g_componentType);
	if (pyPaths.first.size() == 0)
	{
		KBE_ASSERT(false && "EntityApp::installPyScript: KBE_RES_PATH error[char2wchar]!\n");
		return false;
	}

	return getScript().install(pyPaths.first.c_str(), pyPaths.second, "KBEngine", componentType_);
}

template<class E>
void EntityApp<E>::registerScript(PyTypeObject* pto)
{
	scriptBaseTypes_.push_back(pto);
}

template<class E>
bool EntityApp<E>::uninstallPyScript()
{
	return uninstallPyModules() && getScript().uninstall();
}

template<class E>
bool EntityApp<E>::installPyModules()
{
	Entities<E>::installScript(NULL);
	EntityGarbages<E>::installScript(NULL);
	//Entity::installScript(g_script.getModule());

	pEntities_ = new Entities<E>();
	registerPyObjectToScript("entities", pEntities_);

	// 添加pywatcher支持
	if(!initializePyWatcher(&this->getScript()))
		return false;

	// 添加globalData, globalBases支持
	pGlobalData_ = new GlobalDataClient(DBMGR_TYPE, GlobalDataServer::GLOBAL_DATA);
	registerPyObjectToScript("globalData", pGlobalData_);

	// 注册创建entity的方法到py
	// 允许assert底层，用于调试脚本某个时机时底层状态
	APPEND_SCRIPT_MODULE_METHOD(getScript().getModule(),	kbassert,			__py_assert,							METH_VARARGS,	0);

	// 向脚本注册app发布状态
	APPEND_SCRIPT_MODULE_METHOD(getScript().getModule(),	publish,			__py_getAppPublish,						METH_VARARGS,	0);

	// 获取自定义配置参数，只读访问server配置中的<customCfg>节点。
	APPEND_SCRIPT_MODULE_METHOD(getScript().getModule(),	getCustomCfg,		__py_getCustomCfg,						METH_VARARGS,	0);

	// 注册设置脚本输出类型
	APPEND_SCRIPT_MODULE_METHOD(getScript().getModule(),	scriptLogType,		__py_setScriptLogType,					METH_VARARGS,	0);

	// 获得资源全路径
	APPEND_SCRIPT_MODULE_METHOD(getScript().getModule(),	getResFullPath,		__py_getResFullPath,					METH_VARARGS,	0);

	// 是否存在某个资源
	APPEND_SCRIPT_MODULE_METHOD(getScript().getModule(),	hasRes,				__py_hasRes,							METH_VARARGS,	0);

	// 打开一个文件
	APPEND_SCRIPT_MODULE_METHOD(getScript().getModule(),	open,				__py_kbeOpen,							METH_VARARGS,	0);

	// 列出目录下所有文件
	APPEND_SCRIPT_MODULE_METHOD(getScript().getModule(),	listPathRes,		__py_listPathRes,						METH_VARARGS,	0);

	// 匹配相对路径获得全路径
	APPEND_SCRIPT_MODULE_METHOD(getScript().getModule(),	matchPath,			__py_matchPath,							METH_VARARGS,	0);

	// 获取watcher值
	APPEND_SCRIPT_MODULE_METHOD(getScript().getModule(),	getWatcher,			__py_getWatcher,						METH_VARARGS,	0);

	// 获取watcher目录
	APPEND_SCRIPT_MODULE_METHOD(getScript().getModule(),	getWatcherDir,		__py_getWatcherDir,						METH_VARARGS,	0);

	// debug追踪kbe封装的py对象计数
	APPEND_SCRIPT_MODULE_METHOD(getScript().getModule(),	debugTracing,		script::PyGC::__py_debugTracing,		METH_VARARGS,	0);

	if(PyModule_AddIntConstant(this->getScript().getModule(), "LOG_TYPE_NORMAL", log4cxx::ScriptLevel::SCRIPT_INT))
	{
		ERROR_MSG( "EntityApp::installPyModules: Unable to set KBEngine.LOG_TYPE_NORMAL.\n");
	}

	if(PyModule_AddIntConstant(this->getScript().getModule(), "LOG_TYPE_INFO", log4cxx::ScriptLevel::SCRIPT_INFO))
	{
		ERROR_MSG( "EntityApp::installPyModules: Unable to set KBEngine.LOG_TYPE_INFO.\n");
	}

	if(PyModule_AddIntConstant(this->getScript().getModule(), "LOG_TYPE_ERR", log4cxx::ScriptLevel::SCRIPT_ERR))
	{
		ERROR_MSG( "EntityApp::installPyModules: Unable to set KBEngine.LOG_TYPE_ERR.\n");
	}

	if(PyModule_AddIntConstant(this->getScript().getModule(), "LOG_TYPE_DBG", log4cxx::ScriptLevel::SCRIPT_DBG))
	{
		ERROR_MSG( "EntityApp::installPyModules: Unable to set KBEngine.LOG_TYPE_DBG.\n");
	}

	if(PyModule_AddIntConstant(this->getScript().getModule(), "LOG_TYPE_WAR", log4cxx::ScriptLevel::SCRIPT_WAR))
	{
		ERROR_MSG( "EntityApp::installPyModules: Unable to set KBEngine.LOG_TYPE_WAR.\n");
	}

	if(PyModule_AddIntConstant(this->getScript().getModule(), "NEXT_ONLY", KBE_NEXT_ONLY))
	{
		ERROR_MSG( "EntityApp::installPyModules: Unable to set KBEngine.NEXT_ONLY.\n");
	}

	for(int i = 0; i < SERVER_ERR_MAX; i++)
	{
		if(PyModule_AddIntConstant(getScript().getModule(), SERVER_ERR_STR[i], i))
		{
			ERROR_MSG( fmt::format("EntityApp::installPyModules: Unable to set KBEngine.{}.\n", SERVER_ERR_STR[i]));
		}
	}

	// 安装入口模块
	std::string entryScriptFileName = "";
	if (componentType() == BASEAPP_TYPE)
	{
		ENGINE_COMPONENT_INFO& info = g_kbeSrvConfig.getBaseApp();
		entryScriptFileName = info.entryScriptFile;
	}
	else if (componentType() == CELLAPP_TYPE)
	{
		ENGINE_COMPONENT_INFO& info = g_kbeSrvConfig.getCellApp();
		entryScriptFileName = info.entryScriptFile;
	}

	if(entryScriptFileName.size() > 0)
	{
		PyObject *pyEntryScriptFileName = PyUnicode_FromString(entryScriptFileName.c_str());
		entryScript_ = PyImport_Import(pyEntryScriptFileName);

		if (PyErr_Occurred())
		{
			INFO_MSG(fmt::format("EntityApp::installPyModules: importing scripts/{}{}.py...\n",
				(componentType() == BASEAPP_TYPE ? "base/" : "cell/"),
				entryScriptFileName));

			PyErr_PrintEx(0);
		}

		S_RELEASE(pyEntryScriptFileName);

		if(entryScript_.get() == NULL)
		{
			return false;
		}
	}

	if (!installPluginModules())
		return false;

	onInstallPyModules();
	return true;
}

template<class E>
bool EntityApp<E>::installPluginModules()
{
	// EntityApp 覆盖 baseapp/cellapp。
	// 这里沿用组件自身的安装流程：主 entry 导入完成后，再导入当前组件声明的插件 entry。
	if (!pluginEntryScripts_.empty())
		return true;

	PyObject* importlibUtil = NULL;
	const std::vector<PluginDescriptor>& plugins = PluginManager::instance().plugins();
	for (std::vector<PluginDescriptor>::const_iterator iter = plugins.begin(); iter != plugins.end(); ++iter)
	{
		std::map<COMPONENT_TYPE, PluginComponentDescriptor>::const_iterator componentIter = iter->components.find(componentType());
		if (componentIter == iter->components.end() || componentIter->second.entry.empty())
			continue;

		std::string entryPath = entityapp_plugins::entryPath(*iter, componentType(), componentIter->second.entry);
		if (!entityapp_plugins::pathExists(entryPath))
			continue;

		if (!importlibUtil)
		{
			importlibUtil = PyImport_ImportModule("importlib.util");
			if (!importlibUtil)
			{
				SCRIPT_ERROR_CHECK();
				return false;
			}
		}

		std::string moduleName = "_kbe_plugin_" + entityapp_plugins::safeModuleName(iter->name) + "_" +
			entityapp_plugins::safeModuleName(COMPONENT_NAME_EX(componentType())) + "_" + entityapp_plugins::safeModuleName(componentIter->second.entry);

		PyObject* spec = PyObject_CallMethod(importlibUtil,
			const_cast<char*>("spec_from_file_location"),
			const_cast<char*>("ss"),
			moduleName.c_str(),
			entryPath.c_str());

		if (!spec)
		{
			SCRIPT_ERROR_CHECK();
			Py_XDECREF(importlibUtil);
			return false;
		}

		PyObject* pyModule = PyObject_CallMethod(importlibUtil,
			const_cast<char*>("module_from_spec"),
			const_cast<char*>("O"),
			spec);

		if (!pyModule)
		{
			SCRIPT_ERROR_CHECK();
			Py_DECREF(spec);
			Py_XDECREF(importlibUtil);
			return false;
		}

		PyObject* loader = PyObject_GetAttrString(spec, "loader");
		PyObject* pyRet = loader ? PyObject_CallMethod(loader, const_cast<char*>("exec_module"), const_cast<char*>("O"), pyModule) : NULL;
		Py_XDECREF(loader);
		Py_DECREF(spec);

		if (!pyRet)
		{
			ERROR_MSG(fmt::format("EntityApp::installPluginModules: could not import [{}] for plugin [{}]\n",
				entryPath, iter->name));
			SCRIPT_ERROR_CHECK();
			Py_DECREF(pyModule);
			Py_XDECREF(importlibUtil);
			return false;
		}

		Py_DECREF(pyRet);
		pluginEntryScripts_.push_back(pyModule);
	}

	Py_XDECREF(importlibUtil);
	return true;
}

template<class E>
void EntityApp<E>::dispatchPluginEvent(const std::string& eventName)
{
	for (std::vector<PyObject*>::iterator iter = pluginEntryScripts_.begin(); iter != pluginEntryScripts_.end(); ++iter)
		entityapp_plugins::callEntry(*iter, eventName, "", false);
}

template<class E>
void EntityApp<E>::dispatchPluginEvent(const std::string& eventName, bool arg)
{
	for (std::vector<PyObject*>::iterator iter = pluginEntryScripts_.begin(); iter != pluginEntryScripts_.end(); ++iter)
		entityapp_plugins::callEntry(*iter, eventName, "i", arg);
}

template<class E>
void EntityApp<E>::uninstallPluginModules()
{
	for (std::vector<PyObject*>::iterator iter = pluginEntryScripts_.begin(); iter != pluginEntryScripts_.end(); ++iter)
		Py_XDECREF(*iter);

	pluginEntryScripts_.clear();
}

template<class E>
bool EntityApp<E>::uninstallPyModules()
{
	// script::PyGC::set_debug(script::PyGC::DEBUG_STATS|script::PyGC::DEBUG_LEAK);
	// script::PyGC::collect();
	unregisterPyObjectToScript("globalData");
	S_RELEASE(pGlobalData_);

	S_RELEASE(pEntities_);
	unregisterPyObjectToScript("entities");

	Entities<E>::uninstallScript();
	EntityGarbages<E>::uninstallScript();
	//Entity::uninstallScript();
	EntityDef::uninstallScript();

	script::PyGC::debugTracing();
	return true;
}

template<class E>
E* EntityApp<E>::createEntity(const char* entityType, PyObject* params,
										 bool isInitializeScript, ENTITY_ID eid, bool initProperty)
{
	// 检查ID是否足够, 不足返回NULL
	if(eid <= 0 && idClient_.size() == 0)
	{
		PyErr_SetString(PyExc_SystemError, "EntityApp::createEntity: is Failed. not enough entityIDs.");
		PyErr_PrintEx(0);
		return NULL;
	}

	ScriptDefModule* sm = EntityDef::findScriptModule(entityType);
	if(sm == NULL)
	{
		PyErr_Format(PyExc_TypeError, "EntityApp::createEntity: entityType [%s] not found! Please register in entities.xml and implement a %s.def and %s.py\n",
			entityType, entityType, entityType);

		PyErr_PrintEx(0);
		return NULL;
	}
	else if(componentType_ == CELLAPP_TYPE ? !sm->hasCell() : !sm->hasBase())
	{
		PyErr_Format(PyExc_TypeError, "EntityApp::createEntity: cannot create %s(%s=false)! Please check the setting of the entities.xml and the implementation of %s.py\n",
			entityType,
			(componentType_ == CELLAPP_TYPE ? "hasCell()" : "hasBase()"),
			entityType);

		PyErr_PrintEx(0);
		return NULL;
	}

	PyObject* obj = sm->createObject();

	// 判断是否要分配一个新的id
	ENTITY_ID id = eid;
	if(id <= 0)
		id = idClient_.alloc();

	EntityDef::context().currEntityID = id;

	E* entity = onCreateEntity(obj, sm, id);

	if(initProperty)
		entity->initProperty();

	// 将entity加入entities
	pEntities_->add(id, entity);

	// 初始化脚本
	if(isInitializeScript)
		entity->initializeEntity(params);

	SCRIPT_ERROR_CHECK();

	if(g_debugEntity)
	{
		INFO_MSG(fmt::format("EntityApp::createEntity: new {} ({}) refc={}.\n", entityType, id, obj->ob_refcnt));
	}
	else
	{
		INFO_MSG(fmt::format("EntityApp::createEntity: new {0} {1}\n", entityType, id));
	}

	return entity;
}

template<class E>
E* EntityApp<E>::onCreateEntity(PyObject* pyEntity, ScriptDefModule* sm, ENTITY_ID eid)
{
	// 执行Entity的构造函数
	return new(pyEntity) E(eid, sm);
}

template<class E>
void EntityApp<E>::onSignalled(int sigNum)
{
	this->ServerApp::onSignalled(sigNum);

	switch (sigNum)
	{
	case SIGQUIT:
		CRITICAL_MSG(fmt::format("Received QUIT signal. This is likely caused by the "
				"{}Mgr killing this {} because it has been "
					"unresponsive for too long. Look at the callstack from "
					"the core dump to find the likely cause.\n",
				COMPONENT_NAME_EX(componentType_),
				COMPONENT_NAME_EX(componentType_)));

		break;
	default:
		break;
	}
}

template<class E>
PyObject* EntityApp<E>::tryGetEntity(COMPONENT_ID componentID, ENTITY_ID eid)
{
	if(componentID != componentID_)
		return NULL;

	E* entity = pEntities_->find(eid);
	if(entity == NULL){
		ERROR_MSG(fmt::format("EntityApp::tryGetEntity: can't found entity:{}.\n", eid));
		return NULL;
	}

	return entity;
}

template<class E>
Network::Channel* EntityApp<E>::findChannelByEntityCall(EntityCallAbstract& entityCall)
{
	// 如果组件ID大于0则查找组件
	if(entityCall.componentID() > 0)
	{
		Components::ComponentInfos* cinfos =
			Components::getSingleton().findComponent(entityCall.componentID());

		if(cinfos != NULL && cinfos->pChannel != NULL)
			return cinfos->pChannel;
	}
	else
	{
		return Components::getSingleton().pNetworkInterface()->findChannel(entityCall.addr());
	}

	return NULL;
}

template<class E>
void EntityApp<E>::handleTimeout(TimerHandle handle, void * arg)
{
	switch (reinterpret_cast<uintptr>(arg))
	{
		case TIMEOUT_GAME_TICK:
			this->handleGameTick();
			break;
		default:
			break;
	}

	ServerApp::handleTimeout(handle, arg);
}

template<class E>
void EntityApp<E>::handleGameTick()
{
	// time_t t = ::time(NULL);
	// DEBUG_MSG(fmt::format("EntityApp::handleGameTick[{}]:{}\n", t, g_kbetime));

	++g_kbetime;
	threadPool_.onMainThreadTick();
	handleTimers();

	{
		networkInterface().processChannels(KBEngine::Network::MessageHandlers::pMainMessageHandlers);
	}
}

template<class E>
bool EntityApp<E>::destroyEntity(ENTITY_ID entityID, bool callScript)
{
	PyObjectPtr entity = pEntities_->erase(entityID);
	if(entity != NULL)
	{
		static_cast<E*>(entity.get())->destroy(callScript);
		return true;
	}

	ERROR_MSG(fmt::format("EntityApp::destroyEntity: not found {}!\n", entityID));
	return false;
}

template<class E>
E* EntityApp<E>::findEntity(ENTITY_ID entityID)
{
	AUTO_SCOPED_PROFILE("findEntity");
	return pEntities_->find(entityID);
}

template<class E>
void EntityApp<E>::onReqAllocEntityID(Network::Channel* pChannel, ENTITY_ID startID, ENTITY_ID endID)
{
	if(pChannel->isExternal())
		return;

	// INFO_MSG("EntityApp::onReqAllocEntityID: entityID alloc(%d-%d).\n", startID, endID);
	idClient_.onAddRange(startID, endID);
}

template<class E>
PyObject* EntityApp<E>::__py_assert(PyObject* self, PyObject* args)
{
	KBE_ASSERT(false && "kbassert");
	return NULL;
}

template<class E>
PyObject* EntityApp<E>::__py_getAppPublish(PyObject* self, PyObject* args)
{
	return PyLong_FromLong(g_appPublish);
}

template<class E>
PyObject* EntityApp<E>::__py_getCustomCfg(PyObject* self, PyObject* args)
{
	// EntityApp组件复用PythonApp的实现，保证所有服务端组件读取customCfg时的缺省值、类型转换和错误处理完全一致。
	return PythonApp::__py_getCustomCfg(self, args);
}

template<class E>
PyObject* EntityApp<E>::__py_getWatcher(PyObject* self, PyObject* args)
{
	Py_ssize_t argCount = PyTuple_Size(args);
	if(argCount != 1)
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::getWatcher(): args[strpath] error!");
		PyErr_PrintEx(0);
		return 0;
	}

	char* path;

	if(!PyArg_ParseTuple(args, "s", &path))
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::getWatcher(): args[strpath] error!");
		PyErr_PrintEx(0);
		return 0;
	}

	//DebugHelper::getSingleton().setScriptMsgType(type);

	KBEShared_ptr< WatcherObject > pWobj = WatcherPaths::root().getWatcher(path);
	if(pWobj.get() == NULL)
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::getWatcher(): not found watcher[%s]!", path);
		PyErr_PrintEx(0);
		return 0;
	}

	WATCHER_VALUE_TYPE wtype = pWobj->getType();
	PyObject* pyval = NULL;
	MemoryStream* stream = MemoryStream::createPoolObject(OBJECTPOOL_POINT);
	pWobj->addToStream(stream);
	WATCHER_ID id;
	(*stream) >> id;

	switch(wtype)
	{
	case WATCHER_VALUE_TYPE_UINT8:
		{
			uint8 v;
			(*stream) >> v;
			pyval = PyLong_FromUnsignedLong(v);
		}
		break;
	case WATCHER_VALUE_TYPE_UINT16:
		{
			uint16 v;
			(*stream) >> v;
			pyval = PyLong_FromUnsignedLong(v);
		}
		break;
	case WATCHER_VALUE_TYPE_UINT32:
		{
			uint32 v;
			(*stream) >> v;
			pyval = PyLong_FromUnsignedLong(v);
		}
		break;
	case WATCHER_VALUE_TYPE_UINT64:
		{
			uint64 v;
			(*stream) >> v;
			pyval = PyLong_FromUnsignedLongLong(v);
		}
		break;
	case WATCHER_VALUE_TYPE_INT8:
		{
			int8 v;
			(*stream) >> v;
			pyval = PyLong_FromLong(v);
		}
		break;
	case WATCHER_VALUE_TYPE_INT16:
		{
			int16 v;
			(*stream) >> v;
			pyval = PyLong_FromLong(v);
		}
		break;
	case WATCHER_VALUE_TYPE_INT32:
		{
			int32 v;
			(*stream) >> v;
			pyval = PyLong_FromLong(v);
		}
		break;
	case WATCHER_VALUE_TYPE_INT64:
		{
			int64 v;
			(*stream) >> v;
			pyval = PyLong_FromLongLong(v);
		}
		break;
	case WATCHER_VALUE_TYPE_FLOAT:
		{
			float v;
			(*stream) >> v;
			pyval = PyLong_FromDouble(v);
		}
		break;
	case WATCHER_VALUE_TYPE_DOUBLE:
		{
			double v;
			(*stream) >> v;
			pyval = PyLong_FromDouble(v);
		}
		break;
	case WATCHER_VALUE_TYPE_CHAR:
	case WATCHER_VALUE_TYPE_STRING:
		{
			std::string v;
			(*stream) >> v;
			pyval = PyUnicode_FromString(v.c_str());
		}
		break;
	case WATCHER_VALUE_TYPE_BOOL:
		{
			bool v;
			(*stream) >> v;
			pyval = PyBool_FromLong(v);
		}
		break;
	case WATCHER_VALUE_TYPE_COMPONENT_TYPE:
		{
			COMPONENT_TYPE v;
			(*stream) >> v;
			pyval = PyBool_FromLong(v);
		}
		break;
	default:
		KBE_ASSERT(false && "no support!\n");
	};

	MemoryStream::reclaimPoolObject(stream);
	return pyval;
}

template<class E>
PyObject* EntityApp<E>::__py_getWatcherDir(PyObject* self, PyObject* args)
{
	Py_ssize_t argCount = PyTuple_Size(args);
	if(argCount != 1)
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::getWatcherDir(): args[strpath] error!");
		PyErr_PrintEx(0);
		return 0;
	}

	char* path;

	if(!PyArg_ParseTuple(args, "s", &path))
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::getWatcherDir(): args[strpath] error!");
		PyErr_PrintEx(0);
		return 0;
	}

	std::vector<std::string> vec;
	WatcherPaths::root().dirPath(path, vec);

	PyObject* pyTuple = PyTuple_New(static_cast<Py_ssize_t>(vec.size()));
	std::vector<std::string>::iterator iter = vec.begin();
	Py_ssize_t i = 0;
	for(; iter != vec.end(); ++iter)
	{
		PyTuple_SET_ITEM(pyTuple, i++, PyUnicode_FromString((*iter).c_str()));
	}

	return pyTuple;
}

template<class E>
PyObject* EntityApp<E>::__py_setScriptLogType(PyObject* self, PyObject* args)
{
	Py_ssize_t argCount = PyTuple_Size(args);
	if(argCount != 1)
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::scriptLogType(): args error!");
		PyErr_PrintEx(0);
		return 0;
	}

	int type = -1;

	if(!PyArg_ParseTuple(args, "i", &type))
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::scriptLogType(): args error!");
		PyErr_PrintEx(0);
		return 0;
	}

	DebugHelper::getSingleton().setScriptMsgType(type);
	S_Return;
}

template<class E>
PyObject* EntityApp<E>::__py_getResFullPath(PyObject* self, PyObject* args)
{
	Py_ssize_t argCount = PyTuple_Size(args);
	if(argCount != 1)
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::getResFullPath(): args error!");
		PyErr_PrintEx(0);
		return 0;
	}

	char* respath = NULL;

	if(!PyArg_ParseTuple(args, "s", &respath))
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::getResFullPath(): args error!");
		PyErr_PrintEx(0);
		return 0;
	}

	if(!Resmgr::getSingleton().hasRes(respath))
		return PyUnicode_FromString("");

	std::string fullpath = Resmgr::getSingleton().matchRes(respath);
	return PyUnicode_FromString(fullpath.c_str());
}

template<class E>
PyObject* EntityApp<E>::__py_hasRes(PyObject* self, PyObject* args)
{
	Py_ssize_t argCount = PyTuple_Size(args);
	if(argCount != 1)
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::hasRes(): args error!");
		PyErr_PrintEx(0);
		return 0;
	}

	char* respath = NULL;

	if(!PyArg_ParseTuple(args, "s", &respath))
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::hasRes(): args error!");
		PyErr_PrintEx(0);
		return 0;
	}

	return PyBool_FromLong(Resmgr::getSingleton().hasRes(respath));
}

template<class E>
PyObject* EntityApp<E>::__py_kbeOpen(PyObject* self, PyObject* args)
{
	Py_ssize_t argCount = PyTuple_Size(args);
	if (argCount < 1)
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::open(): args error!");
		PyErr_PrintEx(0);
		return 0;
	}

	char* respath = NULL;
	char* fargs = NULL;
	char* encodingArg = NULL;

	if (!PyArg_ParseTuple(args, "s|ss", &respath, &fargs, &encodingArg))
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::open(): args error!");
		PyErr_PrintEx(0);
		return 0;
	}

	std::string sfullpath = Resmgr::getSingleton().matchRes(respath);

	PyObject *ioMod = PyImport_ImportModule("io");

	// SCOPED_PROFILE(SCRIPTCALL_PROFILE);
	PyObject *openedFile = NULL;
	if (argCount > 1)
	{
		openedFile = PyObject_CallMethod(ioMod, const_cast<char*>("open"),
			const_cast<char*>("ssis"),
			const_cast<char*>(sfullpath.c_str()),
			fargs,
			-1,
			encodingArg);
	}
	else
	{
		openedFile = PyObject_CallMethod(ioMod, const_cast<char*>("open"),
			const_cast<char*>("s"),
			const_cast<char*>(sfullpath.c_str()));
	}

	Py_DECREF(ioMod);

	if (openedFile == NULL)
	{
		SCRIPT_ERROR_CHECK();
	}

	return openedFile;
}

template<class E>
PyObject* EntityApp<E>::__py_matchPath(PyObject* self, PyObject* args)
{
	Py_ssize_t argCount = PyTuple_Size(args);
	if(argCount != 1)
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::matchPath(): args error!");
		PyErr_PrintEx(0);
		return 0;
	}

	char* respath = NULL;

	if(!PyArg_ParseTuple(args, "s", &respath))
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::matchPath(): args error!");
		PyErr_PrintEx(0);
		return 0;
	}

	std::string path = Resmgr::getSingleton().matchPath(respath);
	return PyUnicode_FromStringAndSize(path.c_str(), path.size());
}

template<class E>
PyObject* EntityApp<E>::__py_listPathRes(PyObject* self, PyObject* args)
{
	Py_ssize_t argCount = PyTuple_Size(args);
	if(argCount < 1 || argCount > 2)
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::listPathRes(): args[path, pathargs=\'*.*\'] error!");
		PyErr_PrintEx(0);
		return 0;
	}

	std::wstring wExtendName = L"*";
	PyObject* pathobj = NULL;
	PyObject* path_argsobj = NULL;

	if(argCount == 1)
	{
		if(!PyArg_ParseTuple(args, "O", &pathobj))
		{
			PyErr_Format(PyExc_TypeError, "KBEngine::listPathRes(): args[path] error!");
			PyErr_PrintEx(0);
			return 0;
		}
	}
	else
	{
		if(!PyArg_ParseTuple(args, "O|O", &pathobj, &path_argsobj))
		{
			PyErr_Format(PyExc_TypeError, "KBEngine::listPathRes(): args[path, pathargs=\'*.*\'] error!");
			PyErr_PrintEx(0);
			return 0;
		}

		if(PyUnicode_Check(path_argsobj))
		{
			wchar_t* fargs = NULL;
			fargs = PyUnicode_AsWideCharString(path_argsobj, NULL);
			wExtendName = fargs;
			PyMem_Free(fargs);
		}
		else
		{
			if(PySequence_Check(path_argsobj))
			{
				wExtendName = L"";
				Py_ssize_t size = PySequence_Size(path_argsobj);
				for(int i=0; i<size; ++i)
				{
					PyObject* pyobj = PySequence_GetItem(path_argsobj, i);
					if(!PyUnicode_Check(pyobj))
					{
						PyErr_Format(PyExc_TypeError, "KBEngine::listPathRes(): args[path, pathargs=\'*.*\'] error!");
						PyErr_PrintEx(0);
						return 0;
					}

					wchar_t* wtemp = NULL;
					wtemp = PyUnicode_AsWideCharString(pyobj, NULL);
					wExtendName += wtemp;
					wExtendName += L"|";
					PyMem_Free(wtemp);
				}
			}
			else
			{
				PyErr_Format(PyExc_TypeError, "KBEngine::listPathRes(): args[pathargs] error!");
				PyErr_PrintEx(0);
				return 0;
			}
		}
	}

	if(!PyUnicode_Check(pathobj))
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::listPathRes(): args[path] error!");
		PyErr_PrintEx(0);
		return 0;
	}

	if(PyUnicode_GET_LENGTH(pathobj) == 0)
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::listPathRes(): args[path] is NULL!");
		PyErr_PrintEx(0);
		return 0;
	}

	if(wExtendName.size() == 0)
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::listPathRes(): args[pathargs] is NULL!");
		PyErr_PrintEx(0);
		return 0;
	}

	if(wExtendName[0] == '.')
		wExtendName.erase(wExtendName.begin());

	if(wExtendName.size() == 0)
		wExtendName = L"*";

	wchar_t* respath = PyUnicode_AsWideCharString(pathobj, NULL);
	if(respath == NULL)
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::listPathRes(): args[path] is NULL!");
		PyErr_PrintEx(0);
		return 0;
	}

	char* cpath = strutil::wchar2char(respath);
	std::string foundPath = Resmgr::getSingleton().matchPath(cpath);
	free(cpath);
	PyMem_Free(respath);

	respath = strutil::char2wchar(foundPath.c_str());

	std::vector<std::wstring> results;
	Resmgr::getSingleton().listPathRes(respath, wExtendName, results);
	PyObject* pyresults = PyTuple_New(static_cast<Py_ssize_t>(results.size()));

	std::vector<std::wstring>::iterator iter = results.begin();
	int i = 0;

	for(; iter != results.end(); ++iter)
	{
		PyTuple_SET_ITEM(pyresults, i++, PyUnicode_FromWideChar((*iter).c_str(), (*iter).size()));
	}

	free(respath);
	return pyresults;
}

template<class E>
void EntityApp<E>::startProfile_(Network::Channel* pChannel, std::string profileName, int8 profileType, uint32 timelen)
{
	if(pChannel->isExternal())
		return;

	switch(profileType)
	{
	case 0:	// pyprofile
		new PyProfileHandler(this->networkInterface(), timelen, profileName, pChannel->addr());
		return;
	default:
		break;
	};

	ServerApp::startProfile_(pChannel, profileName, profileType, timelen);
}

template<class E>
void EntityApp<E>::onDbmgrInitCompleted(Network::Channel* pChannel,
						GAME_TIME gametime, ENTITY_ID startID, ENTITY_ID endID,
						COMPONENT_ORDER startGlobalOrder, COMPONENT_ORDER startGroupOrder,
						const std::string& digest)
{
	if(pChannel->isExternal())
		return;

	INFO_MSG(fmt::format("EntityApp::onDbmgrInitCompleted: entityID alloc({}-{}), startGlobalOrder={}, startGroupOrder={}, digest={}.\n",
		startID, endID, startGlobalOrder, startGroupOrder, digest));

	startGlobalOrder_ = startGlobalOrder;
	startGroupOrder_ = startGroupOrder;
	g_componentGlobalOrder = startGlobalOrder;
	g_componentGroupOrder = startGroupOrder;

	idClient_.onAddRange(startID, endID);
	g_kbetime = gametime;

	setEvns();

	if(digest != EntityDef::md5().getDigestStr())
	{
		ERROR_MSG(fmt::format("EntityApp::onDbmgrInitCompleted: digest not match. curr({}) != dbmgr({}, addr={})\n",
			EntityDef::md5().getDigestStr(), digest, pChannel->c_str()));

		this->shutDown();
	}
}

template<class E>
void EntityApp<E>::onBroadcastGlobalDataChanged(Network::Channel* pChannel, KBEngine::MemoryStream& s)
{
	if(pChannel->isExternal())
		return;

	std::string key, value;
	bool isDelete;

	s >> isDelete;
	s.readBlob(key);

	if(!isDelete)
	{
		s.readBlob(value);
	}

	PyObject * pyKey = script::Pickler::unpickle(key);
	if(pyKey == NULL)
	{
		ERROR_MSG("EntityApp::onBroadcastGlobalDataChanged: no has key!\n");
		return;
	}

	if(isDelete)
	{
		if(pGlobalData_->del(pyKey))
		{
			// 通知脚本
			// SCOPED_PROFILE(SCRIPTCALL_PROFILE);
			SCRIPT_OBJECT_CALL_ARGS1(getEntryScript().get(), const_cast<char*>("onGlobalDataDel"),
				const_cast<char*>("O"), pyKey, false);
		}
	}
	else
	{
		PyObject * pyValue = script::Pickler::unpickle(value);
		if(pyValue == NULL)
		{
			ERROR_MSG("EntityApp::onBroadcastGlobalDataChanged: no has value!\n");
			Py_DECREF(pyKey);
			return;
		}

		if(pGlobalData_->write(pyKey, pyValue))
		{
			// 通知脚本
			// SCOPED_PROFILE(SCRIPTCALL_PROFILE);
			SCRIPT_OBJECT_CALL_ARGS2(getEntryScript().get(), const_cast<char*>("onGlobalData"),
				const_cast<char*>("OO"), pyKey, pyValue, false);
		}

		Py_DECREF(pyValue);
	}

	Py_DECREF(pyKey);
}

template<class E>
void EntityApp<E>::onExecScriptCommand(Network::Channel* pChannel, KBEngine::MemoryStream& s)
{
	if(pChannel->isExternal())
		return;

	std::string cmd;
	s.readBlob(cmd);

	PyObject* pycmd = PyUnicode_DecodeUTF8(cmd.data(), cmd.size(), NULL);
	if(pycmd == NULL)
	{
		SCRIPT_ERROR_CHECK();
		return;
	}

	DEBUG_MSG(fmt::format("EntityApp::onExecScriptCommand: size({}), command={}.\n",
		cmd.size(), cmd));

	std::string retbuf = "";
	PyObject* pycmd1 = PyUnicode_AsEncodedString(pycmd, "utf-8", NULL);
	script_.run_simpleString(PyBytes_AsString(pycmd1), &retbuf);

	if(retbuf.size() == 0)
	{
		retbuf = "\r\n";
	}

	// 将结果返回给客户端
	Network::Bundle* pBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
	ConsoleInterface::ConsoleExecCommandCBMessageHandler msgHandler;
	(*pBundle).newMessage(msgHandler);
	ConsoleInterface::ConsoleExecCommandCBMessageHandlerArgs1::staticAddToBundle((*pBundle), retbuf);
	pChannel->send(pBundle);

	Py_DECREF(pycmd);
	Py_DECREF(pycmd1);
}

template<class E>
int EntityApp<E>::tickPassedPercent(uint64 curr)
{
	// 得到上一个tick到现在所流逝的时间
	uint64 pass_stamps = (curr - lastTimestamp_) * uint64(1000) / stampsPerSecond();

	// 得到每Hertz的毫秒数
	static int expected = (1000 / g_kbeSrvConfig.gameUpdateHertz());

	// 得到当前流逝的时间占一个时钟周期的的百分比
	return int(pass_stamps) * 100 / expected;
}

template<class E>
uint64 EntityApp<E>::checkTickPeriod()
{
	uint64 curr = timestamp();
	int percent = tickPassedPercent(curr);

	if (percent > 200)
	{
		WARNING_MSG(fmt::format("EntityApp::checkTickPeriod: tick took {0}% ({1:.2f} seconds)!\n",
					percent, (float(percent)/1000.f)));
	}

	uint64 elapsed = curr - lastTimestamp_;
	lastTimestamp_ = curr;
	return elapsed;
}


template<class E>
void EntityApp<E>::updateLoad()
{
	uint64 lastTickInStamps = checkTickPeriod();

	// 获得空闲时间比例
	double spareTime = 1.0;
	if (lastTickInStamps != 0)
	{
		spareTime = double(dispatcher_.getSpareTime()) / double(lastTickInStamps);
	}

	dispatcher_.clearSpareTime();

	// 如果空闲时间比例小于0 或者大于1则表明计时不准确
	if ((spareTime < 0.f) || (1.f < spareTime))
	{
		if (g_timingMethod == RDTSC_TIMING_METHOD)
		{
			CRITICAL_MSG(fmt::format("EntityApp::handleGameTick: "
						"Invalid timing result {:.3f}.\n"
						"Please change the environment variable KBE_TIMING_METHOD to [rdtsc|gettimeofday|gettime](curr = {1})!",
						spareTime, getTimingMethodName()));
		}
		else
		{
			CRITICAL_MSG(fmt::format("EntityApp::handleGameTick: Invalid timing result {:.3f}.\n",
					spareTime));
		}
	}

	// 文件触发热更：在 assets 目录下 touch/修改 .hotreload 文件即可触发所有 app 的 reloadScript
	// 仅在开发模式（publish=0）下生效；PyCharm 插件只需写入该文件即可
	if (g_appPublish == 0)
	{
		static enum { UNINIT, ABSENT, PRESENT } s_state = UNINIT;
		static uint64 s_stamp = 0;
		std::string hotreloadFile = Resmgr::getSingleton().getPyUserScriptsPath() + ".hotreload";

		struct stat st;
		if (stat(hotreloadFile.c_str(), &st) == 0)
		{
			uint64 stamp = ((uint64)st.st_mtime << 32) ^ (uint64)st.st_size;

			if ((s_state == ABSENT) || (s_state == PRESENT && stamp != s_stamp))
			{
				INFO_MSG(fmt::format("{}::updateLoad: .hotreload changed, triggering reloadScript.\n",
					COMPONENT_NAME_EX(g_componentType)));
				reloadScript(false);
			}

			s_stamp = stamp;
			s_state = PRESENT;
		}
		else
		{
			s_state = ABSENT;
		}
	}

	calcLoad((float)spareTime);
	onUpdateLoad();
}

template<class E>
void EntityApp<E>::calcLoad(float spareTime)
{
	// 负载的值为1.0 - 空闲时间比例, 必须在0-1.f之间
	float load = KBEClamp(1.f - spareTime, 0.f, 1.f);

	// 此处算法看server_operations_guide.pdf介绍loadSmoothingBias处
	// loadSmoothingBias 决定本次负载取最后一次负载的loadSmoothingBias剩余比例 + 当前负载的loadSmoothingBias比例
	static float loadSmoothingBias = g_kbeSrvConfig.getConfig().loadSmoothingBias;
	load_ = (1 - loadSmoothingBias) * load_ + loadSmoothingBias * load;
}

template<class E>
void EntityApp<E>::onReloadScript(bool fullReload)
{
	INFO_MSG(fmt::format("{}::onReloadScript: begin, fullReload={}.\n",
		COMPONENT_NAME_EX(g_componentType), fullReload));

	// EntityCall 持有脚本类型/方法描述相关缓存，Entity 脚本换类之后必须同步刷新，
	// 否则跨进程调用可能仍按旧的 method/property 描述打包。
	EntityCall::ENTITYCALLS::iterator iter = EntityCall::entityCalls.begin();
	for(; iter != EntityCall::entityCalls.end(); ++iter)
	{
		(*iter)->reload();
	}

	INFO_MSG(fmt::format("{}::onReloadScript: entitycalls reloaded, count={}.\n",
		COMPONENT_NAME_EX(g_componentType), EntityCall::entityCalls.size()));

	// 全局组件列表中既包含挂在 Entity 上的组件，也可能包含反序列化/临时引用过的组件。
	// 每个组件用 reloadGeneration_ 去重，所以即使 Entity::reload 过程中已经刷新过，
	// 这里再次遍历也不会重复初始化同一代热更。
	EntityComponent::ENTITY_COMPONENTS::iterator iter1 = EntityComponent::entity_components.begin();
	for (; iter1 != EntityComponent::entity_components.end(); ++iter1)
	{
		(*iter1)->reload(fullReload);
	}

	INFO_MSG(fmt::format("{}::onReloadScript: components reloaded, count={}.\n",
		COMPONENT_NAME_EX(g_componentType), EntityComponent::reloadCount()));
}

template<class E>
bool EntityApp<E>::callEntityOnReload(E* entity, bool fullReload)
{
	// 脚本层完全无侵入：没有 onReload 方法就静默跳过，不要求所有 Entity 实现该接口。
	PyObject* pyFunc = PyObject_GetAttrString(static_cast<PyObject*>(entity), "onReload");
	if (!pyFunc)
	{
		PyErr_Clear();
		return false;
	}

	if (!PyCallable_Check(pyFunc))
	{
		// 属性存在但不可调用通常是脚本写错，例如 onReload = 1。
		// 这里只告警，不中断整个热更流程。
		WARNING_MSG(fmt::format("{}::callEntityOnReload: {} {}.onReload is not callable.\n",
			COMPONENT_NAME_EX(g_componentType), entity->scriptName(), entity->id()));

		Py_DECREF(pyFunc);
		return false;
	}

	PyObject* pyResult = PyObject_CallFunction(pyFunc, const_cast<char*>("i"), fullReload ? 1 : 0);
	Py_DECREF(pyFunc);

	if (pyResult != NULL)
	{
		// 兼容同步函数和 coroutine：如果脚本返回 awaitable，由 AsyncioHelper 接管；
		// 如果是普通返回值，submitCoroutine 内部会按现有逻辑处理。
		AsyncioHelper::submitCoroutine(pyResult);
		Py_DECREF(pyResult);
	}
	else
	{
		SCRIPT_ERROR_CHECK();
	}

	return true;
}

template<class E>
ReloadScriptEntityStats EntityApp<E>::reloadScriptEntitiesAndNotify(bool fullReload)
{
	ReloadScriptEntityStats stats;

	typename Entities<E>::ENTITYS_MAP& entities = pEntities_->getEntities();
	INFO_MSG(fmt::format("{}::reloadScriptEntitiesAndNotify: begin, fullReload={}, entities={}.\n",
		COMPONENT_NAME_EX(g_componentType), fullReload, entities.size()));

	typename Entities<E>::ENTITYS_MAP::iterator eiter = entities.begin();
	for (; eiter != entities.end(); ++eiter)
	{
		// Entity::reload 会把在线 Entity 的 __class__ 切到新脚本类型。
		// 非 fullReload 时不会重置数据属性，只更新行为层；fullReload 才做属性差异补齐。
		static_cast<E*>(eiter->second.get())->reload(fullReload);
		++stats.entitiesReloaded;
	}

	INFO_MSG(fmt::format("{}::reloadScriptEntitiesAndNotify: entities reloaded, count={}.\n",
		COMPONENT_NAME_EX(g_componentType), stats.entitiesReloaded));

	EntityApp<E>::onReloadScript(fullReload);

	{
		// onReload 放在 Entity/Component/EntityCall 都刷新之后调用，
		// 这样脚本回调内访问自身方法、组件方法、EntityCall 都会尽量拿到新实现。
		// 生产环境也会触发，但 fullReload 在生产会被强制降级为 false。
		INFO_MSG(fmt::format("{}::reloadScriptEntitiesAndNotify: begin onReload callbacks, fullReload={}, publish={}.\n",
			COMPONENT_NAME_EX(g_componentType), fullReload, g_appPublish));

		eiter = entities.begin();
		for (; eiter != entities.end(); ++eiter)
		{
			if (callEntityOnReload(static_cast<E*>(eiter->second.get()), fullReload))
				++stats.onReloadCallbacks;
		}

		INFO_MSG(fmt::format("{}::reloadScriptEntitiesAndNotify: onReload callbacks done, count={}.\n",
			COMPONENT_NAME_EX(g_componentType), stats.onReloadCallbacks));
	}

	return stats;
}

template<class E>
void EntityApp<E>::reloadScript(bool fullReload)
{
	static bool isReloading = false;
	if (isReloading)
	{
		// reload 过程中脚本的 onInit、Timer 或 telnet 命令如果再次触发 reload，
		// 会和当前这轮 EntityDef/Entity/Timer 刷新交叉执行，容易产生半新半旧状态。
		// 这里直接拒绝重入，要求上一轮完整结束后再发起下一次热更。
		WARNING_MSG(fmt::format("{}::reloadScript: ignored reentrant reload request, fullReload={}.\n",
			COMPONENT_NAME_EX(g_componentType), fullReload));
		return;
	}

	isReloading = true;
	struct ReloadScriptGuard
	{
		bool& value;
		ReloadScriptGuard(bool& v) : value(v) {}
		~ReloadScriptGuard() { value = false; }
	} reloadScriptGuard(isReloading);

	if (g_appPublish != 0 && fullReload)
	{
		// 生产环境只允许逻辑层热更。即使运维误传 True 或脚本不传参数走默认值，
		// 这里也强制降级为 fullReload=false，避免线上触碰属性补齐、数据重建等高风险路径。
		WARNING_MSG(fmt::format("{}::reloadScript: production mode forces fullReload=false, requested fullReload=true.\n",
			COMPONENT_NAME_EX(g_componentType)));
		fullReload = false;
	}

	INFO_MSG(fmt::format("{}::reloadScript: begin, fullReload={}.\n",
		COMPONENT_NAME_EX(g_componentType), fullReload));

	// 先重载 EntityDef 及脚本模块。EntityDef 内部会先刷新当前组件目录下已加载的依赖模块，
	// 再加载 Entity/Component 主脚本，避免 interface/mixin 仍停留在旧实现。
	ReloadScriptDefStats defStats = EntityDef::reload(fullReload);

	INFO_MSG(fmt::format("{}::reloadScript: EntityDef::reload done, ok={}, changedFiles={}, skippedFiles={}, reloadedModules={}, duplicateModulePatches={}, staleAttrsKept={}.\n",
		COMPONENT_NAME_EX(g_componentType), defStats.ok, defStats.changedFiles, defStats.skippedFiles,
		defStats.reloadedModules, defStats.duplicateModulePatches, defStats.staleAttrsKept));

	if (!defStats.ok)
	{
		// 脚本模块 reload 已经失败时不能继续刷新在线对象，否则会把 Entity/Timer 推到半新半旧状态。
		ERROR_MSG(fmt::format("{}::reloadScript: aborted because EntityDef::reload failed.\n",
			COMPONENT_NAME_EX(g_componentType)));
		return;
	}

	if (defStats.changedFiles == 0)
	{
		// 没有任何脚本文件变化时，不刷新在线 Entity/Component/Timer，也不触发 onInit。
		// 这样空 reload 只作为一次检查，不会给线上对象制造额外扰动。
		INFO_MSG(fmt::format("{}::reloadScript: no changed script files, skip entity/component/timer/onInit refresh.\n",
			COMPONENT_NAME_EX(g_componentType)));
		return;
	}

	// 开启组件热更代次。组件可能从 Entity::reload 和全局组件遍历两条路径被访问，
	// 代次标记用于保证同一轮 reload 中每个组件只刷新一次。
	EntityComponent::beginReload();
	INFO_MSG(fmt::format("{}::reloadScript: EntityComponent::beginReload done.\n",
		COMPONENT_NAME_EX(g_componentType)));

	// 派生类 Baseapp/Cellapp 会在这里刷新在线 Entity，并触发 onReload(fullReload)。
	onReloadScript(fullReload);
	INFO_MSG(fmt::format("{}::reloadScript: onReloadScript done.\n",
		COMPONENT_NAME_EX(g_componentType)));

	// Timer 不重新创建，只把保存的 Python 回调对象重新定位到新脚本实现。
	ReloadScriptTimerStats timerStats = PythonApp::reloadScriptTimers();

	INFO_MSG(fmt::format("{}::reloadScript: fullReload={}, componentsReloaded={}, timersRefreshed={}, timersKeptOld={}\n",
		COMPONENT_NAME_EX(g_componentType), fullReload, EntityComponent::reloadCount(),
		timerStats.refreshed, timerStats.keptOld));

	for (std::vector<std::string>::const_iterator iter = timerStats.keptOldCallbacks.begin();
		iter != timerStats.keptOldCallbacks.end(); ++iter)
	{
		WARNING_MSG(fmt::format("{}::reloadScript: timer kept old callback: {}\n",
			COMPONENT_NAME_EX(g_componentType), (*iter)));
	}

	// SCOPED_PROFILE(SCRIPTCALL_PROFILE);

	// 所有脚本都加载完毕
	PyObject* pyResult = PyObject_CallMethod(getEntryScript().get(),
										const_cast<char*>("onInit"),
										const_cast<char*>("i"),
										1);

	if(pyResult != NULL) {
		AsyncioHelper::submitCoroutine(pyResult);
		Py_DECREF(pyResult);
	}
	else
		SCRIPT_ERROR_CHECK();

	uninstallPluginModules();
	if (installPluginModules())
		dispatchPluginEvent("onInit", true);
}

}

#endif // KBE_ENTITY_APP_H
