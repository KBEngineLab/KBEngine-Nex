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


#include "python_app.h"
#include "asyncio_helper.h"
#include "pyscript/py_memorystream.h"
#include "server/py_file_descriptor.h"
#include "server/plugin_runtime.h"
#include "resmgr/plugins/plugin_manager.h"
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace KBEngine{

namespace {

// 这些组件直接使用 PythonApp 启动脚本环境，并共享同一套插件生命周期入口。
// These components install their script environment through PythonApp and share one plugin lifecycle entry point.
bool usesPythonAppPluginRuntime(COMPONENT_TYPE componentType)
{
	return componentType == DBMGR_TYPE || componentType == LOGINAPP_TYPE ||
		componentType == INTERFACES_TYPE || componentType == LOGGER_TYPE;
}

std::string normalizeCustomCfgType(const std::string& type)
{
	std::string lowerType = type;
	std::transform(lowerType.begin(), lowerType.end(), lowerType.begin(), [](unsigned char ch) {
		return static_cast<char>(std::tolower(ch));
	});
	return lowerType;
}

// bool 显式支持常用可读写法，非法值不会静默退化成 false。
// Bool accepts common readable forms and never silently converts invalid input to false.
bool parseCustomCfgBool(const std::string& value, bool& result)
{
	std::string lowerValue = normalizeCustomCfgType(value);
	if(lowerValue == "true" || lowerValue == "1" || lowerValue == "yes")
	{
		result = true;
		return true;
	}

	if(lowerValue == "false" || lowerValue == "0" || lowerValue == "no")
	{
		result = false;
		return true;
	}

	return false;
}

// strtod 配合完整尾部检查，拒绝 "3.5abc" 这类部分成功的配置。
// strtod plus full-tail validation rejects partially parsed values such as "3.5abc".
bool parseCustomCfgFloat(const std::string& value, double& result)
{
	char* end = NULL;
	errno = 0;
	result = std::strtod(value.c_str(), &end);
	return end != value.c_str() && end != NULL && *end == '\0' && errno != ERANGE;
}

// dict/list 使用 literal_eval，支持 Python 字面量但不执行任意脚本。
// Dict/list values use literal_eval to support Python literals without executing arbitrary code.
PyObject* parseCustomCfgLiteral(const ServerConfig::CustomCfgItem& item, const char* expectedType)
{
	PyObject* astModule = PyImport_ImportModule("ast");
	if(astModule == NULL)
	{
		ERROR_MSG("KBEngine::getCustomCfg(): unable to import ast module for customCfg literal parsing.\n");
		PyErr_PrintEx(0);
		Py_RETURN_NONE;
	}

	PyObject* literalEval = PyObject_GetAttrString(astModule, "literal_eval");
	Py_DECREF(astModule);
	if(literalEval == NULL)
	{
		ERROR_MSG("KBEngine::getCustomCfg(): unable to get ast.literal_eval for customCfg literal parsing.\n");
		PyErr_PrintEx(0);
		Py_RETURN_NONE;
	}

	PyObject* pyValueText = PyUnicode_FromString(item.value.c_str());
	if(pyValueText == NULL)
	{
		ERROR_MSG(fmt::format(
			"KBEngine::getCustomCfg(): customCfg[{}] unable to build unicode value, value={}.\n",
			item.name, item.value));
		Py_DECREF(literalEval);
		PyErr_PrintEx(0);
		Py_RETURN_NONE;
	}

	PyObject* pyValue = PyObject_CallFunctionObjArgs(literalEval, pyValueText, NULL);
	Py_DECREF(literalEval);
	Py_DECREF(pyValueText);
	if(pyValue == NULL)
	{
		ERROR_MSG(fmt::format(
			"KBEngine::getCustomCfg(): customCfg[{}] value parse failed, type={}, value={}.\n",
			item.name, item.type, item.value));
		PyErr_PrintEx(0);
		Py_RETURN_NONE;
	}

	// 声明类型必须与解析结果一致，避免脚本拿到与配置契约不同的对象。
	// The parsed object must match the declared type so scripts receive a stable contract.
	if(strcmp(expectedType, "dict") == 0 && !PyDict_Check(pyValue))
	{
		ERROR_MSG(fmt::format(
			"KBEngine::getCustomCfg(): customCfg[{}] expects dict, value={}.\n",
			item.name, item.value));
		Py_DECREF(pyValue);
		Py_RETURN_NONE;
	}

	if(strcmp(expectedType, "list") == 0 && !PyList_Check(pyValue))
	{
		ERROR_MSG(fmt::format(
			"KBEngine::getCustomCfg(): customCfg[{}] expects list, value={}.\n",
			item.name, item.value));
		Py_DECREF(pyValue);
		Py_RETURN_NONE;
	}

	return pyValue;
}

// default 只处理缺失 key，不参与已有配置的类型推断，保证不同调用方看到相同类型。
// The default handles missing keys only; declared XML types remain stable across callers.
PyObject* customCfgItemToPyObject(const ServerConfig::CustomCfgItem& item)
{
	std::string type = normalizeCustomCfgType(item.type);
	if(type == "bool")
	{
		bool value = false;
		if(!parseCustomCfgBool(item.value, value))
		{
			ERROR_MSG(fmt::format(
				"KBEngine::getCustomCfg(): customCfg[{}] bool parse failed, value={}.\n",
				item.name, item.value));
			Py_RETURN_NONE;
		}

		if(value)
			Py_RETURN_TRUE;
		Py_RETURN_FALSE;
	}

	if(type == "int")
	{
		char* end = NULL;
		PyObject* pyValue = PyLong_FromString(const_cast<char*>(item.value.c_str()), &end, 10);
		if(pyValue == NULL || end == NULL || *end != '\0')
		{
			Py_XDECREF(pyValue);
			PyErr_Clear();
			ERROR_MSG(fmt::format(
				"KBEngine::getCustomCfg(): customCfg[{}] int parse failed, value={}.\n",
				item.name, item.value));
			Py_RETURN_NONE;
		}
		return pyValue;
	}

	if(type == "float")
	{
		double value = 0.0;
		if(!parseCustomCfgFloat(item.value, value))
		{
			ERROR_MSG(fmt::format(
				"KBEngine::getCustomCfg(): customCfg[{}] float parse failed, value={}.\n",
				item.name, item.value));
			Py_RETURN_NONE;
		}
		return PyFloat_FromDouble(value);
	}

	if(type == "string" || type == "str")
		return PyUnicode_FromString(item.value.c_str());

	if(type == "dict")
		return parseCustomCfgLiteral(item, "dict");

	if(type == "list")
		return parseCustomCfgLiteral(item, "list");

	ERROR_MSG(fmt::format(
		"KBEngine::getCustomCfg(): customCfg[{}] unsupported type={}, value={}.\n",
		item.name, item.type, item.value));
	Py_RETURN_NONE;
}

}

KBEngine::ScriptTimers KBEngine::PythonApp::scriptTimers_;

/**
内部定时器处理类
*/
class ScriptTimerHandler : public TimerHandler
{
public:
	ScriptTimerHandler(ScriptTimers* scriptTimers, PyObject * callback) :
		pyCallback_(callback),
		pyCallbackOwner_(NULL),
		scriptTimers_(scriptTimers)
	{
		Py_INCREF(pyCallback_);
		captureReloadPath(callback);
		handlers_.push_back(this);
	}

	~ScriptTimerHandler()
	{
		std::vector<ScriptTimerHandler*>::iterator iter =
			std::find(handlers_.begin(), handlers_.end(), this);
		if (iter != handlers_.end())
			handlers_.erase(iter);

		Py_XDECREF(pyCallbackOwner_);
		Py_DECREF(pyCallback_);
	}

	static ReloadScriptTimerStats reloadAllCallbacks()
	{
		ReloadScriptTimerStats stats;
		// 使用快照遍历，并在每次操作前确认 handler 仍存活，避免回调解析间接删除 Timer 后悬空访问。
		// Iterate a snapshot and recheck liveness so callback resolution cannot leave a dangling handler after indirect timer deletion.
		std::vector<ScriptTimerHandler*> handlers = handlers_;
		for (std::vector<ScriptTimerHandler*>::iterator iter = handlers.begin(); iter != handlers.end(); ++iter)
		{
			if (std::find(handlers_.begin(), handlers_.end(), *iter) == handlers_.end())
				continue;

			if ((*iter)->reloadCallback())
				++stats.refreshed;
			else
			{
				++stats.keptOld;
				stats.keptOldCallbacks.push_back((*iter)->describeCallback());
			}
		}

		return stats;
	}

private:
	virtual void handleTimeout(TimerHandle handle, void * pUser)
	{
		int id = ScriptTimersUtil::getIDForHandle(scriptTimers_, handle);

		PyObject *pyRet = PyObject_CallFunction(pyCallback_, "i", id);
		if (pyRet == NULL)
		{
			SCRIPT_ERROR_CHECK();
			return;
		}
		return;
	}

	virtual void onRelease(TimerHandle handle, void * /*pUser*/)
	{
		scriptTimers_->releaseTimer(handle);
		delete this;
	}

	bool getStringAttr(PyObject* pyObj, const char* attrName, std::string& out)
	{
		PyObject* pyAttr = PyObject_GetAttrString(pyObj, attrName);
		if (!pyAttr)
		{
			PyErr_Clear();
			return false;
		}

		const char* attr = PyUnicode_AsUTF8AndSize(pyAttr, NULL);
		if (!attr)
		{
			PyErr_Clear();
			Py_DECREF(pyAttr);
			return false;
		}

		out = attr;
		Py_DECREF(pyAttr);
		return true;
	}

	void captureReloadPath(PyObject* callback)
	{
		// 绑定方法从原 owner 重新取同名属性；Entity/Component 换类后会自然绑定到新实现。
		// Bound methods are resolved again from their owner, naturally binding to the new Entity/Component class.
		if (PyMethod_Check(callback))
		{
			PyObject* pySelf = PyMethod_GET_SELF(callback);
			if (pySelf && getStringAttr(callback, "__name__", callbackName_))
			{
				pyCallbackOwner_ = pySelf;
				Py_INCREF(pyCallbackOwner_);
			}
			return;
		}

		getStringAttr(callback, "__module__", callbackModule_);
		getStringAttr(callback, "__qualname__", callbackQualName_);
	}

	PyObject* resolveModuleCallback()
	{
		if (callbackModule_.empty() || callbackQualName_.empty() ||
			callbackQualName_.find("<locals>") != std::string::npos)
		{
			return NULL;
		}

		PyObject* pyObj = PyDict_GetItemString(PyImport_GetModuleDict(), callbackModule_.c_str());
		if (!pyObj)
			return NULL;

		Py_INCREF(pyObj);
		std::string::size_type start = 0;
		while (start < callbackQualName_.size())
		{
			std::string::size_type end = callbackQualName_.find('.', start);
			std::string attrName = callbackQualName_.substr(start,
				end == std::string::npos ? std::string::npos : end - start);
			PyObject* pyNext = PyObject_GetAttrString(pyObj, attrName.c_str());
			Py_DECREF(pyObj);
			if (!pyNext)
			{
				PyErr_Clear();
				return NULL;
			}

			pyObj = pyNext;
			if (end == std::string::npos)
				break;
			start = end + 1;
		}

		return pyObj;
	}

	bool reloadCallback()
	{
		PyObject* pyNewCallback = NULL;
		if (pyCallbackOwner_ && !callbackName_.empty())
		{
			pyNewCallback = PyObject_GetAttrString(pyCallbackOwner_, callbackName_.c_str());
			if (!pyNewCallback)
				PyErr_Clear();
		}
		else
		{
			pyNewCallback = resolveModuleCallback();
		}

		// 无法稳定定位的闭包或已删除函数继续使用旧对象，Timer 不能因热更静默丢失。
		// Closures or removed functions that cannot be resolved keep the old object; hot reload must not silently drop a timer.
		if (!pyNewCallback)
			return false;
		if (!PyCallable_Check(pyNewCallback))
		{
			Py_DECREF(pyNewCallback);
			return false;
		}

		Py_DECREF(pyCallback_);
		pyCallback_ = pyNewCallback;
		return true;
	}

	std::string describeCallback() const
	{
		if (pyCallbackOwner_ && !callbackName_.empty())
			return fmt::format("{}.{}", pyCallbackOwner_->ob_type->tp_name, callbackName_);
		if (!callbackModule_.empty() || !callbackQualName_.empty())
			return fmt::format("{}.{}", callbackModule_, callbackQualName_);
		return "<unknown>";
	}

	PyObject* pyCallback_;
	PyObject* pyCallbackOwner_;
	std::string callbackModule_;
	std::string callbackQualName_;
	std::string callbackName_;
	ScriptTimers* scriptTimers_;
	static std::vector<ScriptTimerHandler*> handlers_;
};

std::vector<ScriptTimerHandler*> ScriptTimerHandler::handlers_;

//-------------------------------------------------------------------------------------
PythonApp::PythonApp(Network::EventDispatcher& dispatcher, 
					 Network::NetworkInterface& ninterface, 
					 COMPONENT_TYPE componentType,
					 COMPONENT_ID componentID):
ServerApp(dispatcher, ninterface, componentType, componentID),
script_(),
entryScript_()
{
	ScriptTimers::initialize(*this);
}

//-------------------------------------------------------------------------------------
PythonApp::~PythonApp()
{
}

//-------------------------------------------------------------------------------------
bool PythonApp::inInitialize()
{
	if(!installPyScript())
		return false;

	if(!installPyModules())
		return false;
	
	return true;
}

//-------------------------------------------------------------------------------------	
bool PythonApp::initializeEnd()
{
	gameTickTimerHandle_ = this->dispatcher().addTimer(1000000 / g_kbeSrvConfig.gameUpdateHertz(), this,
		reinterpret_cast<void *>(TIMEOUT_GAME_TICK));

	if (usesPythonAppPluginRuntime(componentType_) &&
		!PluginRuntime::instance().initialize(componentType_, false))
		return false;

	// 非 EntityApp 组件也使用自身 dispatcher 在主线程推进 asyncio，避免跨线程访问脚本状态。
	// Non-EntityApp components also pump asyncio on their own dispatcher thread to avoid cross-thread script access.
	if (!AsyncioHelper::installTimer(this->dispatcher()))
		return false;

	return true;
}

//-------------------------------------------------------------------------------------	
void PythonApp::onShutdownBegin()
{
	ServerApp::onShutdownBegin();
}

//-------------------------------------------------------------------------------------	
void PythonApp::onShutdownEnd()
{
	ServerApp::onShutdownEnd();
}

//-------------------------------------------------------------------------------------
void PythonApp::finalise(void)
{
	// 必须先停止协程，再卸载插件和 Python 模块，否则待执行 Task 可能访问已释放对象。
	// Coroutines must stop before plugins and Python modules are unloaded or pending Tasks may access released objects.
	AsyncioHelper::shutdown();

	if (usesPythonAppPluginRuntime(componentType_))
		PluginRuntime::instance().finalise();

	gameTickTimerHandle_.cancel();
	scriptTimers_.cancelAll();
	ScriptTimers::finalise(*this);

	uninstallPyScript();
	ServerApp::finalise();
}

//-------------------------------------------------------------------------------------
void PythonApp::handleTimeout(TimerHandle handle, void * arg)
{
	ServerApp::handleTimeout(handle, arg);

	switch (reinterpret_cast<uintptr>(arg))
	{
	case TIMEOUT_GAME_TICK:
		++g_kbetime;
		handleTimers();
		break;
	default:
		break;
	}
}

//-------------------------------------------------------------------------------------
int PythonApp::registerPyObjectToScript(const char* attrName, PyObject* pyObj)
{ 
	return script_.registerToModule(attrName, pyObj); 
}

//-------------------------------------------------------------------------------------
int PythonApp::unregisterPyObjectToScript(const char* attrName)
{ 
	return script_.unregisterToModule(attrName); 
}

//-------------------------------------------------------------------------------------
bool PythonApp::installPyScript()
{
	if(Resmgr::getSingleton().respaths().size() <= 0 || 
		Resmgr::getSingleton().getPyUserResPath().size() == 0 || 
		Resmgr::getSingleton().getPySysResPath().size() == 0 ||
		Resmgr::getSingleton().getPyUserScriptsPath().size() == 0)
	{
		KBE_ASSERT(false && "PythonApp::installPyScript: KBE_RES_PATH error!\n");
		return false;
	}

	std::wstring user_scripts_path = L"";
	wchar_t* tbuf = KBEngine::strutil::char2wchar(const_cast<char*>(Resmgr::getSingleton().getPyUserScriptsPath().c_str()));
	if(tbuf != NULL)
	{
		user_scripts_path += tbuf;
		free(tbuf);
	}
	else
	{
		KBE_ASSERT(false && "PythonApp::installPyScript: KBE_RES_PATH error[char2wchar]!\n");
		return false;
	}

	// 用户脚本根目录使 plugins.<name> 包名可以稳定导入，同时仍保持宿主资源路径优先。
	// The user script root makes plugins.<name> package imports stable while preserving host-resource precedence.
	std::wstring pyPaths = user_scripts_path + L";";
	pyPaths += user_scripts_path + L"common;";
	pyPaths += user_scripts_path + L"data;";
	pyPaths += user_scripts_path + L"user_type;";

	switch (componentType_)
	{
	case BASEAPP_TYPE:
		pyPaths += user_scripts_path + L"server_common;";
		pyPaths += user_scripts_path + L"base;";
		pyPaths += user_scripts_path + L"base/interfaces;";
		pyPaths += user_scripts_path + L"base/components;";
		break;
	case CELLAPP_TYPE:
		pyPaths += user_scripts_path + L"server_common;";
		pyPaths += user_scripts_path + L"cell;";
		pyPaths += user_scripts_path + L"cell/interfaces;";
		pyPaths += user_scripts_path + L"cell/components;";
		break;
	case DBMGR_TYPE:
		pyPaths += user_scripts_path + L"server_common;";
		pyPaths += user_scripts_path + L"db;";
		break;
	case INTERFACES_TYPE:
		pyPaths += user_scripts_path + L"server_common;";
		pyPaths += user_scripts_path + L"interface;";
		break;
	case LOGINAPP_TYPE:
		pyPaths += user_scripts_path + L"server_common;";
		pyPaths += user_scripts_path + L"login;";
		break;
	case LOGGER_TYPE:
		pyPaths += user_scripts_path + L"server_common;";
		pyPaths += user_scripts_path + L"logger;";
		break;
	default:
		pyPaths += user_scripts_path + L"client;";
		pyPaths += user_scripts_path + L"client/interfaces;";
		pyPaths += user_scripts_path + L"client/components;";
		break;
	};

	if (!PluginManager::instance().initialize())
		return false;

	// manifest 中的脚本路径按 plugins.xml 顺序追加，保证插件覆盖关系在各组件中一致。
	// Manifest script paths follow plugins.xml order so plugin precedence stays consistent across components.
	std::vector<std::string> pluginPaths = PluginManager::instance().getComponentPythonPaths(componentType_);
	for (std::vector<std::string>::const_iterator iter = pluginPaths.begin(); iter != pluginPaths.end(); ++iter)
	{
		tbuf = KBEngine::strutil::char2wchar(const_cast<char*>(iter->c_str()));
		if (tbuf != NULL)
		{
			pyPaths += tbuf;
			pyPaths += L";";
			free(tbuf);
		}
	}
	
	std::string kbe_res_path = Resmgr::getSingleton().getPySysResPath();
	kbe_res_path += "scripts/common";

	tbuf = KBEngine::strutil::char2wchar(const_cast<char*>(kbe_res_path.c_str()));
	bool ret = getScript().install(tbuf, pyPaths, "KBEngine", componentType_);
	free(tbuf);

	if (ret)
		script::PyMemoryStream::installScript(NULL);

	return ret;
}

//-------------------------------------------------------------------------------------
bool PythonApp::uninstallPyScript()
{
	script::PyMemoryStream::uninstallScript();
	return uninstallPyModules() && getScript().uninstall();
}

//-------------------------------------------------------------------------------------
bool PythonApp::installPyModules()
{
	// 安装入口模块
	PyObject *entryScriptFileName = NULL;
	if(componentType() == BASEAPP_TYPE)
	{
		ENGINE_COMPONENT_INFO& info = g_kbeSrvConfig.getBaseApp();
		entryScriptFileName = PyUnicode_FromString(info.entryScriptFile);
	}
	else if(componentType() == CELLAPP_TYPE)
	{
		ENGINE_COMPONENT_INFO& info = g_kbeSrvConfig.getCellApp();
		entryScriptFileName = PyUnicode_FromString(info.entryScriptFile);
	}
	else if(componentType() == INTERFACES_TYPE)
	{
		ENGINE_COMPONENT_INFO& info = g_kbeSrvConfig.getInterfaces();
		entryScriptFileName = PyUnicode_FromString(info.entryScriptFile);
	}
	else if (componentType() == LOGINAPP_TYPE)
	{
		ENGINE_COMPONENT_INFO& info = g_kbeSrvConfig.getLoginApp();
		entryScriptFileName = PyUnicode_FromString(info.entryScriptFile);
	}
	else if (componentType() == DBMGR_TYPE)
	{
		ENGINE_COMPONENT_INFO& info = g_kbeSrvConfig.getDBMgr();
		entryScriptFileName = PyUnicode_FromString(info.entryScriptFile);
	}
	else if (componentType() == LOGGER_TYPE)
	{
		ENGINE_COMPONENT_INFO& info = g_kbeSrvConfig.getLogger();
		entryScriptFileName = PyUnicode_FromString(info.entryScriptFile);
	}
	else
	{
		ERROR_MSG("PythonApp::installPyModules: Unsupported script!\n");
	}

	PyObject * module = getScript().getModule();

	APPEND_SCRIPT_MODULE_METHOD(module, MemoryStream, script::PyMemoryStream::py_new, METH_VARARGS, 0);

	// 注册创建entity的方法到py
	// 向脚本注册app发布状态
	APPEND_SCRIPT_MODULE_METHOD(module, publish, __py_getAppPublish, METH_VARARGS, 0);

	// 所有直接使用 PythonApp 的服务端组件共享同一套只读配置接口。
	// Every server component using PythonApp shares the same read-only configuration API.
	APPEND_SCRIPT_MODULE_METHOD(module, getCustomCfg, __py_getCustomCfg, METH_VARARGS, 0);

	// 注册设置脚本输出类型
	APPEND_SCRIPT_MODULE_METHOD(module, scriptLogType, __py_setScriptLogType, METH_VARARGS, 0);
	
	// 获得资源全路径
	APPEND_SCRIPT_MODULE_METHOD(module, getResFullPath, __py_getResFullPath, METH_VARARGS, 0);

	// 是否存在某个资源
	APPEND_SCRIPT_MODULE_METHOD(module, hasRes, __py_hasRes, METH_VARARGS, 0);

	// 打开一个文件
	APPEND_SCRIPT_MODULE_METHOD(module, open, __py_kbeOpen, METH_VARARGS, 0);

	// 列出目录下所有文件
	APPEND_SCRIPT_MODULE_METHOD(module, listPathRes, __py_listPathRes, METH_VARARGS, 0);

	// 匹配相对路径获得全路径
	APPEND_SCRIPT_MODULE_METHOD(module, matchPath, __py_matchPath, METH_VARARGS, 0);

	// debug追踪kbe封装的py对象计数
	APPEND_SCRIPT_MODULE_METHOD(module, debugTracing, script::PyGC::__py_debugTracing, METH_VARARGS, 0);

	if (PyModule_AddIntConstant(module, "LOG_TYPE_NORMAL", log4cxx::ScriptLevel::SCRIPT_INT))
	{
		ERROR_MSG( "PythonApp::installPyModules: Unable to set KBEngine.LOG_TYPE_NORMAL.\n");
	}

	if (PyModule_AddIntConstant(module, "LOG_TYPE_INFO", log4cxx::ScriptLevel::SCRIPT_INFO))
	{
		ERROR_MSG( "PythonApp::installPyModules: Unable to set KBEngine.LOG_TYPE_INFO.\n");
	}

	if (PyModule_AddIntConstant(module, "LOG_TYPE_ERR", log4cxx::ScriptLevel::SCRIPT_ERR))
	{
		ERROR_MSG( "PythonApp::installPyModules: Unable to set KBEngine.LOG_TYPE_ERR.\n");
	}

	if (PyModule_AddIntConstant(module, "LOG_TYPE_DBG", log4cxx::ScriptLevel::SCRIPT_DBG))
	{
		ERROR_MSG( "PythonApp::installPyModules: Unable to set KBEngine.LOG_TYPE_DBG.\n");
	}

	if (PyModule_AddIntConstant(module, "LOG_TYPE_WAR", log4cxx::ScriptLevel::SCRIPT_WAR))
	{
		ERROR_MSG( "PythonApp::installPyModules: Unable to set KBEngine.LOG_TYPE_WAR.\n");
	}

	if (PyModule_AddIntConstant(module, "NEXT_ONLY", KBE_NEXT_ONLY))
	{
		ERROR_MSG( "PythonApp::installPyModules: Unable to set KBEngine.NEXT_ONLY.\n");
	}
	
	// 注册所有pythonApp都要用到的通用接口
	APPEND_SCRIPT_MODULE_METHOD(module,		addTimer,						__py_addTimer,											METH_VARARGS,	0);
	APPEND_SCRIPT_MODULE_METHOD(module,		delTimer,						__py_delTimer,											METH_VARARGS,	0);
	APPEND_SCRIPT_MODULE_METHOD(module,		registerReadFileDescriptor,		PyFileDescriptor::__py_registerReadFileDescriptor,		METH_VARARGS,	0);
	APPEND_SCRIPT_MODULE_METHOD(module,		registerWriteFileDescriptor,	PyFileDescriptor::__py_registerWriteFileDescriptor,		METH_VARARGS,	0);
	APPEND_SCRIPT_MODULE_METHOD(module,		deregisterReadFileDescriptor,	PyFileDescriptor::__py_deregisterReadFileDescriptor,	METH_VARARGS,	0);
	APPEND_SCRIPT_MODULE_METHOD(module,		deregisterWriteFileDescriptor,	PyFileDescriptor::__py_deregisterWriteFileDescriptor,	METH_VARARGS,	0);
	// 完成式文件描述符接口供 Nex 2.8 脚本使用；上方旧名称仅保留为带迁移提示的报错入口。
	// Completion file-descriptor APIs serve Nex 2.8 scripts; legacy names above remain only as error entries with migration guidance.
	APPEND_SCRIPT_MODULE_METHOD(module,		registerReadDataFileDescriptor,	PyFileDescriptor::__py_registerReadDataFileDescriptor,	METH_VARARGS,	0);
	APPEND_SCRIPT_MODULE_METHOD(module,		deregisterReadDataFileDescriptor,	PyFileDescriptor::__py_deregisterReadDataFileDescriptor,	METH_VARARGS,	0);
	APPEND_SCRIPT_MODULE_METHOD(module,		registerAcceptFileDescriptor,	PyFileDescriptor::__py_registerAcceptFileDescriptor,	METH_VARARGS,	0);
	APPEND_SCRIPT_MODULE_METHOD(module,		deregisterAcceptFileDescriptor,	PyFileDescriptor::__py_deregisterAcceptFileDescriptor,	METH_VARARGS,	0);
	APPEND_SCRIPT_MODULE_METHOD(module,		writeFileDescriptor,			PyFileDescriptor::__py_writeFileDescriptor,			METH_VARARGS,	0);

	onInstallPyModules();

	if (entryScriptFileName != NULL)
	{
		entryScript_ = PyImport_Import(entryScriptFileName);
		SCRIPT_ERROR_CHECK();
		S_RELEASE(entryScriptFileName);

		if(entryScript_.get() == NULL)
		{
			return false;
		}
	}

	return true;
}

//-------------------------------------------------------------------------------------
bool PythonApp::uninstallPyModules()
{
	// script::PyGC::set_debug(script::PyGC::DEBUG_STATS|script::PyGC::DEBUG_LEAK);
	// script::PyGC::collect();

	script::PyGC::debugTracing();
	return true;
}

//-------------------------------------------------------------------------------------
PyObject* PythonApp::__py_getAppPublish(PyObject* self, PyObject* args)
{
	return PyLong_FromLong(g_appPublish);
}

//-------------------------------------------------------------------------------------
PyObject* PythonApp::__py_getCustomCfg(PyObject* self, PyObject* args)
{
	Py_ssize_t argCount = PyTuple_Size(args);
	if(argCount != 1 && argCount != 2)
	{
		PyErr_Format(PyExc_TypeError,
			"KBEngine::getCustomCfg(): requires 1 or 2 args (key[, default])!");
		return NULL;
	}

	const char* key = NULL;
	PyObject* pyDefault = NULL;
	if(!PyArg_ParseTuple(args, "s|O", &key, &pyDefault))
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::getCustomCfg(): args error!");
		return NULL;
	}

	const std::map<std::string, ServerConfig::CustomCfgItem>& cfg = g_kbeSrvConfig.customCfg();
	std::map<std::string, ServerConfig::CustomCfgItem>::const_iterator iter = cfg.find(key);
	if(iter == cfg.end())
	{
		if(pyDefault != NULL)
		{
			Py_INCREF(pyDefault);
			return pyDefault;
		}

		Py_RETURN_NONE;
	}

	return customCfgItemToPyObject(iter->second);
}

