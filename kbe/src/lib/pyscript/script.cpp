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


#include "script.h"
#include "math.h"
#include "pickler.h"
#include "pyprofile.h"
#include "copy.h"
#include "pystruct.h"
#include "py_gc.h"
#include "pyurl.h"
#include "py_compression.h"
#include "py_platform.h"
#include "resmgr/resmgr.h"
#include "thread/concurrency.h"

#include <fstream>

#ifndef CODE_INLINE
#include "script.inl"
#endif

namespace KBEngine{ 

KBE_SINGLETON_INIT(script::Script);
namespace script{

namespace
{

// 控制台单行需要保留 REPL 回显，例如输入 "a" 时输出 repr(a)；多行块必须按脚本文件编译，
// 否则 "def ...\n...\nprint(...)" 会被 Py_single_input 当成非法交互输入。
// Console single-line input must keep REPL echo semantics, while multi-line blocks must be
// compiled as file input so compound statements followed by more statements are accepted.
int getConsoleCompileMode(const char* command)
{
	if (command == NULL)
		return Py_single_input;

	std::string text = command;
	while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '\t'))
	{
		text.pop_back();
	}

	return text.find('\n') == std::string::npos ? Py_single_input : Py_file_input;
}

//-------------------------------------------------------------------------------------
bool isCompatibleVenvRoot(const std::string& rootPath)
{
	std::ifstream config((rootPath + "/pyvenv.cfg").c_str());
	if (!config.is_open())
		return false;

	const std::string expectedVersion = fmt::format("{}.{}", PY_MAJOR_VERSION, PY_MINOR_VERSION);
	std::string line;
	while (std::getline(config, line))
	{
		const std::string::size_type separator = line.find('=');
		if (separator == std::string::npos)
			continue;

		const std::string key = strutil::kbe_trim(line.substr(0, separator));
		if (key != "version" && key != "version_info")
			continue;

		const std::string version = strutil::kbe_trim(line.substr(separator + 1));
		const bool matches = version.compare(0, expectedVersion.size(), expectedVersion) == 0 &&
			(version.size() == expectedVersion.size() || version[expectedVersion.size()] == '.');
		if (!matches)
		{
			/*
				venv 的主次版本必须与嵌入式 Python 一致，否则同目录中的 cpXY 二进制扩展会在运行时失败。
				The venv major/minor version must match embedded Python or cpXY binary extensions in that directory will fail at runtime.
			*/
			WARNING_MSG(fmt::format("Script::install(): ignoring incompatible venv, path={}, expected={}, actual={}\n",
				rootPath, expectedVersion, version));
		}

		return matches;
	}

	WARNING_MSG(fmt::format("Script::install(): pyvenv.cfg has no version, path={}\n", rootPath));
	return false;
}

//-------------------------------------------------------------------------------------
std::vector<std::wstring> resolveVenvSitePackages(const std::string& configuredPaths)
{
	std::vector<std::wstring> resolvedPaths;
	if (configuredPaths.empty())
		return resolvedPaths;

	std::vector<std::string> entries;
	strutil::kbe_split<char>(configuredPaths, ';', entries);
#if KBE_PLATFORM != PLATFORM_WIN32
	if (entries.size() < 2)
	{
		entries.clear();
		strutil::kbe_split<char>(configuredPaths, ':', entries);
	}
#endif

	for (std::vector<std::string>::iterator iter = entries.begin(); iter != entries.end(); ++iter)
	{
		std::string path = strutil::kbe_trim(*iter);
		if (path.empty())
			continue;

		strutil::kbe_replace(path, "\\", "/");
		while (!path.empty() && path[path.size() - 1] == '/')
			path.erase(path.size() - 1);

		/*
			显式配置可继续传入 Nex 兼容的 site-packages 目录；自动发现的 venv 根目录则按当前 Python ABI 转换。
			Explicit configuration may keep passing a Nex-compatible site-packages directory, while an auto-detected venv root is converted using the active Python ABI.
		*/
		if (access((path + "/pyvenv.cfg").c_str(), 0) == 0)
		{
			if (!isCompatibleVenvRoot(path))
				continue;

#if KBE_PLATFORM == PLATFORM_WIN32
			path += "/Lib/site-packages";
#else
			path += fmt::format("/lib/python{}.{}/site-packages", PY_MAJOR_VERSION, PY_MINOR_VERSION);
#endif
		}

		if (access(path.c_str(), 0) != 0)
		{
			WARNING_MSG(fmt::format("Script::install(): ignoring missing venv site-packages path={}\n", path));
			continue;
		}

		wchar_t* widePath = strutil::char2wchar(const_cast<char*>(path.c_str()));
		if (widePath != NULL)
		{
			resolvedPaths.push_back(widePath);
			free(widePath);
		}
	}

	return resolvedPaths;
}

//-------------------------------------------------------------------------------------
bool processVenvSitePackages(const std::vector<std::wstring>& paths)
{
	if (paths.empty())
		return true;

	PyObject* siteModule = PyImport_ImportModule("site");
	if (siteModule == NULL)
		return false;

	PyObject* addSiteDir = PyObject_GetAttrString(siteModule, "addsitedir");
	Py_DECREF(siteModule);
	if (addSiteDir == NULL)
		return false;

	for (std::vector<std::wstring>::const_iterator iter = paths.begin(); iter != paths.end(); ++iter)
	{
		PyObject* path = PyUnicode_FromWideChar(iter->c_str(), static_cast<Py_ssize_t>(iter->size()));
		if (path == NULL)
		{
			Py_DECREF(addSiteDir);
			return false;
		}

		PyObject* result = PyObject_CallFunctionObjArgs(addSiteDir, path, NULL);
		Py_DECREF(path);
		if (result == NULL)
		{
			Py_DECREF(addSiteDir);
			return false;
		}

		Py_DECREF(result);
	}

	Py_DECREF(addSiteDir);
	return true;
}

}

