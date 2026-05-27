// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

#ifndef KBE_PYTHON_APP_H
#define KBE_PYTHON_APP_H

// common include
#include "pyscript/py_gc.h"
#include "pyscript/script.h"
#include "pyscript/pyprofile.h"
#include "pyscript/pyprofile_handler.h"
#include "common/common.h"
#include "common/timer.h"
#include "common/smartpointer.h"
#include "pyscript/pyobject_pointer.h"
#include "helper/debug_helper.h"
#include "helper/script_loglevel.h"
#include "helper/profile.h"
#include "server/serverconfig.h"
#include "network/message_handler.h"
#include "resmgr/resmgr.h"
#include "helper/console_helper.h"
#include "server/serverapp.h"
#include "server/script_timers.h"

#if KBE_PLATFORM == PLATFORM_WIN32
#pragma warning (disable : 4996)
#endif

	
namespace KBEngine{

/**
	脚本热更时刷新 Timer 回调的统计信息。
	Timer 本身仍然保留原来的 handle、触发时间和 userArg，只替换它保存的 Python 回调对象；
	refreshed 表示已经成功指向新脚本对象的回调数量，keptOld 表示无法安全解析新对象、继续沿用旧回调的数量。
*/
struct ReloadScriptTimerStats
{
	ReloadScriptTimerStats() :
		refreshed(0),
		keptOld(0)
	{
	}

	uint32 refreshed;
	uint32 keptOld;
	std::vector<std::string> keptOldCallbacks;
};

class PythonApp : public ServerApp
{
public:
	enum TimeOutType
	{
		TIMEOUT_GAME_TICK = TIMEOUT_SERVERAPP_MAX + 1,

		// 这个必须放在最后面，表示当前最大的枚举值是多少
		TIMEOUT_PYTHONAPP_MAX = TIMEOUT_GAME_TICK
	};

	PythonApp(Network::EventDispatcher& dispatcher,
		Network::NetworkInterface& ninterface, 
		COMPONENT_TYPE componentType,
		COMPONENT_ID componentID);

	~PythonApp();
	
	KBEngine::script::Script& getScript(){ return script_; }
	PyObjectPtr getEntryScript(){ return entryScript_; }

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

	virtual void finalise();
	virtual bool inInitialize();
	virtual bool initializeEnd();
	virtual void onShutdownBegin();
	virtual void onShutdownEnd();

	virtual void handleTimeout(TimerHandle, void * arg);

	/** 网络接口
		请求执行一段python指令
	*/
	void onExecScriptCommand(Network::Channel* pChannel, KBEngine::MemoryStream& s);

	/** 
		console请求开始profile
	*/
	virtual void startProfile_(Network::Channel* pChannel, std::string profileName, int8 profileType, uint32 timelen);

	/**
		获取apps发布状态, 可在脚本中获取该值
	*/
	static PyObject* __py_getAppPublish(PyObject* self, PyObject* args);

	/**
		获取自定义配置参数。
		脚本调用格式:
		KBEngine.getCustomCfg("battle.maxPlayers")
		KBEngine.getCustomCfg("battle.maxPlayers", 100)
		配置存在时按XML中<param type="...">声明的类型返回Python对象；配置不存在时返回default或None。
		该接口只读，不提供任何脚本层写入配置的能力。
	*/
	static PyObject* __py_getCustomCfg(PyObject* self, PyObject* args);

	/**
		设置脚本输出类型前缀
	*/
	static PyObject* __py_setScriptLogType(PyObject* self, PyObject* args);

	/**
		重新导入所有的脚本
	*/
	virtual void reloadScript(bool fullReload);
	virtual void onReloadScript(bool fullReload);

	/**
		刷新当前进程内所有脚本 Timer 保存的 Python 回调。
		该接口只处理脚本函数/绑定方法对象，不重新创建 Timer，因此不会改变剩余触发时间；
		当回调无法解析到新对象时会保留旧对象并计入 keptOld，避免热更导致 Timer 丢失。
	*/
	static ReloadScriptTimerStats reloadScriptTimers();

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

	/** Timer操作
	*/
	static PyObject* __py_addTimer(PyObject* self, PyObject* args);
	static PyObject* __py_delTimer(PyObject* self, PyObject* args);

	static ScriptTimers &scriptTimers() { return scriptTimers_; }


protected:
	static ScriptTimers										scriptTimers_;

	TimerHandle												gameTickTimerHandle_;

	KBEngine::script::Script								script_;

	PyObjectPtr												entryScript_;
	std::vector<PyObject*>									pluginEntryScripts_;

};


}

#endif // KBE_PYTHON_APP_H