//-------------------------------------------------------------------------------------
PyObject* PythonApp::__py_setScriptLogType(PyObject* self, PyObject* args)
{
	Py_ssize_t argCount = PyTuple_Size(args);
	if(argCount != 1)
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::scriptLogType(): args error!");
		PyErr_PrintEx(0);
		S_Return;
	}

	int type = -1;

	if(!PyArg_ParseTuple(args, "i", &type))
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::scriptLogType(): args error!");
		PyErr_PrintEx(0);
		S_Return;
	}

	DebugHelper::getSingleton().setScriptMsgType(type);
	S_Return;
}

//-------------------------------------------------------------------------------------
PyObject* PythonApp::__py_getResFullPath(PyObject* self, PyObject* args)
{
	Py_ssize_t argCount = PyTuple_Size(args);
	if(argCount != 1)
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::getResFullPath(): args error!");
		PyErr_PrintEx(0);
		S_Return;
	}

	char* respath = NULL;

	if(!PyArg_ParseTuple(args, "s", &respath))
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::getResFullPath(): args error!");
		PyErr_PrintEx(0);
		S_Return;
	}

	if(!Resmgr::getSingleton().hasRes(respath))
		return PyUnicode_FromString("");

	std::string fullpath = Resmgr::getSingleton().matchRes(respath);
	return PyUnicode_FromString(fullpath.c_str());
}

