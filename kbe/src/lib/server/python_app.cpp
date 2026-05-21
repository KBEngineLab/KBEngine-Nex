// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com


#include "python_app.h"
#include "asyncio_helper.h"
#include "pyscript/py_memorystream.h"
#include "server/py_file_descriptor.h"
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <cstdlib>

namespace KBEngine{

KBEngine::ScriptTimers KBEngine::PythonApp::scriptTimers_;

namespace
{

// 将customCfg中的type统一转成小写，避免配置里写Int、FLOAT等大小写差异导致解析失败。
std::string normalizeCustomCfgType(const std::string& type)
{
	std::string lowerType = type;
	std::transform(lowerType.begin(), lowerType.end(), lowerType.begin(), [](unsigned char ch) {
		return static_cast<char>(std::tolower(ch));
	});
	return lowerType;
}

// 解析bool配置值。这里显式支持true/false、1/0、yes/no，便于配置文件保持可读性。
// 解析失败时返回false，并由调用方决定是否返回None和输出具体错误。
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

// 解析float配置值。使用strtod并检查尾部字符，避免"3.5abc"这类配置被静默截断为3.5。
bool parseCustomCfgFloat(const std::string& value, double& result)
{
	char* end = NULL;
	errno = 0;
	result = std::strtod(value.c_str(), &end);
	return end != value.c_str() && end != NULL && *end == '\0' && errno != ERANGE;
}

// dict/list使用Python标准库ast.literal_eval解析。
// 这样脚本可以按Python字面量书写{"rate": 1.2}或[1, 2, 3]，同时避免eval执行任意代码。
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
		ERROR_MSG(fmt::format("KBEngine::getCustomCfg(): customCfg[{}] unable to build unicode value, value={}.\n",
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
		ERROR_MSG(fmt::format("KBEngine::getCustomCfg(): customCfg[{}] value parse failed, type={}, value={}.\n",
			item.name, item.type, item.value));
		PyErr_PrintEx(0);
		Py_RETURN_NONE;
	}

	// 配置声明为dict/list时，解析结果必须严格匹配声明类型，避免写错后脚本拿到意外对象。
	if(strcmp(expectedType, "dict") == 0 && !PyDict_Check(pyValue))
	{
		ERROR_MSG(fmt::format("KBEngine::getCustomCfg(): customCfg[{}] expects dict, value={}.\n",
			item.name, item.value));
		Py_DECREF(pyValue);
		Py_RETURN_NONE;
	}

	if(strcmp(expectedType, "list") == 0 && !PyList_Check(pyValue))
	{
		ERROR_MSG(fmt::format("KBEngine::getCustomCfg(): customCfg[{}] expects list, value={}.\n",
			item.name, item.value));
		Py_DECREF(pyValue);
		Py_RETURN_NONE;
	}

	return pyValue;
}

// 按XML中声明的type把字符串配置转换成Python对象。
// default参数只用于key不存在时的兜底值，不参与已存在配置项的类型推断，避免不同脚本传不同default导致类型不一致。
PyObject* customCfgItemToPyObject(const ServerConfig::CustomCfgItem& item)
{
	std::string type = normalizeCustomCfgType(item.type);

	if(type == "bool")
	{
		bool value = false;
		if(!parseCustomCfgBool(item.value, value))
		{
			ERROR_MSG(fmt::format("KBEngine::getCustomCfg(): customCfg[{}] bool parse failed, value={}.\n",
				item.name, item.value));
			Py_RETURN_NONE;
		}

		if(value)
			Py_RETURN_TRUE;

		Py_RETURN_FALSE;
	}

	if(type == "int")
	{
		// PyLong_FromString负责生成Python int，同时比atoi更严格，非法输入不会被静默转换成0。
		char* end = NULL;
		PyObject* pyValue = PyLong_FromString(const_cast<char*>(item.value.c_str()), &end, 10);
		if(pyValue == NULL || end == NULL || *end != '\0')
		{
			Py_XDECREF(pyValue);
			PyErr_Clear();
			ERROR_MSG(fmt::format("KBEngine::getCustomCfg(): customCfg[{}] int parse failed, value={}.\n",
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
			ERROR_MSG(fmt::format("KBEngine::getCustomCfg(): customCfg[{}] float parse failed, value={}.\n",
				item.name, item.value));
			Py_RETURN_NONE;
		}

		return PyFloat_FromDouble(value);
	}

	if(type == "string" || type == "str")
	{
		return PyUnicode_FromString(item.value.c_str());
	}

	if(type == "dict")
	{
		return parseCustomCfgLiteral(item, "dict");
	}

	if(type == "list")
	{
		return parseCustomCfgLiteral(item, "list");
	}

	ERROR_MSG(fmt::format("KBEngine::getCustomCfg(): customCfg[{}] unsupported type={}, value={}.\n",
		item.name, item.type, item.value));
	Py_RETURN_NONE;
}

}

/**
内部定时器处理类
*/
class ScriptTimerHandler : public TimerHandler
{
public:
	ScriptTimerHandler(ScriptTimers* scriptTimers, PyObject * callback) :
		pyCallback_(callback),
		scriptTimers_(scriptTimers)
	{
		Py_INCREF(pyCallback_);
	}

	~ScriptTimerHandler()
	{
		Py_DECREF(pyCallback_);
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

	PyObject* pyCallback_;
	ScriptTimers* scriptTimers_;
};

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

	// PythonApp类组件在主线程安装底层dispatcher timer，用来周期性推进协程。
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
	// 先关闭asyncio，取消未完成协程，避免卸载Python脚本时仍有Task持有对象。
	AsyncioHelper::shutdown();

	// 再取消普通脚本timer并释放Python脚本环境。
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

	std::wstring pyPaths = user_scripts_path + L"common;";
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

	// 获取自定义配置参数
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
		PyErr_Format(PyExc_TypeError, "KBEngine::getCustomCfg(): requires 1 or 2 args (key[, default])!");
		return NULL;
	}

	const char* key = NULL;
	PyObject* pyDefault = NULL;

	// default是可选参数：key不存在时返回default；如果没有传default，则按需求返回Python None。
	// 已存在的配置项始终使用XML里的type转换，不再通过default类型推断。
	if(!PyArg_ParseTuple(args, "s|O", &key, &pyDefault))
	{
		PyErr_Format(PyExc_TypeError, "KBEngine::getCustomCfg(): args error!");
		return NULL;
	}

	const std::map<std::string, ServerConfig::CustomCfgItem>& cfg = g_kbeSrvConfig.customCfg();
	auto it = cfg.find(key);

	if(it == cfg.end())
	{
		if(pyDefault != NULL)
		{
			Py_INCREF(pyDefault);
			return pyDefault;
		}

		Py_RETURN_NONE;
	}

	return customCfgItemToPyObject(it->second);
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

//-------------------------------------------------------------------------------------
void PythonApp::startProfile_(Network::Channel* pChannel, std::string profileName, 
	int8 profileType, uint32 timelen)
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

//-------------------------------------------------------------------------------------
void PythonApp::onExecScriptCommand(Network::Channel* pChannel, KBEngine::MemoryStream& s)
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
	onReloadScript(fullReload);

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