//-------------------------------------------------------------------------------------
static PyObject* __py_genUUID64(PyObject *self, void *closure)
{
	static int8 check = -1;

	if(check < 0)
	{
		if(g_componentGlobalOrder <= 0 || g_componentGlobalOrder > 65535)
		{
			WARNING_MSG(fmt::format("globalOrder({}) is not in the range of 0~65535, genUUID64 is not safe, "
				"in the multi process may be repeated.\n", g_componentGlobalOrder));
		}

		check = 1;
	}

	return PyLong_FromUnsignedLongLong(genUUID64());
}

//-------------------------------------------------------------------------------------
PyObject * PyTuple_FromStringVector(const std::vector< std::string > & v)
{
	const Py_ssize_t sz = static_cast<Py_ssize_t>(v.size());
	PyObject * t = PyTuple_New( sz );
	for (Py_ssize_t i = 0; i < sz; ++i)
	{
		PyTuple_SetItem( t, i, PyUnicode_FromString( v[i].c_str() ) );
	}

	return t;
}

//-------------------------------------------------------------------------------------
Script::Script():
module_(NULL),
extraModule_(NULL),
sysInitModules_(NULL),
pyStdouterr_(NULL)
{
}

//-------------------------------------------------------------------------------------
Script::~Script()
{
}

//-------------------------------------------------------------------------------------
int Script::run_simpleString(const char* command, std::string* retBufferPtr)
{
	if(command == NULL)
	{
		ERROR_MSG("Script::Run_SimpleString: command is NULL!\n");
		return 0;
	}

	ScriptStdOutErrHook* pStdouterrHook = new ScriptStdOutErrHook();

	if(retBufferPtr != NULL)
	{
		DebugHelper::getSingleton().resetScriptMsgType();
		if(!pStdouterrHook->install()){												
			ERROR_MSG("Script::Run_SimpleString: pyStdouterrHook_->install() is failed!\n");
			SCRIPT_ERROR_CHECK();
			delete pStdouterrHook;
			return -1;
		}
			
		pStdouterrHook->setHookBuffer(retBufferPtr);
		//PyRun_SimpleString(command);

		PyObject *m, *d, *v;
		m = PyImport_AddModule("__main__");
		if (m == NULL)
		{
			SCRIPT_ERROR_CHECK();
			pStdouterrHook->uninstall();
			delete pStdouterrHook;
			return -1;
		}

		d = PyModule_GetDict(m);

		v = PyRun_String(command, getConsoleCompileMode(command), d, d);
		if (v == NULL) 
		{
			PyErr_Print();
			pStdouterrHook->uninstall();
			delete pStdouterrHook;
			return -1;
		}

		Py_DECREF(v);
		SCRIPT_ERROR_CHECK();
		
		pStdouterrHook->uninstall();
		delete pStdouterrHook;
		return 0;
	}

	PyRun_SimpleString(command);

	SCRIPT_ERROR_CHECK();
	delete pStdouterrHook;
	return 0;
}