//-------------------------------------------------------------------------------------
PyObject* PythonApp::__py_hasRes(PyObject* self, PyObject* args)
{
	Py_ssize_t argCount = PyTuple_Size(args);
	if(argCount != 1)
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::hasRes(): args error!");
		PyErr_PrintEx(0);
		S_Return;
	}

	char* respath = NULL;

	if(!PyArg_ParseTuple(args, "s", &respath))
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::hasRes(): args error!");
		PyErr_PrintEx(0);
		S_Return;
	}

	return PyBool_FromLong(Resmgr::getSingleton().hasRes(respath));
}

//-------------------------------------------------------------------------------------
PyObject* PythonApp::__py_kbeOpen(PyObject* self, PyObject* args)
{
	Py_ssize_t argCount = PyTuple_Size(args);
	if(argCount != 2)
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::open(): args error!");
		PyErr_PrintEx(0);
		S_Return;
	}

	char* respath = NULL;
	char* fargs = NULL;

	if(!PyArg_ParseTuple(args, "s|s", &respath, &fargs))
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::open(): args error!");
		PyErr_PrintEx(0);
		S_Return;
	}

	std::string sfullpath = Resmgr::getSingleton().matchRes(respath);

	PyObject *ioMod = PyImport_ImportModule("io");

	// SCOPED_PROFILE(SCRIPTCALL_PROFILE);
	PyObject *openedFile = PyObject_CallMethod(ioMod, const_cast<char*>("open"), 
		const_cast<char*>("ss"), 
		const_cast<char*>(sfullpath.c_str()), 
		fargs);

	Py_DECREF(ioMod);
	
	if(openedFile == NULL)
	{
		SCRIPT_ERROR_CHECK();
	}

	return openedFile;
}

//-------------------------------------------------------------------------------------
PyObject* PythonApp::__py_matchPath(PyObject* self, PyObject* args)
{
	Py_ssize_t argCount = PyTuple_Size(args);
	if(argCount != 1)
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::matchPath(): args error!");
		PyErr_PrintEx(0);
		S_Return;
	}

	char* respath = NULL;

	if(!PyArg_ParseTuple(args, "s", &respath))
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::matchPath(): args error!");
		PyErr_PrintEx(0);
		S_Return;
	}

	std::string path = Resmgr::getSingleton().matchPath(respath);
	return PyUnicode_FromStringAndSize(path.c_str(), path.size());
}

//-------------------------------------------------------------------------------------
PyObject* PythonApp::__py_listPathRes(PyObject* self, PyObject* args)
{
	Py_ssize_t argCount = PyTuple_Size(args);
	if(argCount < 1 || argCount > 2)
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::listPathRes(): args[path, pathargs=\'*.*\'] error!");
		PyErr_PrintEx(0);
		S_Return;
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
			S_Return;
		}
	}
	else
	{
		if(!PyArg_ParseTuple(args, "O|O", &pathobj, &path_argsobj))
		{
			PyErr_Format(PyExc_TypeError, "KBEngine::listPathRes(): args[path, pathargs=\'*.*\'] error!");
			PyErr_PrintEx(0);
			S_Return;
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
						S_Return;
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
				S_Return;
			}
		}
	}

	if(!PyUnicode_Check(pathobj))
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::listPathRes(): args[path] error!");
		PyErr_PrintEx(0);
		S_Return;
	}

	if(PyUnicode_GET_LENGTH(pathobj) == 0)
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::listPathRes(): args[path] is NULL!");
		PyErr_PrintEx(0);
		S_Return;
	}

	if(wExtendName.size() == 0)
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::listPathRes(): args[pathargs] is NULL!");
		PyErr_PrintEx(0);
		S_Return;
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
		S_Return;
	}

	char* cpath = strutil::wchar2char(respath);
	std::string foundPath = Resmgr::getSingleton().matchPath(cpath);
	free(cpath);
	PyMem_Free(respath);

	respath = strutil::char2wchar(foundPath.c_str());

	std::vector<std::wstring> results;
	Resmgr::getSingleton().listPathRes(respath, wExtendName, results);
	PyObject* pyresults = PyTuple_New(results.size());

	std::vector<std::wstring>::iterator iter = results.begin();
	int i = 0;

	for(; iter != results.end(); ++iter)
	{
		PyTuple_SET_ITEM(pyresults, i++, PyUnicode_FromWideChar((*iter).c_str(), (*iter).size()));
	}

	free(respath);
	return pyresults;
}