//-------------------------------------------------------------------------------------
bool Script::install(const wchar_t* pythonHomeDir, std::wstring pyPaths,
	const char* moduleName, COMPONENT_TYPE componentType)
{
	const std::vector<std::wstring> venvSitePackages = resolveVenvSitePackages(
		Resmgr::getSingleton().getEnv().venv_path);
	for (std::vector<std::wstring>::const_iterator iter = venvSitePackages.begin(); iter != venvSitePackages.end(); ++iter)
	{
		pyPaths += *iter;
		pyPaths += L";";
	}

	APPEND_PYSYSPATH(pyPaths);

#if KBE_PLATFORM != PLATFORM_WIN32
	strutil::kbe_replace(pyPaths, L";", L":");
#endif

	char* tmpchar = strutil::wchar2char(const_cast<wchar_t*>(pyPaths.c_str()));
	DEBUG_MSG(fmt::format("Script::install(): paths={}.\n", tmpchar));
	free(tmpchar);

	/*
		隔离配置阻止宿主机的 PYTHONHOME、PYTHONPATH 和 site.py 改变服务器脚本搜索路径。
		The isolated configuration prevents host PYTHONHOME, PYTHONPATH, and site.py from changing server script lookup.
	*/
	PyConfig config;
	PyConfig_InitIsolatedConfig(&config);
	config.site_import = 0;
	config.use_environment = 0;
	config.parse_argv = 0;
	config.install_signal_handlers = 0;
	config.module_search_paths_set = 1;

	PyStatus status = PyConfig_SetString(&config, &config.home, pythonHomeDir);
	if (!PyStatus_Exception(status))
	{
#if KBE_PLATFORM == PLATFORM_WIN32
		const wchar_t pathSeparator = L';';
#else
		const wchar_t pathSeparator = L':';
#endif

		std::wstring::size_type pathBegin = 0;
		while (pathBegin <= pyPaths.size() && !PyStatus_Exception(status))
		{
			const std::wstring::size_type pathEnd = pyPaths.find(pathSeparator, pathBegin);
			const std::wstring path = pyPaths.substr(pathBegin, pathEnd - pathBegin);
			if (!path.empty())
				status = PyWideStringList_Append(&config.module_search_paths, path.c_str());

			if (pathEnd == std::wstring::npos)
				break;

			pathBegin = pathEnd + 1;
		}
	}

	/*
		sys.argv 保留一个空的程序名，避免部分第三方包假设 argv[0] 存在时越界。
		sys.argv keeps an empty program name so third-party packages that assume argv[0] exists remain safe.
	*/
	if (!PyStatus_Exception(status))
	{
		wchar_t emptyProgramName[] = L"";
		wchar_t* argv[] = { emptyProgramName };
		status = PyConfig_SetArgv(&config, 1, argv);
	}

	if (!PyStatus_Exception(status))
		status = Py_InitializeFromConfig(&config);

	if (PyStatus_Exception(status))
	{
		ERROR_MSG(fmt::format("Script::install(): Python initialization failed, function={}, error={}\n",
			status.func ? status.func : "unknown", status.err_msg ? status.err_msg : "unknown"));
		PyConfig_Clear(&config);
		return false;
	}

	PyConfig_Clear(&config);
	if (!Py_IsInitialized())
	{
		ERROR_MSG("Script::install(): Py_InitializeFromConfig did not initialize Python!\n");
		return false;
	}

	sysInitModules_ = PyDict_Copy(PySys_GetObject("modules"));

	PyObject *m = PyImport_AddModule("__main__");

	// 添加一个脚本基础模块
	module_ = PyImport_AddModule(moduleName);
	if (module_ == NULL)
		return false;
	
	const char* componentName = COMPONENT_NAME_EX(componentType);
	if (PyModule_AddStringConstant(module_, "component", componentName))
	{
		ERROR_MSG(fmt::format("Script::install(): Unable to set KBEngine.component to {}\n",
			componentName));
		return false;
	}
	
	// 注册产生uuid方法到py
	APPEND_SCRIPT_MODULE_METHOD(module_,		genUUID64,			__py_genUUID64,					METH_VARARGS,			0);

	// 安装py重定向模块
	ScriptStdOut::installScript(NULL);
	ScriptStdErr::installScript(NULL);

	// 将模块对象加入main
	PyObject_SetAttrString(m, moduleName, module_);	
	PyObject* pyDoc = PyUnicode_FromString("This module is created by KBEngine!");
	PyObject_SetAttrString(module_, "__doc__", pyDoc);
	Py_DECREF(pyDoc);

	// 重定向python输出
	pyStdouterr_ = new ScriptStdOutErr();
	
	// 安装py重定向脚本模块
	if(!pyStdouterr_->install()){
		ERROR_MSG("Script::install::pyStdouterr_->install() is failed!\n");
		delete pyStdouterr_;
		SCRIPT_ERROR_CHECK();
		return false;
	}

	/*
		只处理底层确认的 venv 目录，以支持 pip 生成的 .pth 和 editable install，同时不启用全局 site.py 扫描。
		Process only the venv directories confirmed by the engine to support pip .pth and editable installs without enabling global site.py scanning.
	*/
	if (!processVenvSitePackages(venvSitePackages))
	{
		ERROR_MSG("Script::install(): failed to process venv site-packages.\n");
		SCRIPT_ERROR_CHECK();
		return false;
	}

	PyGC::initialize();
	Pickler::initialize();
	PyProfile::initialize(this);
	PyStruct::initialize();
	Copy::initialize();
	PyUrl::initialize(this);
	PyCompression::initialize();
	PyPlatform::initialize();
	SCRIPT_ERROR_CHECK();

	math::installModule("Math");
	INFO_MSG(fmt::format("Script::install(): is successfully, Python=({})!\n", Py_GetVersion()));
	return installExtraModule("KBExtra");
}