//-------------------------------------------------------------------------------------
void PythonApp::startProfile_(Network::Channel* pChannel, std::string profileName, 
	int8 profileType, uint32 timelen)
{
	if(pChannel == NULL || pChannel->isExternal())
		return;
	
	switch(profileType)
	{
	case 0:	// pyprofile
		new PyProfileHandler(this->networkInterface(), timelen, profileName, pChannel->addr());
		return;
	case 4: // pytickprofile
		if(g_componentType != BASEAPP_TYPE && g_componentType != CELLAPP_TYPE)
		{
			WARNING_MSG(fmt::format("PythonApp::startProfile_: pytickprofile is unsupported on componentType={}.\n",
				g_componentType));
			return;
		}

		// WebConsole/Telnet already expose Python tick profiling. GUIConsole uses the same
		// handler so operators can inspect per-tick Python hotspots without a separate shell.
		// WebConsole/Telnet 已经提供逐 Tick Python 采样，这里复用同一处理器开放给 GUIConsole。
		new PyTickProfileHandler(this->networkInterface(), timelen, profileName, pChannel->addr());
		return;
	default:
		break;
	};

	ServerApp::startProfile_(pChannel, profileName, profileType, timelen);
}

//-------------------------------------------------------------------------------------
void PythonApp::onExecScriptCommand(Network::Channel* pChannel, KBEngine::MemoryStream& s)
{
	if(pChannel->isExternal())
		return;
	
	std::string cmd;
	s.readBlob(cmd);
	if (!validateManagementAdminToken(pChannel, s, "onExecScriptCommand"))
	{
		return;
	}

	PyObject* pycmd = PyUnicode_DecodeUTF8(cmd.data(), cmd.size(), NULL);
	if(pycmd == NULL)
	{
		SCRIPT_ERROR_CHECK();
		return;
	}

	DEBUG_MSG(fmt::format("PythonApp::onExecScriptCommand: size({}), command={}.\n", 
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

//-------------------------------------------------------------------------------------
void PythonApp::onReloadScript(bool fullReload)
{
}

//-------------------------------------------------------------------------------------
void PythonApp::reloadScript(bool fullReload)
{
	static bool isReloading = false;
	if (isReloading)
	{
		WARNING_MSG(fmt::format("{}::reloadScript: ignored reentrant reload request, fullReload={}.\n",
			COMPONENT_NAME_EX(g_componentType), fullReload));
		return;
	}

	isReloading = true;
	struct ReloadGuard
	{
		bool& value;
		ReloadGuard(bool& v) : value(v) {}
		~ReloadGuard() { value = false; }
	} reloadGuard(isReloading);

	if (g_appPublish != 0 && fullReload)
	{
		WARNING_MSG(fmt::format("{}::reloadScript: production mode forces fullReload=false.\n",
			COMPONENT_NAME_EX(g_componentType)));
		fullReload = false;
	}

	if (usesPythonAppPluginRuntime(componentType_))
		PluginRuntime::instance().finalise();

	onReloadScript(fullReload);
	ReloadScriptTimerStats timerStats = reloadScriptTimers();
	INFO_MSG(fmt::format("{}::reloadScript: fullReload={}, timersRefreshed={}, timersKeptOld={}.\n",
		COMPONENT_NAME_EX(g_componentType), fullReload, timerStats.refreshed, timerStats.keptOld));
	for (std::vector<std::string>::const_iterator iter = timerStats.keptOldCallbacks.begin();
		iter != timerStats.keptOldCallbacks.end(); ++iter)
	{
		WARNING_MSG(fmt::format("{}::reloadScript: timer kept old callback: {}.\n",
			COMPONENT_NAME_EX(g_componentType), *iter));
	}

	// SCOPED_PROFILE(SCRIPTCALL_PROFILE);

	// 所有脚本都加载完毕
	PyObject* pyResult = PyObject_CallMethod(getEntryScript().get(),
										const_cast<char*>("onInit"),
										const_cast<char*>("i"), 
										1);

	if(pyResult != NULL)
	{
		AsyncioHelper::submitCoroutine(pyResult);
		Py_DECREF(pyResult);
	}
	else
		SCRIPT_ERROR_CHECK();

	if (usesPythonAppPluginRuntime(componentType_) &&
		!PluginRuntime::instance().initialize(componentType_, true))
	{
		ERROR_MSG("PythonApp::reloadScript: plugin lifecycle reload failed.\n");
	}
}

//-------------------------------------------------------------------------------------
ReloadScriptTimerStats PythonApp::reloadScriptTimers()
{
	return ScriptTimerHandler::reloadAllCallbacks();
}

//-------------------------------------------------------------------------------------
PyObject* PythonApp::__py_addTimer(PyObject* self, PyObject* args)
{
	float interval, repeat;
	PyObject *pyCallback;

	if (!PyArg_ParseTuple(args, "ffO", &interval, &repeat, &pyCallback))
		S_Return;

	if (!PyCallable_Check(pyCallback))
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::addTimer: '%.200s' object is not callable", 
			(pyCallback ? pyCallback->ob_type->tp_name : "NULL"));

		PyErr_PrintEx(0);
		S_Return;
	}

	ScriptTimers * pTimers = &scriptTimers();
	ScriptTimerHandler *handler = new ScriptTimerHandler(pTimers, pyCallback);

	ScriptID id = ScriptTimersUtil::addTimer(&pTimers, interval, repeat, 0, handler);

	if (id == 0)
	{
		delete handler;
		PyErr_SetString(PyExc_ValueError, "Unable to add timer");
		PyErr_PrintEx(0);
		S_Return;
	}

	return PyLong_FromLong(id);
}

//-------------------------------------------------------------------------------------
PyObject* PythonApp::__py_delTimer(PyObject* self, PyObject* args)
{
	ScriptID timerID;

	if (!PyArg_ParseTuple(args, "i", &timerID))
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::delTimer: args error!");
		PyErr_PrintEx(0);
		S_Return;
	}

	if (!ScriptTimersUtil::delTimer(&scriptTimers(), timerID))
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::delTimer: error!");
		PyErr_PrintEx(0);
		return PyLong_FromLong(-1);
	}

	return PyLong_FromLong(timerID);
}

//-------------------------------------------------------------------------------------
}