//-------------------------------------------------------------------------------------
bool Script::uninstall()
{
	math::uninstallModule();
	Pickler::finalise();
	PyProfile::finalise();
	PyStruct::finalise();
	Copy::finalise();
	PyUrl::finalise();
	PyCompression::finalise();
	PyPlatform::finalise();
	SCRIPT_ERROR_CHECK();

	if(pyStdouterr_)
	{
		if(pyStdouterr_->isInstall() && !pyStdouterr_->uninstall())	{
			ERROR_MSG("Script::uninstall(): pyStdouterr_->uninstall() is failed!\n");
		}
		
		delete pyStdouterr_;
	}

	ScriptStdOut::uninstallScript();
	ScriptStdErr::uninstallScript();

	PyGC::finalise();

	if (sysInitModules_)
	{
		Py_DECREF(sysInitModules_);
		sysInitModules_ = NULL;
	}

	// 卸载python解释器
	Py_Finalize();

	INFO_MSG("Script::uninstall(): is successfully!\n");
	return true;	
}

//-------------------------------------------------------------------------------------
bool Script::installExtraModule(const char* moduleName)
{
	PyObject *m = PyImport_AddModule("__main__");

	// 添加一个脚本扩展模块
	extraModule_ = PyImport_AddModule(moduleName);
	if (extraModule_ == NULL)
		return false;

	// 将扩展模块对象加入main
	PyObject_SetAttrString(m, moduleName, extraModule_);

	INFO_MSG(fmt::format("Script::install(): {} is successfully!\n", moduleName));
	return true;
}

//-------------------------------------------------------------------------------------
bool Script::registerExtraMethod(const char* attrName, PyMethodDef* pyFunc)
{
	return PyModule_AddObject(extraModule_, attrName, PyCFunction_New(pyFunc, NULL)) != -1;
}

//-------------------------------------------------------------------------------------
bool Script::registerExtraObject(const char* attrName, PyObject* pyObj)
{
	return PyObject_SetAttrString(extraModule_, attrName, pyObj) != -1;
}

//-------------------------------------------------------------------------------------
int Script::registerToModule(const char* attrName, PyObject* pyObj)
{
	return PyObject_SetAttrString(module_, attrName, pyObj);
}

//-------------------------------------------------------------------------------------
int Script::unregisterToModule(const char* attrName)
{
	if(module_ == NULL || attrName == NULL)
		return 0;

	return PyObject_DelAttrString(module_, attrName);
}

//-------------------------------------------------------------------------------------
void Script::setenv(const std::string& name, const std::string& value)
{
	PyObject* osModule = PyImport_ImportModule("os");

	if(osModule)
	{
		PyObject* py_environ = NULL;
		PyObject* py_name = NULL;
		PyObject* py_value = NULL;

		PyObject* supports_bytes_environ = PyObject_GetAttrString(osModule, "supports_bytes_environ");
		if(Py_True == supports_bytes_environ)
		{
			py_environ = PyObject_GetAttrString(osModule, "environb");
			py_name = PyBytes_FromString(name.c_str());
			py_value = PyBytes_FromString(value.c_str());
		}
		else
		{
			py_environ = PyObject_GetAttrString(osModule, "environ");
			py_name = PyUnicode_FromString(name.c_str());
			py_value = PyUnicode_FromString(value.c_str());
		}

		Py_DECREF(supports_bytes_environ);
		Py_DECREF(osModule);

		if (!py_environ)
		{
			ERROR_MSG("Script::setenv: get os.environ error!\n");
			PyErr_PrintEx(0);
			Py_DECREF(py_value);
			Py_DECREF(py_name);
			return;
		}

		PyObject* environData = PyObject_GetAttrString(py_environ, "_data");
		if (!environData)
		{
			ERROR_MSG("Script::setenv: os.environ._data not exist!\n");
			PyErr_PrintEx(0);
			Py_DECREF(py_value);
			Py_DECREF(py_name);
			Py_DECREF(py_environ);
			return;
		}

		int ret = PyDict_SetItem(environData, py_name, py_value);
		
		Py_DECREF(environData);
		Py_DECREF(py_environ);
		Py_DECREF(py_value);
		Py_DECREF(py_name);
		
		if(ret == -1)
		{
			ERROR_MSG("Script::setenv: get os.environ error!\n");
			PyErr_PrintEx(0);
			return;
		}
	}
}

//-------------------------------------------------------------------------------------

}
}
