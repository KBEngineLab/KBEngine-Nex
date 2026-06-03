// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com


#include "entitydef.h"
#include "scriptdef_module.h"
#include "datatypes.h"
#include "common.h"
#include "py_entitydef.h"
#include "entity_component.h"
#include "pyscript/py_memorystream.h"
#include "resmgr/resmgr.h"
#include "resmgr/plugins/plugin_manager.h"
#include "common/smartpointer.h"
#include "entitydef/volatileinfo.h"
#include "entitydef/entity_call.h"
#include "entitydef/entity_component_call.h"
#include "pyscript/py_platform.h"
#include <algorithm>
#include <set>
#include <sys/stat.h>

#ifndef CODE_INLINE
#include "entitydef.inl"
#endif

namespace KBEngine{

namespace
{

std::string normalizePluginPath(std::string path)
{
	std::replace(path.begin(), path.end(), '\\', '/');
	return path;
}

bool pluginFileExists(const std::string& path)
{
	return access(path.c_str(), 0) == 0;
}

bool pluginEntityScriptExists(const PluginEntityDescriptor& entity, const std::string& folder)
{
	std::string file = normalizePluginPath(entity.pluginRootPath + "/" + folder + "/" + entity.name + ".py");
	return pluginFileExists(file) || pluginFileExists(file + "c");
}

std::string findPluginComponentDefFile(const std::string& componentTypeName, std::string* pluginDefFilePath)
{
	const std::vector<PluginDescriptor>& plugins = PluginManager::instance().plugins();
	for (std::vector<PluginDescriptor>::const_iterator iter = plugins.begin(); iter != plugins.end(); ++iter)
	{
		std::string defFile = normalizePluginPath(iter->rootPath + "/entity_defs/components/" + componentTypeName + ".def");
		if (!pluginFileExists(defFile))
			continue;

		if (pluginDefFilePath)
			*pluginDefFilePath = normalizePluginPath(iter->rootPath + "/entity_defs/");

		INFO_MSG(fmt::format("EntityDef::loadComponents: use plugin component def [{}] from plugin [{}].\n",
			defFile, iter->name));
		return defFile;
	}

	return "";
}

}

std::vector<ScriptDefModulePtr>	EntityDef::__scriptModules;
std::vector<ScriptDefModulePtr>	EntityDef::__oldScriptModules;

std::map<std::string, ENTITY_SCRIPT_UID> EntityDef::__scriptTypeMappingUType;
std::map<std::string, ENTITY_SCRIPT_UID> EntityDef::__oldScriptTypeMappingUType;

COMPONENT_TYPE EntityDef::__loadComponentType;
std::vector<PyTypeObject*> EntityDef::__scriptBaseTypes;
std::string EntityDef::__entitiesPath;

KBE_MD5 EntityDef::__md5;
bool EntityDef::_isInit = false;
bool g_isReload = false;

bool EntityDef::__entityAliasID = false;
bool EntityDef::__entitydefAliasID = false;

EntityDef::Context EntityDef::__context;

// 方法产生时自动产生utype用的
ENTITY_METHOD_UID g_methodUtypeAuto = 1;
std::vector<ENTITY_METHOD_UID> g_methodCusUtypes;

ENTITY_PROPERTY_UID g_propertyUtypeAuto = 1;
std::vector<ENTITY_PROPERTY_UID> g_propertyUtypes;

// 产生新的脚本模块时自动产生utype
ENTITY_SCRIPT_UID g_scriptUtype = 1;

// 获得某个entity的函数地址
EntityDef::GetEntityFunc EntityDef::__getEntityFunc;

static std::map<std::string, std::vector<PropertyDescription*> > g_logComponentPropertys;
// 记录脚本模块文件的版本戳。key 使用 Python 模块名，例如 Avatar、interfaces.Teleport。
// 初始化加载时建立基线；热更时只有版本戳变化的模块才真正 PyImport_ReloadModule。
static std::map<std::string, uint64> g_scriptModuleStamps;
static std::map<std::string, uint64> g_scriptFileStamps;
// 本轮 reload 中实际检测到变更并执行 reload 的脚本文件列表，用于最终日志输出。
static std::vector<std::string> g_reloadChangedFiles;
// 本轮 reload 中检查过但未变化的脚本文件列表，只在汇总中输出数量，避免日志刷屏。
static std::vector<std::string> g_reloadSkippedFiles;
// changed/skipped 日志按物理文件去重。历史脚本可能同时存在 interfaces.Teleport 和 Teleport 两个模块名，
// 它们指向同一个 Teleport.py；reload 可以分别处理两个模块对象，但汇总日志只展示一个文件。
static std::set<std::string> g_reloadChangedFileKeys;
static std::set<std::string> g_reloadSkippedFileKeys;
// 本轮 reload 的结构化统计。EntityDef::reload 返回它，上层据此决定是否继续刷新在线对象。
static ReloadScriptDefStats g_reloadStats;

//-------------------------------------------------------------------------------------
static uint64 getScriptFileStamp(const std::string& filePath)
{
	// 使用“最后修改时间 + 文件大小”作为热更判断依据。
	// Windows 下优先读取 FILETIME，精度高于 stat 的秒级 st_mtime，避免连续快速保存时漏判。
#if KBE_PLATFORM == PLATFORM_WIN32
	WIN32_FILE_ATTRIBUTE_DATA fileInfo;
	if (GetFileAttributesExA(filePath.c_str(), GetFileExInfoStandard, &fileInfo))
	{
		ULARGE_INTEGER mtime;
		mtime.HighPart = fileInfo.ftLastWriteTime.dwHighDateTime;
		mtime.LowPart = fileInfo.ftLastWriteTime.dwLowDateTime;

		ULARGE_INTEGER size;
		size.HighPart = fileInfo.nFileSizeHigh;
		size.LowPart = fileInfo.nFileSizeLow;

		return (uint64)(mtime.QuadPart ^ (size.QuadPart << 1));
	}
#endif

	// 非 Windows 平台保留 stat 兜底；加入 st_size 后，即使同一秒内保存且大小变化也能识别。
	struct stat st;
	if (stat(filePath.c_str(), &st) != 0)
		return 0;

	return ((uint64)st.st_mtime << 32) ^ (uint64)st.st_size;
}

//-------------------------------------------------------------------------------------
static std::string getPyModuleFilePath(PyObject* pyModule)
{
	// 从 Python 模块对象拿到真实文件路径。这里统一转为 / 分隔，方便后续日志和路径比较。
	// 失败时返回空字符串，调用方会保守地认为模块需要 reload。
	PyObject* fileobj = PyModule_GetFilenameObject(pyModule);
	if (!fileobj)
	{
		PyErr_Clear();
		return "";
	}

	const char* filePath = PyUnicode_AsUTF8(fileobj);
	std::string result = filePath ? filePath : "";
	Py_DECREF(fileobj);

	if (result.size() > 0)
		result = normalizePluginPath(result);

	return result;
}

//-------------------------------------------------------------------------------------
static bool isTrackedScriptFileChanged(const std::string& moduleName, const std::string& filePath, uint64& currStamp, bool& firstTrack)
{
	// 返回 true 表示需要 reload。
	// 如果模块第一次进入跟踪表，只记录当前文件版本戳作为基线，不把它当成变更。
	// 这样第一次 reloadScript(False) 不会因为“没有历史记录”而把所有已加载依赖都 reload 一遍。
	// 正常启动加载完成后会提前建立依赖模块基线；这里仍然保留懒加载兜底，覆盖后续才 import 的脚本。
	firstTrack = false;
	currStamp = getScriptFileStamp(filePath);
	if (currStamp == 0)
		return true;

	std::map<std::string, uint64>::iterator iter = g_scriptModuleStamps.find(moduleName);
	if (iter == g_scriptModuleStamps.end())
	{
		g_scriptModuleStamps[moduleName] = currStamp;
		std::map<std::string, uint64>::iterator fileIter = g_scriptFileStamps.find(filePath);
		if (fileIter != g_scriptFileStamps.end() && fileIter->second != currStamp)
		{
			fileIter->second = currStamp;
			return true;
		}

		g_scriptFileStamps[filePath] = currStamp;
		firstTrack = true;
		return false;
	}

	if (iter->second != currStamp)
	{
		iter->second = currStamp;
		g_scriptFileStamps[filePath] = currStamp;
		return true;
	}

	g_scriptFileStamps[filePath] = currStamp;
	return false;
}

//-------------------------------------------------------------------------------------
static void rememberReloadFile(bool changed, const std::string& moduleName, const std::string& filePath)
{
	// 统一记录本轮 reload 的文件检查结果。changed 列表会逐条打印，
	// skipped 列表只统计数量，避免每次热更输出大量未变化文件。
	// key 优先使用文件路径，保证同一个 .py 被多个模块名引用时只在汇总里出现一次。
	std::string key = filePath.empty() ? moduleName : filePath;
	std::string text = fmt::format("{} ({})", filePath, moduleName);
	if (changed)
	{
		if (g_reloadChangedFileKeys.insert(key).second)
			g_reloadChangedFiles.push_back(text);
	}
	else
	{
		if (g_reloadSkippedFileKeys.insert(key).second)
			g_reloadSkippedFiles.push_back(text);
	}
}

//-------------------------------------------------------------------------------------
static std::string getPythonModuleLeafName(const std::string& moduleName)
{
	// 将 interfaces.Teleport 规约为 Teleport，用来兼容历史脚本的裸 import。
	// 同一个文件可能被 Python 以 Teleport 和 interfaces.Teleport 两个名字加载，
	// 类对象的 __module__ 不一定和当前遍历到的模块名完全一致。
	std::string::size_type pos = moduleName.find_last_of('.');
	return pos == std::string::npos ? moduleName : moduleName.substr(pos + 1);
}

//-------------------------------------------------------------------------------------
static void collectModuleTypes(PyObject* pyModule, const std::string& moduleName, std::map<std::string, PyObject*>& oldTypes)
{
	// reload dependency module 前收集旧类对象。
	// 例如 Avatar 继承旧的 interfaces.Teleport.Teleport；只 reload Teleport.py 时，
	// Avatar 的基类指针不会自动变成新类对象，所以后续需要把新类属性拷贝回旧类对象。
	// 这里兼容两种模块名：完整包名 interfaces.Teleport 和历史裸名 Teleport。
	std::string moduleLeafName = getPythonModuleLeafName(moduleName);
	PyObject* pyDict = PyModule_GetDict(pyModule);
	if (!pyDict)
		return;

	PyObject* key = NULL;
	PyObject* value = NULL;
	Py_ssize_t pos = 0;
	while (PyDict_Next(pyDict, &pos, &key, &value))
	{
		if (!PyUnicode_Check(key) || !PyType_Check(value))
			continue;

		PyObject* pyTypeModule = PyObject_GetAttrString(value, "__module__");
		if (!pyTypeModule)
		{
			PyErr_Clear();
			continue;
		}

		const char* typeModule = PyUnicode_AsUTF8(pyTypeModule);
		std::string typeModuleName = typeModule ? typeModule : "";
		bool sameModule = typeModuleName == moduleName ||
			getPythonModuleLeafName(typeModuleName) == moduleLeafName;
		Py_DECREF(pyTypeModule);

		if (!sameModule)
			continue;

		const char* name = PyUnicode_AsUTF8(key);
		if (!name)
		{
			PyErr_Clear();
			continue;
		}

		Py_INCREF(value);
		oldTypes[name] = value;
	}
}

//-------------------------------------------------------------------------------------
static bool shouldSkipTypeAttr(const std::string& attrName)
{
	// __dict__/__weakref__ 是 Python 类型对象上的特殊描述符，不应从新类拷贝覆盖旧类。
	return attrName == "__dict__" || attrName == "__weakref__";
}

//-------------------------------------------------------------------------------------
static bool shouldReportStaleTypeAttr(const std::string& attrName)
{
	// 删除方法时旧类上会保留旧属性。为了让开发者知道“删除没有真正生效”，
	// 只报告普通业务属性/方法；Python 内部双下划线属性噪声较大，不计入 stale attrs。
	return !(attrName.size() >= 4 &&
		attrName[0] == '_' && attrName[1] == '_' &&
		attrName[attrName.size() - 1] == '_' && attrName[attrName.size() - 2] == '_');
}

//-------------------------------------------------------------------------------------
static void patchOldTypeFromNewType(PyObject* pyOldType, PyObject* pyNewType)
{
	// 将 reload 后新类对象上的属性同步到 reload 前的旧类对象。
	// 这样未修改的 Entity 主脚本不需要额外 reload，也能通过旧基类对象拿到新的 interface 方法实现。
	// 注意这里主要解决“方法替换/新增”的场景；删除旧方法不会强行从旧类删除，以降低破坏性。
	PyObject* pyNewDict = PyObject_GetAttrString(pyNewType, "__dict__");
	if (!pyNewDict)
	{
		PyErr_Clear();
		return;
	}

	PyObject* pyItems = PyMapping_Items(pyNewDict);
	if (!pyItems)
	{
		Py_DECREF(pyNewDict);
		PyErr_Clear();
		return;
	}

	Py_ssize_t size = PyList_Size(pyItems);
	for (Py_ssize_t i = 0; i < size; ++i)
	{
		PyObject* pyItem = PyList_GetItem(pyItems, i);
		if (!pyItem || !PyTuple_Check(pyItem) || PyTuple_Size(pyItem) != 2)
			continue;

		PyObject* pyKey = PyTuple_GET_ITEM(pyItem, 0);
		PyObject* pyValue = PyTuple_GET_ITEM(pyItem, 1);
		if (!PyUnicode_Check(pyKey))
			continue;

		const char* key = PyUnicode_AsUTF8(pyKey);
		if (!key)
		{
			PyErr_Clear();
			continue;
		}

		if (shouldSkipTypeAttr(key))
			continue;

		if (PyObject_SetAttrString(pyOldType, key, pyValue) == -1)
		{
			WARNING_MSG(fmt::format("EntityDef::patchOldTypeFromNewType: set attr({}) failed.\n", key));
			PyErr_Clear();
		}
	}

	Py_DECREF(pyItems);

	PyObject* pyOldDict = PyObject_GetAttrString(pyOldType, "__dict__");
	if (pyOldDict)
	{
		PyObject* pyOldItems = PyMapping_Items(pyOldDict);
		if (pyOldItems)
		{
			Py_ssize_t oldSize = PyList_Size(pyOldItems);
			for (Py_ssize_t i = 0; i < oldSize; ++i)
			{
				PyObject* pyItem = PyList_GetItem(pyOldItems, i);
				if (!pyItem || !PyTuple_Check(pyItem) || PyTuple_Size(pyItem) != 2)
					continue;

				PyObject* pyKey = PyTuple_GET_ITEM(pyItem, 0);
				if (!PyUnicode_Check(pyKey))
					continue;

				const char* key = PyUnicode_AsUTF8(pyKey);
				if (!key)
				{
					PyErr_Clear();
					continue;
				}

				if (!shouldReportStaleTypeAttr(key) || PyMapping_HasKey(pyNewDict, pyKey))
					continue;

				++g_reloadStats.staleAttrsKept;
				WARNING_MSG(fmt::format("EntityDef::patchOldTypeFromNewType: stale attr kept on old class, attr={}. "
					"Deleted script attributes are not removed during safe reload.\n", key));
			}

			Py_DECREF(pyOldItems);
		}
		else
		{
			PyErr_Clear();
		}

		Py_DECREF(pyOldDict);
	}
	else
	{
		PyErr_Clear();
	}

	Py_DECREF(pyNewDict);
	PyType_Modified((PyTypeObject*)pyOldType);
}

//-------------------------------------------------------------------------------------
static void patchReloadedModuleTypes(PyObject* pyModule, const std::map<std::string, PyObject*>& oldTypes)
{
	// 对一个刚 reload 完的模块，把其中同名新类同步回 reload 前收集到的旧类对象。
	// 如果新模块没有同名类或同名对象不再是 type，则跳过，避免误改普通变量。
	std::map<std::string, PyObject*>::const_iterator iter = oldTypes.begin();
	for (; iter != oldTypes.end(); ++iter)
	{
		PyObject* pyNewType = PyObject_GetAttrString(pyModule, iter->first.c_str());
		if (!pyNewType)
		{
			PyErr_Clear();
			continue;
		}

		if (PyType_Check(pyNewType) && pyNewType != iter->second)
			patchOldTypeFromNewType(iter->second, pyNewType);

		Py_DECREF(pyNewType);
	}
}

//-------------------------------------------------------------------------------------
static void releaseCollectedTypes(std::map<std::string, PyObject*>& oldTypes)
{
	// collectModuleTypes 对旧类对象做了 INCREF，reload/patch 结束后统一释放。
	std::map<std::string, PyObject*>::iterator iter = oldTypes.begin();
	for (; iter != oldTypes.end(); ++iter)
		Py_DECREF(iter->second);

	oldTypes.clear();
}

//-------------------------------------------------------------------------------------
static void rememberInitialScriptFileStamp(const std::string& moduleName, const std::string& filePath)
{
	// 进程启动时导入 Entity/Component 主模块会经过 loadScriptModule，
	// 在这里记录初始文件版本戳，后续 reloadScript(False) 才能判断主模块是否真的变化。
	if (filePath.empty())
		return;

	uint64 currStamp = getScriptFileStamp(filePath);
	if (currStamp > 0)
	{
		g_scriptModuleStamps[moduleName] = currStamp;
		g_scriptFileStamps[filePath] = currStamp;
	}
}

//-------------------------------------------------------------------------------------
static std::string getCurrentComponentScriptPath(const std::string& entitiesPath)
{
	// 把 scripts 根目录收敛到当前进程真正可执行的脚本目录。
	// cellapp 只看 scripts/cell，baseapp 只看 scripts/base，避免跨组件扫描导致 import 错误。
	std::string dependencyPath = normalizePluginPath(entitiesPath);
	while (dependencyPath.size() > 0 && dependencyPath[dependencyPath.size() - 1] != '/' && dependencyPath[dependencyPath.size() - 1] != '\\')
		dependencyPath += "/";

	switch (g_componentType)
	{
	case BASEAPP_TYPE:
		dependencyPath += "base";
		break;
	case CELLAPP_TYPE:
		dependencyPath += "cell";
		break;
	default:
		return "";
	}

	return dependencyPath;
}

//-------------------------------------------------------------------------------------
static void rememberLoadedDependencyScriptFileStamps(const std::string& entitiesPath)
{
	// 启动阶段 Entity 主脚本 import 的 interface/helper 模块不会经过 loadScriptModule。
	// 如果不给这些已加载模块建立文件版本戳基线，第一次 reloadScript(False) 就只能把它们当成未知模块。
	// 这里直接遍历 sys.modules，将当前组件脚本目录下已经加载的 .py 全部纳入追踪。
	std::string dependencyPath = getCurrentComponentScriptPath(entitiesPath);
	if (dependencyPath.empty() || access(dependencyPath.c_str(), 0) != 0)
		return;

	std::string rootPath = dependencyPath;
	while (rootPath.size() > 0 && (rootPath[rootPath.size() - 1] == '/' || rootPath[rootPath.size() - 1] == '\\'))
		rootPath.erase(rootPath.size() - 1, 1);

	PyObject* sysModules = PyImport_GetModuleDict();
	PyObject* key = NULL;
	PyObject* value = NULL;
	Py_ssize_t pos = 0;
	uint32 tracked = 0;
	while (PyDict_Next(sysModules, &pos, &key, &value))
	{
		if (!PyUnicode_Check(key) || !PyModule_Check(value))
			continue;

		const char* moduleName = PyUnicode_AsUTF8(key);
		if (!moduleName)
		{
			PyErr_Clear();
			continue;
		}

		std::string filePath = getPyModuleFilePath(value);
		if (filePath.empty() || filePath.find(rootPath) != 0)
			continue;

		rememberInitialScriptFileStamp(moduleName, filePath);
		++tracked;
	}

	INFO_MSG(fmt::format("EntityDef::rememberLoadedDependencyScriptFileStamps: path={}, tracked={}.\n",
		dependencyPath, tracked));
}

//-------------------------------------------------------------------------------------
static void logReloadChangedFiles()
{
	// 本轮 EntityDef reload 完成后输出变更文件汇总。
	// 只逐条打印真正 reload 的文件，未变化文件只给数量，方便从日志里直接定位本次热更内容。
	g_reloadStats.changedFiles = (uint32)g_reloadChangedFiles.size();
	g_reloadStats.skippedFiles = (uint32)g_reloadSkippedFiles.size();

	if (g_reloadChangedFiles.empty())
	{
		INFO_MSG(fmt::format("EntityDef::reload: no changed script files, skippedFiles={}.\n",
			g_reloadSkippedFiles.size()));
		return;
	}

	INFO_MSG(fmt::format("EntityDef::reload: changed script files count={}, skippedFiles={}.\n",
		g_reloadChangedFiles.size(), g_reloadSkippedFiles.size()));

	std::vector<std::string>::iterator iter = g_reloadChangedFiles.begin();
	for (; iter != g_reloadChangedFiles.end(); ++iter)
	{
		INFO_MSG(fmt::format("EntityDef::reload: changed script file: {}\n", (*iter)));
	}

	if (g_reloadStats.duplicateModulePatches > 0 || g_reloadStats.staleAttrsKept > 0)
	{
		INFO_MSG(fmt::format("EntityDef::reload: duplicateModulePatches={}, staleAttrsKept={}.\n",
			g_reloadStats.duplicateModulePatches, g_reloadStats.staleAttrsKept));
	}
}

//-------------------------------------------------------------------------------------
EntityDef::EntityDef()
{
}

//-------------------------------------------------------------------------------------
EntityDef::~EntityDef()
{
	EntityDef::finalise();
}

//-------------------------------------------------------------------------------------
bool EntityDef::finalise(bool isReload)
{
	PropertyDescription::resetDescriptionCount();
	MethodDescription::resetDescriptionCount();

	EntityDef::__md5.clear();
	g_methodUtypeAuto = 1;
	EntityDef::_isInit = false;

	g_propertyUtypeAuto = 1;
	g_propertyUtypes.clear();

	if(!isReload)
	{
		std::vector<ScriptDefModulePtr>::iterator iter = EntityDef::__scriptModules.begin();
		for(; iter != EntityDef::__scriptModules.end(); ++iter)
		{
			(*iter)->finalise();
		}

		iter = EntityDef::__oldScriptModules.begin();
		for(; iter != EntityDef::__oldScriptModules.end(); ++iter)
		{
			(*iter)->finalise();
		}

		EntityDef::__oldScriptModules.clear();
		EntityDef::__oldScriptTypeMappingUType.clear();
	}

	g_scriptUtype = 1;
	EntityDef::__scriptModules.clear();
	EntityDef::__scriptTypeMappingUType.clear();
	g_methodCusUtypes.clear();
	DataType::finalise();
	DataTypes::finalise();
	return true;
}

//-------------------------------------------------------------------------------------
PyObject* EntityDef::tryGetEntity(COMPONENT_ID componentID, ENTITY_ID entityID)
{
	return __getEntityFunc(componentID, entityID);
}

//-------------------------------------------------------------------------------------
bool EntityDef::isReload()
{
	return g_isReload;
}

//-------------------------------------------------------------------------------------
bool EntityDef::reloadDependencyScriptModules(std::string entitiesPath)
{
	// __entitiesPath 指向用户 scripts 根目录。依赖热更必须收敛到当前进程所属目录：
	// cellapp 只处理 scripts/cell，baseapp 只处理 scripts/base。
	// 如果扫描整个 scripts，会在 cell 进程误 import base.Account，导致 KBEngine.Proxy 不存在。
	std::string dependencyPath = getCurrentComponentScriptPath(entitiesPath);
	if (dependencyPath.empty())
	{
		INFO_MSG(fmt::format("EntityDef::reloadDependencyScriptModules: skip componentType={}.\n",
			COMPONENT_NAME_EX(g_componentType)));
		return true;
	}

	if (access(dependencyPath.c_str(), 0) != 0)
	{
		// 某些组件或工具进程可能没有对应脚本目录，这不应阻断 reload。
		WARNING_MSG(fmt::format("EntityDef::reloadDependencyScriptModules: dependency path({}) not found.\n",
			dependencyPath));
		return true;
	}

	// Entity/Component 主模块由 loadAllEntityScriptModules/loadAllComponentScriptModules 负责。
	// 这里记录它们的名字，后续扫描到同名根目录脚本时跳过，避免重复 reload 破坏原有检查流程。
	std::set<std::string> ownedScriptModules;
	std::vector<ScriptDefModulePtr>::iterator iter = EntityDef::__scriptModules.begin();
	for (; iter != EntityDef::__scriptModules.end(); ++iter)
	{
		ownedScriptModules.insert((*iter)->getName());

		const ScriptDefModule::COMPONENTDESCRIPTION_MAP& componentDescrs = (*iter)->getComponentDescrs();
		ScriptDefModule::COMPONENTDESCRIPTION_MAP::const_iterator compIter = componentDescrs.begin();
		for (; compIter != componentDescrs.end(); ++compIter)
		{
			ownedScriptModules.insert(compIter->second->getName());
		}
	}

	wchar_t* wentitiesPath = strutil::char2wchar(dependencyPath.c_str());
	if (!wentitiesPath)
	{
		ERROR_MSG("EntityDef::reloadDependencyScriptModules: char2wchar entitiesPath failed.\n");
		return false;
	}

	std::vector<std::wstring> results;
	Resmgr::getSingleton().listPathRes(wentitiesPath, L"py", results);
	free(wentitiesPath);

	// rootPath 用来把绝对路径裁剪为相对模块路径，例如：
	// D:/.../scripts/cell/interfaces/Teleport.py -> interfaces.Teleport。
	std::string rootPath = dependencyPath;
	while (rootPath.size() > 0 && (rootPath[rootPath.size() - 1] == '/' || rootPath[rootPath.size() - 1] == '\\'))
		rootPath.erase(rootPath.size() - 1, 1);

	// rootModules：cell/SpaceContext.py 这类当前组件根目录 helper。
	// interfaceModules：cell/interfaces/Teleport.py 这类 mixin/interface，必须在 Entity 主脚本前 reload。
	// otherModules：当前组件目录下其他子包，放在最后处理。
	std::vector<std::string> rootModules;
	std::vector<std::string> interfaceModules;
	std::vector<std::string> otherModules;
	// 兼容历史脚本曾经通过 import Teleport 直接导入 interface 文件的情况。
	// 正常路径是 interfaces.Teleport，但 sys.modules 里可能还留有裸模块 Teleport。
	std::vector<std::string> aliasModules;
	std::vector<std::wstring>::iterator resultIter = results.begin();
	for (; resultIter != results.end(); ++resultIter)
	{
		std::wstring wstrpath = (*resultIter);

		if (wstrpath.find(L"__pycache__") != std::wstring::npos)
			continue;

		if (wstrpath.find(L"__init__.") != std::wstring::npos)
			continue;

		std::pair<std::wstring, std::wstring> pathPair = script::PyPlatform::splitPath(wstrpath);
		std::pair<std::wstring, std::wstring> filePair = script::PyPlatform::splitText(pathPair.second);

		if (filePair.first.size() == 0)
			continue;

		char* cpacketPath = strutil::wchar2char(pathPair.first.c_str());
		char* cmoduleName = strutil::wchar2char(filePair.first.c_str());

		if (!cpacketPath || !cmoduleName)
		{
			free(cpacketPath);
			free(cmoduleName);
			continue;
		}

		std::string packetPath = normalizePluginPath(cpacketPath);
		std::string moduleName = cmoduleName;
		free(cpacketPath);
		free(cmoduleName);

		if (packetPath.find(rootPath) == 0)
			packetPath.erase(0, rootPath.size());

		while (packetPath.size() > 0 && (packetPath[0] == '/' || packetPath[0] == '\\'))
			packetPath.erase(0, 1);

		strutil::kbe_replace(packetPath, "/", ".");
		strutil::kbe_replace(packetPath, "\\", ".");

		if (packetPath == "components")
			continue;

		// 根目录下的 Entity 主模块要交给原始 EntityDef 加载流程。
		// 非 fullReload 时主模块会被 loadScriptModule 重新 import/reload 并做 def 校验。
		if (packetPath.size() == 0 && ownedScriptModules.find(moduleName) != ownedScriptModules.end())
			continue;

		std::string fullModuleName = packetPath.size() == 0 ? moduleName : packetPath + "." + moduleName;

		if (packetPath == "interfaces" || packetPath.find("interfaces.") == 0)
			interfaceModules.push_back(fullModuleName);
		else if (packetPath.size() == 0)
			rootModules.push_back(fullModuleName);
		else
			otherModules.push_back(fullModuleName);

		if (packetPath == "interfaces")
			aliasModules.push_back(moduleName);
	}

	std::vector<std::string> modules;
	// reload 顺序很重要：helper -> interfaces -> other -> Entity/Component 主脚本。
	// 例如 Teleport.py import SpaceContext，则 SpaceContext 必须先刷新。
	modules.insert(modules.end(), rootModules.begin(), rootModules.end());
	modules.insert(modules.end(), interfaceModules.begin(), interfaceModules.end());
	modules.insert(modules.end(), otherModules.begin(), otherModules.end());

	PyObject* sysModules = PyImport_GetModuleDict();
	uint32 reloaded = 0;
	uint32 unchanged = 0;
	uint32 skippedNotLoaded = 0;
	uint32 aliasReloaded = 0;
	uint32 aliasUnchanged = 0;
	bool ok = true;
	std::map<std::string, PyObject*> reloadedModulesByFile;

	std::vector<std::string>::iterator moduleIter = modules.begin();
	for (; moduleIter != modules.end(); ++moduleIter)
	{
		PyObject* pyModule = PyDict_GetItemString(sysModules, moduleIter->c_str());
		if (pyModule)
		{
			// 只 reload 已经加载过的模块。热更不主动 import 新模块，避免执行未参与当前进程的脚本顶层代码。
			std::string filePath = getPyModuleFilePath(pyModule);
			uint64 currStamp = 0;
			bool firstTrack = false;
			bool changed = filePath.empty() || isTrackedScriptFileChanged(*moduleIter, filePath, currStamp, firstTrack);
			rememberReloadFile(changed, *moduleIter, filePath);

			if (!changed)
			{
				++unchanged;
				continue;
			}

			std::map<std::string, PyObject*> oldTypes;
			collectModuleTypes(pyModule, *moduleIter, oldTypes);

			std::map<std::string, PyObject*>::iterator reloadedIter = reloadedModulesByFile.find(filePath);
			if (!filePath.empty() && reloadedIter != reloadedModulesByFile.end())
			{
				// 同一个物理文件可能被 Python 以多个模块名加载。文件已经 reload 过时，
				// 不再重复执行顶层代码，只用已 reload 模块的新类去 patch 当前模块名下的旧类对象。
				patchReloadedModuleTypes(reloadedIter->second, oldTypes);
				releaseCollectedTypes(oldTypes);
				++g_reloadStats.duplicateModulePatches;
				INFO_MSG(fmt::format("EntityDef::reloadDependencyScriptModules: patch duplicate module={}, file={} without second reload.\n",
					(*moduleIter), filePath));
				continue;
			}

			PyObject* pyReloadedModule = PyImport_ReloadModule(pyModule);
			if (!pyReloadedModule)
			{
				ERROR_MSG(fmt::format("EntityDef::reloadDependencyScriptModules: reload module({}) failed.\n",
					(*moduleIter)));
				PyErr_Print();
				releaseCollectedTypes(oldTypes);
				ok = false;
				continue;
			}

			patchReloadedModuleTypes(pyReloadedModule, oldTypes);

			// 关键修复：patch 完后必须把模块里的属性指回被 patch 的旧类对象。
			// 否则下次 reload 时 collectModuleTypes 会从模块里拿到第一次 reload 创建的新类对象，
			// 而不是 Entity 实际继承的原始类。连续两次 reload 只有第一次能生效。
			// 注意：只在这里（主模块路径）执行，alias duplicate 路径不执行，避免覆盖。
			for (std::map<std::string, PyObject*>::const_iterator ot = oldTypes.begin(); ot != oldTypes.end(); ++ot)
			{
				if (PyObject_SetAttrString(pyReloadedModule, ot->first.c_str(), ot->second) == -1)
				{
					WARNING_MSG(fmt::format("EntityDef::reloadDependencyScriptModules: "
						"reset module attr '{}' to old type failed.\n", ot->first));
					PyErr_Clear();
				}
			}

			releaseCollectedTypes(oldTypes);
			if (!filePath.empty())
			{
				Py_INCREF(pyReloadedModule);
				reloadedModulesByFile[filePath] = pyReloadedModule;
			}
			Py_DECREF(pyReloadedModule);
			++reloaded;
			++g_reloadStats.reloadedModules;

			INFO_MSG(fmt::format("EntityDef::reloadDependencyScriptModules: reload changed module={}, file={}, firstTrack={}, stamp={}.\n",
				(*moduleIter), filePath, firstTrack, currStamp));
		}
		else
		{
			// 未加载模块说明当前进程尚未用到它，跳过即可。
			// 后续如果 Entity 主脚本 import 它，会由 Python 正常 import 到最新文件。
			++skippedNotLoaded;
		}
	}

	// 兼容 sys.path 中 interfaces 目录直接可见时产生的裸模块名。
	// 如果不存在裸模块，说明脚本一直使用 interfaces.X 路径，直接跳过。
	std::vector<std::string>::iterator aliasIter = aliasModules.begin();
	for (; aliasIter != aliasModules.end(); ++aliasIter)
	{
		PyObject* pyModule = PyDict_GetItemString(sysModules, aliasIter->c_str());
		if (!pyModule)
			continue;

		std::string filePath = getPyModuleFilePath(pyModule);
		uint64 currStamp = 0;
		bool firstTrack = false;
		bool changed = filePath.empty() || isTrackedScriptFileChanged(*aliasIter, filePath, currStamp, firstTrack);
		rememberReloadFile(changed, *aliasIter, filePath);

		if (!changed)
		{
			++aliasUnchanged;
			continue;
		}

		std::map<std::string, PyObject*> oldTypes;
		collectModuleTypes(pyModule, *aliasIter, oldTypes);

		std::map<std::string, PyObject*>::iterator reloadedIter = reloadedModulesByFile.find(filePath);
		if (!filePath.empty() && reloadedIter != reloadedModulesByFile.end())
		{
			// 裸模块别名指向已 reload 的同一个文件时，只 patch 旧类，不重复执行模块顶层代码。
			patchReloadedModuleTypes(reloadedIter->second, oldTypes);
			releaseCollectedTypes(oldTypes);
			++aliasReloaded;
			++g_reloadStats.duplicateModulePatches;
			INFO_MSG(fmt::format("EntityDef::reloadDependencyScriptModules: patch duplicate alias module={}, file={} without second reload.\n",
				(*aliasIter), filePath));
			continue;
		}

		PyObject* pyReloadedModule = PyImport_ReloadModule(pyModule);
		if (!pyReloadedModule)
		{
			ERROR_MSG(fmt::format("EntityDef::reloadDependencyScriptModules: reload alias module({}) failed.\n",
				(*aliasIter)));
			PyErr_Print();
			releaseCollectedTypes(oldTypes);
			ok = false;
			continue;
		}

		patchReloadedModuleTypes(pyReloadedModule, oldTypes);
		releaseCollectedTypes(oldTypes);
		if (!filePath.empty())
		{
			Py_INCREF(pyReloadedModule);
			reloadedModulesByFile[filePath] = pyReloadedModule;
		}
		Py_DECREF(pyReloadedModule);
		++aliasReloaded;
		++g_reloadStats.reloadedModules;

		INFO_MSG(fmt::format("EntityDef::reloadDependencyScriptModules: reload changed alias module={}, file={}, firstTrack={}, stamp={}.\n",
			(*aliasIter), filePath, firstTrack, currStamp));
	}

	INFO_MSG(fmt::format("EntityDef::reloadDependencyScriptModules: path={}, modules={}, reloaded={}, unchanged={}, skippedNotLoaded={}, aliasReloaded={}, aliasUnchanged={}, ok={}.\n",
		dependencyPath, modules.size(), reloaded, unchanged, skippedNotLoaded, aliasReloaded, aliasUnchanged, ok));

	std::map<std::string, PyObject*>::iterator reloadedIter = reloadedModulesByFile.begin();
	for (; reloadedIter != reloadedModulesByFile.end(); ++reloadedIter)
		Py_DECREF(reloadedIter->second);

	return ok;
}

//-------------------------------------------------------------------------------------
ReloadScriptDefStats EntityDef::reload(bool fullReload)
{
	g_isReload = true;
	g_reloadStats = ReloadScriptDefStats();
	g_reloadChangedFiles.clear();
	g_reloadSkippedFiles.clear();
	g_reloadChangedFileKeys.clear();
	g_reloadSkippedFileKeys.clear();

	script::entitydef::reload(fullReload);

	// 先刷新当前组件目录下的 helper/interface 依赖模块，再刷新 Entity/Component 主模块。
	// 这样 Avatar.py 重新定义 class 时继承到的是新的 interfaces.Teleport.Teleport。
	if (!reloadDependencyScriptModules(EntityDef::__entitiesPath))
	{
		g_reloadStats.ok = false;
		WARNING_MSG("EntityDef::reload: dependency script reload has errors, abort current reload before entity refresh.\n");
		logReloadChangedFiles();
		g_isReload = false;
		return g_reloadStats;
	}

	if(fullReload)
	{
		EntityDef::__oldScriptModules.clear();
		EntityDef::__oldScriptTypeMappingUType.clear();

		std::vector<ScriptDefModulePtr>::iterator iter = EntityDef::__scriptModules.begin();
		for(; iter != EntityDef::__scriptModules.end(); ++iter)
		{
			__oldScriptModules.push_back((*iter));
			__oldScriptTypeMappingUType[(*iter)->getName()] = (*iter)->getUType();
		}

		bool ret = finalise(true);
		KBE_ASSERT(ret && "EntityDef::reload: finalise error!");

		ret = initialize(EntityDef::__scriptBaseTypes, EntityDef::__loadComponentType);
		KBE_ASSERT(ret && "EntityDef::reload: initialize error!");
	}
	else
	{
		if (!loadAllEntityScriptModules(EntityDef::__entitiesPath, EntityDef::__scriptBaseTypes))
			g_reloadStats.ok = false;
	}

	EntityDef::_isInit = true;
	logReloadChangedFiles();
	g_isReload = false;
	return g_reloadStats;
}

//-------------------------------------------------------------------------------------
bool EntityDef::initialize(std::vector<PyTypeObject*>& scriptBaseTypes,
						   COMPONENT_TYPE loadComponentType)
{
	__loadComponentType = loadComponentType;
	__scriptBaseTypes = scriptBaseTypes;

	__entitiesPath = Resmgr::getSingleton().getPyUserScriptsPath();

	g_entityFlagMapping["CELL"]									= ED_FLAG_CELL_PUBLIC;
	g_entityFlagMapping["CELL_AND_CLIENT"]						= ED_FLAG_CELL_PUBLIC_AND_OWN;
	g_entityFlagMapping["CELL_AND_CLIENTS"]						= ED_FLAG_ALL_CLIENTS;
	g_entityFlagMapping["CELL_AND_OTHER_CLIENTS"]				= ED_FLAG_OTHER_CLIENTS;
	g_entityFlagMapping["BASE_AND_CLIENT"]						= ED_FLAG_BASE_AND_CLIENT;
	g_entityFlagMapping["BASE"]									= ED_FLAG_BASE;

	g_entityFlagMapping["CELL_PUBLIC"]							= ED_FLAG_CELL_PUBLIC;
	g_entityFlagMapping["CELL_PRIVATE"]							= ED_FLAG_CELL_PRIVATE;
	g_entityFlagMapping["ALL_CLIENTS"]							= ED_FLAG_ALL_CLIENTS;
	g_entityFlagMapping["CELL_PUBLIC_AND_OWN"]					= ED_FLAG_CELL_PUBLIC_AND_OWN;

	g_entityFlagMapping["OTHER_CLIENTS"]						= ED_FLAG_OTHER_CLIENTS;
	g_entityFlagMapping["OWN_CLIENT"]							= ED_FLAG_OWN_CLIENT;

	std::string entitiesFile = __entitiesPath + "entities.xml";
	std::string defFilePath = __entitiesPath + "entity_defs/";
	std::string assetsTypesFile = defFilePath + "types.xml";

	if (!PluginManager::instance().initialize())
		return false;

	// 插件 schema 前置：插件 types.xml 先于 assets/entity_defs/types.xml 加载。
	// 这样 assets 的 .def 和 types.xml 可以直接引用启用插件提供的类型；启用插件即表示修改当前 assets schema。
	const std::vector<PluginDescriptor>& plugins = PluginManager::instance().plugins();
	if (!plugins.empty())
	{
		INFO_MSG(fmt::format("EntityDef::initialize: loading plugin type files before assets types, plugins={}.\n",
			plugins.size()));

		bool dataTypesInitialized = false;

		std::vector<PluginTypeFileDescriptor> pluginTypeFiles = PluginManager::instance().getTypeFileDescriptors();
		for (std::vector<PluginTypeFileDescriptor>::const_iterator iter = pluginTypeFiles.begin(); iter != pluginTypeFiles.end(); ++iter)
		{
			INFO_MSG(fmt::format("EntityDef::initialize: loading plugin [{}] types [{}] with prefix [{}].\n",
				iter->pluginName, iter->file, iter->pluginPrefix));

			if (!dataTypesInitialized)
			{
				if (!DataTypes::initialize(iter->file, iter->pluginPrefix, iter->file))
					return false;

				dataTypesInitialized = true;
			}
			else
			{
				if (!DataTypes::loadTypes(iter->file, iter->pluginPrefix, iter->file))
					return false;
			}
		}

		if (!dataTypesInitialized)
		{
			if (!DataTypes::initialize(assetsTypesFile))
				return false;
		}
		else
		{
			if (!DataTypes::loadTypes(assetsTypesFile))
				return false;
		}
	}
	else
	{
		// 没有启用插件类型时保持原 KBE 路径：初始化基础类型，然后读取 assets/entity_defs/types.xml。
		if(!DataTypes::initialize(assetsTypesFile))
			return false;
	}

	// 插件实体同样前置注册。plugins.xml 的顺序就是插件实体的 utype/MD5 顺序；
	// assets/entities.xml 随后加载，若出现同名实体会启动失败，避免静默覆盖插件 schema。
	const std::vector<PluginEntityDescriptor>& pluginEntities = PluginManager::instance().entities();
	if (!pluginEntities.empty())
	{
		INFO_MSG(fmt::format("EntityDef::initialize: loading {} plugin entity definition(s) before assets entities.\n",
			pluginEntities.size()));
	}

	for (std::vector<PluginEntityDescriptor>::const_iterator iter = pluginEntities.begin(); iter != pluginEntities.end(); ++iter)
	{
		if (findScriptModule(iter->name.c_str(), false))
		{
			ERROR_MSG(fmt::format("EntityDef::initialize: plugin entity [{}] conflicts with an existing entity.\n",
				iter->name));
			return false;
		}

		ScriptDefModule* pScriptModule = registerNewScriptDefModule(iter->name);
		pScriptModule->setDefSourceFile(iter->defFullPath);
		SmartPointer<XML> defxml(new XML());

		if (!pluginFileExists(iter->defFullPath))
		{
			ERROR_MSG(fmt::format("EntityDef::initialize: plugin entity [{}] def not found [{}]\n",
				iter->name, iter->defFullPath));
			return false;
		}

		if (iter->hasBase && !pluginEntityScriptExists(*iter, "base"))
		{
			ERROR_MSG(fmt::format("EntityDef::initialize: plugin [{}] entity [{}] declared base but script not found, manifest [{}].\n",
				iter->pluginName, iter->name, iter->manifestFile));
			return false;
		}

		if (iter->hasCell && !pluginEntityScriptExists(*iter, "cell"))
		{
			ERROR_MSG(fmt::format("EntityDef::initialize: plugin [{}] entity [{}] declared cell but script not found, manifest [{}].\n",
				iter->pluginName, iter->name, iter->manifestFile));
			return false;
		}

		if (iter->hasClient && !pluginEntityScriptExists(*iter, "client"))
		{
			ERROR_MSG(fmt::format("EntityDef::initialize: plugin [{}] entity [{}] declared client but script not found, manifest [{}].\n",
				iter->pluginName, iter->name, iter->manifestFile));
			return false;
		}

		if (!defxml->openSection(iter->defFullPath.c_str()))
			return false;

		TiXmlNode* defNode = defxml->getRootNode();
		if (defNode != NULL)
		{
			std::string pluginDefPath = iter->defFullPath;
			std::string::size_type pos = pluginDefPath.find_last_of("/\\");
			pluginDefPath = (pos == std::string::npos) ? "" : pluginDefPath.substr(0, pos + 1);

			if (!loadDefInfo(pluginDefPath, iter->name, defxml.get(), defNode, pScriptModule))
			{
				ERROR_MSG(fmt::format("EntityDef::initialize: failed to load plugin entity({}) module!\n",
					iter->name));
				return false;
			}

			if (!loadDetailLevelInfo(pluginDefPath, iter->name, defxml.get(), defNode, pScriptModule))
			{
				ERROR_MSG(fmt::format("EntityDef::initialize: failed to load plugin entity({}) DetailLevelInfo!\n",
					iter->name));
				return false;
			}
		}

		pScriptModule->onLoaded();
	}

	// 打开这个entities.xml文件
	// 允许纯脚本定义，则可能没有这个文件
	if (access(entitiesFile.c_str(), 0) == 0)
	{
		SmartPointer<XML> xml(new XML());
		if (!xml->openSection(entitiesFile.c_str()))
			return false;

		// 获得entities.xml根节点, 如果没有定义一个entity那么直接返回true
		TiXmlNode* node = xml->getRootNode();
		if (node == NULL)
			return true;

		// 开始遍历所有的entity节点。插件实体已经前置注册，因此这里必须显式检查重名。
		XML_FOR_BEGIN(node)
		{
			std::string moduleName = xml.get()->getKey(node);

			if (findScriptModule(moduleName.c_str(), false))
			{
				const PluginEntityDescriptor* pluginEntity = PluginManager::instance().findEntity(moduleName);
				if (pluginEntity)
				{
					ERROR_MSG(fmt::format("EntityDef::initialize: assets entity [{}] conflicts with plugin [{}] entity [{}], pluginManifest=[{}], assetsFile=[{}].\n",
						moduleName, pluginEntity->pluginName, pluginEntity->name, pluginEntity->manifestFile, entitiesFile));
				}
				else
				{
					ERROR_MSG(fmt::format("EntityDef::initialize: assets entity [{}] is duplicated in assetsFile=[{}].\n",
						moduleName, entitiesFile));
				}
				return false;
			}

			ScriptDefModule* pScriptModule = registerNewScriptDefModule(moduleName);

			std::string deffile = defFilePath + moduleName + ".def";
			pScriptModule->setDefSourceFile(deffile);
			SmartPointer<XML> defxml(new XML());

			if (!defxml->openSection(deffile.c_str()))
				return false;

			TiXmlNode* defNode = defxml->getRootNode();
			if (defNode == NULL)
			{
				// root节点下没有子节点了
				continue;
			}

			// 加载def文件中的定义
			if (!loadDefInfo(defFilePath, moduleName, defxml.get(), defNode, pScriptModule))
			{
				ERROR_MSG(fmt::format("EntityDef::initialize: failed to load entity({}) module!\n",
					moduleName.c_str()));

				return false;
			}

			// 尝试在主entity文件中加载detailLevel数据
			if (!loadDetailLevelInfo(defFilePath, moduleName, defxml.get(), defNode, pScriptModule))
			{
				ERROR_MSG(fmt::format("EntityDef::initialize: failed to load entity({}) DetailLevelInfo!\n",
					moduleName.c_str()));

				return false;
			}

			pScriptModule->onLoaded();
		}
		XML_FOR_END(node);
	}

	if (!script::entitydef::initialize())
		return false;

	EntityDef::md5().final();

	if(loadComponentType == DBMGR_TYPE)
		return true;

	if (!loadAllEntityScriptModules(__entitiesPath, scriptBaseTypes))
		return false;

	rememberLoadedDependencyScriptFileStamps(__entitiesPath);

	return initializeWatcher();
}

//-------------------------------------------------------------------------------------
ScriptDefModule* EntityDef::registerNewScriptDefModule(const std::string& moduleName)
{
	ScriptDefModule* pScriptModule = findScriptModule(moduleName.c_str(), false);

	if (!pScriptModule)
	{
		__scriptTypeMappingUType[moduleName] = g_scriptUtype;
		pScriptModule = new ScriptDefModule(moduleName, g_scriptUtype++);
		EntityDef::__scriptModules.push_back(pScriptModule);
	}

	return pScriptModule;
}

//-------------------------------------------------------------------------------------
MethodDescription* EntityDef::createMethodDescription(ScriptDefModule* pScriptModule, ENTITY_METHOD_UID utype, COMPONENT_ID domain, const std::string& name, MethodDescription::EXPOSED_TYPE exposedType)
{
	if(utype > 0)
		g_methodCusUtypes.push_back(utype);

	// 如果配置中没有设置过utype, 则产生
	if (utype == 0)
	{
		ENTITY_METHOD_UID muid = 0;
		while (true)
		{
			muid = g_methodUtypeAuto++;
			std::vector<ENTITY_METHOD_UID>::iterator iterutype =
				std::find(g_methodCusUtypes.begin(), g_methodCusUtypes.end(), muid);

			if (iterutype == g_methodCusUtypes.end())
			{
				break;
			}
		}

		utype = muid;
		g_methodCusUtypes.push_back(muid);
	}
	else
	{
		// 检查是否有重复的Utype
		ENTITY_METHOD_UID muid = utype;
		std::vector<ENTITY_METHOD_UID>::iterator iter =
			std::find(g_methodCusUtypes.begin(), g_methodCusUtypes.end(), muid);

		if (iter != g_methodCusUtypes.end())
		{
			bool foundConflict = false;

			MethodDescription* pConflictMethodDescription = pScriptModule->findBaseMethodDescription(muid);
			if (pConflictMethodDescription)
			{
				ERROR_MSG(fmt::format("EntityDef::loadDefMethods: {}.{}, 'Utype' {} Conflict({}.{} 'Utype' {})! componentType={}\n",
					pScriptModule->getName(), name.c_str(), muid, pScriptModule->getName(), pConflictMethodDescription->getName(), muid, COMPONENT_NAME_EX((COMPONENT_TYPE)domain)));

				foundConflict = true;
			}

			pConflictMethodDescription = pScriptModule->findCellMethodDescription(muid);
			if (pConflictMethodDescription)
			{
				ERROR_MSG(fmt::format("EntityDef::loadDefMethods: {}.{}, 'Utype' {} Conflict({}.{} 'Utype' {})! componentType={}\n",
					pScriptModule->getName(), name.c_str(), muid, pScriptModule->getName(), pConflictMethodDescription->getName(), muid, COMPONENT_NAME_EX((COMPONENT_TYPE)domain)));

				foundConflict = true;
			}

			pConflictMethodDescription = pScriptModule->findClientMethodDescription(muid);
			if (pConflictMethodDescription)
			{
				ERROR_MSG(fmt::format("EntityDef::loadDefMethods: {}.{}, 'Utype' {} Conflict({}.{} 'Utype' {})! componentType={}\n",
					pScriptModule->getName(), name.c_str(), muid, pScriptModule->getName(), pConflictMethodDescription->getName(), muid, COMPONENT_NAME_EX((COMPONENT_TYPE)domain)));

				foundConflict = true;
			}

			if (foundConflict)
				return NULL;
		}
	}

	return new MethodDescription(utype, domain, name, exposedType);
}

//-------------------------------------------------------------------------------------
bool EntityDef::loadDefInfo(const std::string& defFilePath,
							const std::string& moduleName,
							XML* defxml,
							TiXmlNode* defNode,
							ScriptDefModule* pScriptModule)
{
	if(!loadAllDefDescriptions(moduleName, defxml, defNode, pScriptModule))
	{
		ERROR_MSG(fmt::format("EntityDef::loadDefInfo: failed to loadAllDefDescription(), entity:{}\n",
			moduleName.c_str()));

		return false;
	}

	// 遍历所有的interface， 并将他们的方法和属性加入到模块中
	if(!loadInterfaces(defFilePath, moduleName, defxml, defNode, pScriptModule))
	{
		ERROR_MSG(fmt::format("EntityDef::loadDefInfo: failed to load entity:{} interface.\n",
			moduleName.c_str()));

		return false;
	}

	// 遍历所有的component， 并将组件属性加入到模块中
	if (!loadComponents(defFilePath, moduleName, defxml, defNode, pScriptModule))
	{
		ERROR_MSG(fmt::format("EntityDef::loadDefInfo: failed to load entity:{} component.\n",
			moduleName.c_str()));

		return false;
	}

	// 加载父类所有的内容
	if(!loadParentClass(defFilePath, moduleName, defxml, defNode, pScriptModule))
	{
		ERROR_MSG(fmt::format("EntityDef::loadDefInfo: failed to load entity:{} parentClass.\n",
			moduleName.c_str()));

		return false;
	}

	// 尝试加载detailLevel数据
	if(!loadDetailLevelInfo(defFilePath, moduleName, defxml, defNode, pScriptModule))
	{
		ERROR_MSG(fmt::format("EntityDef::loadDefInfo: failed to load entity:{} DetailLevelInfo.\n",
			moduleName.c_str()));

		return false;
	}

	// 尝试加载VolatileInfo数据
	if(!loadVolatileInfo(defFilePath, moduleName, defxml, defNode, pScriptModule))
	{
		ERROR_MSG(fmt::format("EntityDef::loadDefInfo: failed to load entity:{} VolatileInfo.\n",
			moduleName.c_str()));

		return false;
	}

	pScriptModule->autoMatchCompOwn();
	return true;
}

//-------------------------------------------------------------------------------------
bool EntityDef::loadDetailLevelInfo(const std::string& defFilePath,
									const std::string& moduleName,
									XML* defxml,
									TiXmlNode* defNode,
									ScriptDefModule* pScriptModule)
{
	TiXmlNode* detailLevelNode = defxml->enterNode(defNode, "DetailLevels");
	if(detailLevelNode == NULL)
		return true;

	DetailLevel& dlInfo = pScriptModule->getDetailLevel();

	TiXmlNode* node = defxml->enterNode(detailLevelNode, "NEAR");
	TiXmlNode* radiusNode = defxml->enterNode(node, "radius");
	TiXmlNode* hystNode = defxml->enterNode(node, "hyst");
	if(node == NULL || radiusNode == NULL || hystNode == NULL)
	{
		ERROR_MSG(fmt::format("EntityDef::loadDetailLevelInfo: failed to load entity:{} NEAR-DetailLevelInfo.\n",
			moduleName.c_str()));

		return false;
	}

	dlInfo.level[DETAIL_LEVEL_NEAR].radius = (float)defxml->getValFloat(radiusNode);
	dlInfo.level[DETAIL_LEVEL_NEAR].hyst = (float)defxml->getValFloat(hystNode);

	node = defxml->enterNode(detailLevelNode, "MEDIUM");
	radiusNode = defxml->enterNode(node, "radius");
	hystNode = defxml->enterNode(node, "hyst");
	if(node == NULL || radiusNode == NULL || hystNode == NULL)
	{
		ERROR_MSG(fmt::format("EntityDef::loadDetailLevelInfo: failed to load entity:{} MEDIUM-DetailLevelInfo.\n",
			moduleName.c_str()));

		return false;
	}

	dlInfo.level[DETAIL_LEVEL_MEDIUM].radius = (float)defxml->getValFloat(radiusNode);

	dlInfo.level[DETAIL_LEVEL_MEDIUM].radius += dlInfo.level[DETAIL_LEVEL_NEAR].radius +
												dlInfo.level[DETAIL_LEVEL_NEAR].hyst;

	dlInfo.level[DETAIL_LEVEL_MEDIUM].hyst = (float)defxml->getValFloat(hystNode);

	node = defxml->enterNode(detailLevelNode, "FAR");
	radiusNode = defxml->enterNode(node, "radius");
	hystNode = defxml->enterNode(node, "hyst");
	if(node == NULL || radiusNode == NULL || hystNode == NULL)
	{
		ERROR_MSG(fmt::format("EntityDef::loadDetailLevelInfo: failed to load entity:{} FAR-DetailLevelInfo.\n",
			moduleName.c_str()));

		return false;
	}

	dlInfo.level[DETAIL_LEVEL_FAR].radius = (float)defxml->getValFloat(radiusNode);

	dlInfo.level[DETAIL_LEVEL_FAR].radius += dlInfo.level[DETAIL_LEVEL_MEDIUM].radius +
													dlInfo.level[DETAIL_LEVEL_MEDIUM].hyst;

	dlInfo.level[DETAIL_LEVEL_FAR].hyst = (float)defxml->getValFloat(hystNode);

	return true;

}

//-------------------------------------------------------------------------------------
bool EntityDef::loadVolatileInfo(const std::string& defFilePath,
									const std::string& moduleName,
									XML* defxml,
									TiXmlNode* defNode,
									ScriptDefModule* pScriptModule)
{
	TiXmlNode* pNode = defxml->enterNode(defNode, "Volatile");
	if(pNode == NULL)
		return true;

	VolatileInfo* pVolatileInfo = pScriptModule->getPVolatileInfo();

	TiXmlNode* node = defxml->enterNode(pNode, "position");
	if(node)
	{
		pVolatileInfo->position((float)defxml->getValFloat(node));
	}
	else
	{
		if(defxml->hasNode(pNode, "position"))
			pVolatileInfo->position(VolatileInfo::ALWAYS);
		else
			pVolatileInfo->position(-1.f);
	}

	node = defxml->enterNode(pNode, "yaw");
	if(node)
	{
		pVolatileInfo->yaw((float)defxml->getValFloat(node));
	}
	else
	{
		if(defxml->hasNode(pNode, "yaw"))
			pVolatileInfo->yaw(VolatileInfo::ALWAYS);
		else
			pVolatileInfo->yaw(-1.f);
	}

	node = defxml->enterNode(pNode, "pitch");
	if(node)
	{
		pVolatileInfo->pitch((float)defxml->getValFloat(node));
	}
	else
	{
		if(defxml->hasNode(pNode, "pitch"))
			pVolatileInfo->pitch(VolatileInfo::ALWAYS);
		else
			pVolatileInfo->pitch(-1.f);
	}

	node = defxml->enterNode(pNode, "roll");
	if(node)
	{
		pVolatileInfo->roll((float)defxml->getValFloat(node));
	}
	else
	{
		if(defxml->hasNode(pNode, "roll"))
			pVolatileInfo->roll(VolatileInfo::ALWAYS);
		else
			pVolatileInfo->roll(-1.f);
	}

	node = defxml->enterNode(pNode, "optimized");
	if (node)
	{
		pVolatileInfo->optimized(defxml->getBool(node));
	}
	else
	{
		if (defxml->hasNode(pNode, "optimized"))
			pVolatileInfo->optimized(true);
		else
			pVolatileInfo->optimized(true);
	}

	return true;
}

//-------------------------------------------------------------------------------------
bool EntityDef::loadInterfaces(const std::string& defFilePath,
							   const std::string& moduleName,
							   XML* defxml,
							   TiXmlNode* defNode,
							   ScriptDefModule* pScriptModule, bool ignoreComponents)
{
	TiXmlNode* implementsNode = defxml->enterNode(defNode, "Interfaces");
	if(implementsNode == NULL)
		return true;

	XML_FOR_BEGIN(implementsNode)
	{
		if (defxml->getKey(implementsNode) != "interface" && defxml->getKey(implementsNode) != "Interface" &&
			defxml->getKey(implementsNode) != "type" && defxml->getKey(implementsNode) != "Type")
			continue;

		TiXmlNode* interfaceNode = defxml->enterNode(implementsNode, "Interface");
		if (!interfaceNode)
		{
			interfaceNode = defxml->enterNode(implementsNode, "interface");
			if (!interfaceNode)
			{
				interfaceNode = defxml->enterNode(implementsNode, "Type");
				if (!interfaceNode)
				{
					interfaceNode = defxml->enterNode(implementsNode, "type");
					if (!interfaceNode)
					{
						continue;
					}
				}
			}
		}

		std::string interfaceName = defxml->getKey(interfaceNode);
		std::string interfacefile = defFilePath + "interfaces/" + interfaceName + ".def";
		SmartPointer<XML> interfaceXml(new XML());
		if(!interfaceXml.get()->openSection(interfacefile.c_str()))
			return false;

		TiXmlNode* interfaceRootNode = interfaceXml->getRootNode();
		if(interfaceRootNode == NULL)
		{
			// root节点下没有子节点了
			return true;
		}

		if(!loadAllDefDescriptions(moduleName, interfaceXml.get(), interfaceRootNode, pScriptModule))
		{
			ERROR_MSG(fmt::format("EntityDef::initialize: interface[{}] error!\n",
				interfaceName.c_str()));

			return false;
		}

		// 尝试加载detailLevel数据
		if(!loadDetailLevelInfo(defFilePath, moduleName, interfaceXml.get(), interfaceRootNode, pScriptModule))
		{
			ERROR_MSG(fmt::format("EntityDef::loadInterfaces: failed to load entity:{} DetailLevelInfo.\n",
				moduleName.c_str()));

			return false;
		}

		// 遍历所有的interface， 并将他们的方法和属性加入到模块中
		if (!ignoreComponents)
		{
			if (!loadComponents(defFilePath, moduleName, interfaceXml.get(), interfaceRootNode, pScriptModule))
			{
				ERROR_MSG(fmt::format("EntityDef::loadInterfaces: failed to load entity:{} component.\n",
					moduleName.c_str()));

				return false;
			}
		}

		// 遍历所有的interface， 并将他们的方法和属性加入到模块中
		if(!loadInterfaces(defFilePath, moduleName, interfaceXml.get(), interfaceRootNode, pScriptModule))
		{
			ERROR_MSG(fmt::format("EntityDef::loadInterfaces: failed to load entity:{} interface.\n",
				moduleName.c_str()));

			return false;
		}
	}
	XML_FOR_END(implementsNode);

	return true;
}

//-------------------------------------------------------------------------------------
bool EntityDef::loadComponents(const std::string& defFilePath,
	const std::string& moduleName,
	XML* defxml,
	TiXmlNode* defNode,
	ScriptDefModule* pScriptModule)
{
	TiXmlNode* implementsNode = defxml->enterNode(defNode, "Components");
	if (implementsNode == NULL)
		return true;

	XML_FOR_BEGIN(implementsNode)
	{
		std::string componentName = defxml->getKey(implementsNode);

		TiXmlNode* componentNode = defxml->enterNode(implementsNode, componentName.c_str());
		if (!componentNode)
			continue;

		if (!validDefPropertyName(componentName))
		{
			ERROR_MSG(fmt::format("EntityDef::loadComponents: '{}' is limited, in module({})!\n",
				componentName, moduleName));

			return false;
		}

		std::string componentTypeName = "";
		TiXmlNode* componentTypeNameNode = defxml->enterNode(componentNode, "Type");
		if (componentTypeNameNode)
			componentTypeName = defxml->getKey(componentTypeNameNode);

		if (componentTypeName == "")
		{
			ERROR_MSG(fmt::format("EntityDef::loadComponents: component name is NULL.\n",
				componentName.c_str()));

			return false;
		}

		// 组件默认仍按 KBE 原规则从宿主实体同级 entity_defs/components 读取。
		// 如果根 assets 没有该组件定义，则允许启用插件提供自己的组件 def：
		// plugins/<Plugin>/entity_defs/components/<Component>.def。
		// 这样 Avatar.def 只需要声明 <Type>BagComponent</Type>，组件的结构仍由插件目录维护。
		std::string componentDefFilePath = defFilePath;
		std::string componentfile = defFilePath + "components/" + componentTypeName + ".def";
		if (!pluginFileExists(componentfile))
		{
			std::string pluginDefFilePath;
			std::string pluginComponentFile = findPluginComponentDefFile(componentTypeName, &pluginDefFilePath);
			if (!pluginComponentFile.empty())
			{
				componentfile = pluginComponentFile;
				componentDefFilePath = pluginDefFilePath;
			}
		}

		SmartPointer<XML> componentXml(new XML());
		if (!componentXml.get()->openSection(componentfile.c_str()))
			return false;

		// 产生一个属性描述实例
		ENTITY_PROPERTY_UID			futype = 0;
		uint32						flags = ED_FLAG_BASE | ED_FLAG_CELL_PUBLIC | ENTITY_CLIENT_DATA_FLAGS;
		bool						isPersistent = true;
		bool						isIdentifier = false;		// 是否是一个索引键
		uint32						databaseLength = 0;			// 这个属性在数据库中的长度
		std::string					indexType = "";
		DETAIL_TYPE					detailLevel = DETAIL_LEVEL_FAR;
		std::string					detailLevelStr = "";
		std::string					strisPersistent;
		std::string					defaultStr = "";

		TiXmlNode* utypeValNode = defxml->enterNode(componentNode, "Utype");

		if (!calcDefPropertyUType(moduleName, componentName, (utypeValNode ? defxml->getValInt(utypeValNode) : -1), pScriptModule, futype))
			return false;

		TiXmlNode* persistentNode = defxml->enterNode(componentNode, "Persistent");
		if (persistentNode)
		{
			strisPersistent = defxml->getValStr(persistentNode);

			std::transform(strisPersistent.begin(), strisPersistent.end(),
				strisPersistent.begin(), tolower);

			if (strisPersistent == "false")
				isPersistent = false;
		}

		// 查找是否有这个模块，如果有说明已经加载过相关描述，这里无需再次加载
		ScriptDefModule* pCompScriptDefModule = findScriptModule(componentTypeName.c_str(), false);

		if (!pCompScriptDefModule)
		{
			pCompScriptDefModule = registerNewScriptDefModule(componentTypeName);
			pCompScriptDefModule->isPersistent(false);
			pCompScriptDefModule->isComponentModule(true);
		}
		else
		{
			flags = ED_FLAG_UNKOWN;

			if (pCompScriptDefModule->hasBase())
				flags |= ED_FLAG_BASE;

			if (pCompScriptDefModule->hasCell())
				flags |= ED_FLAG_CELL_PUBLIC;

			if (pCompScriptDefModule->hasClient())
			{
				if (pCompScriptDefModule->hasBase())
					flags |= ED_FLAG_BASE_AND_CLIENT;
				else
					flags |= (ED_FLAG_ALL_CLIENTS | ED_FLAG_CELL_PUBLIC_AND_OWN | ED_FLAG_OTHER_CLIENTS | ED_FLAG_OWN_CLIENT);
			}

			addComponentProperty(futype, componentTypeName, componentName, flags, isPersistent, isIdentifier,
				indexType, databaseLength, defaultStr, detailLevel, pScriptModule, pCompScriptDefModule);

			pScriptModule->addComponentDescription(componentName.c_str(), pCompScriptDefModule);
			continue;
		}

		TiXmlNode* componentRootNode = componentXml->getRootNode();
		if (componentRootNode == NULL)
		{
			// root节点下没有子节点了
			return true;
		}

		if (!loadAllDefDescriptions(componentTypeName, componentXml.get(), componentRootNode, pCompScriptDefModule))
		{
			ERROR_MSG(fmt::format("EntityDef::initialize: component[{}] error!\n",
				componentTypeName.c_str()));

			return false;
		}

		// 遍历所有的interface， 并将他们的方法和属性加入到模块中
		if (!loadInterfaces(componentDefFilePath, componentTypeName, componentXml.get(), componentRootNode, pCompScriptDefModule, true))
		{
			ERROR_MSG(fmt::format("EntityDef::loadComponents: failed to load component:{} interface.\n",
				componentTypeName.c_str()));

			return false;
		}

		// 加载父类所有的内容
		if (!loadParentClass(componentDefFilePath + "components/", componentTypeName, componentXml.get(), componentRootNode, pCompScriptDefModule))
		{
			ERROR_MSG(fmt::format("EntityDef::loadComponents: failed to load component:{} parentClass.\n",
				componentTypeName.c_str()));

			return false;
		}

		// 尝试加载detailLevel数据
		if (!loadDetailLevelInfo(componentDefFilePath, componentTypeName, componentXml.get(), componentRootNode, pCompScriptDefModule))
		{
			ERROR_MSG(fmt::format("EntityDef::loadComponents: failed to load component:{} DetailLevelInfo.\n",
				componentTypeName.c_str()));

			return false;
		}

		pCompScriptDefModule->autoMatchCompOwn();

		flags = ED_FLAG_UNKOWN;

		if (pCompScriptDefModule->hasBase())
			flags |= ED_FLAG_BASE;

		if (pCompScriptDefModule->hasCell())
			flags |= ED_FLAG_CELL_PUBLIC;

		if (pCompScriptDefModule->hasClient())
		{
			if (pCompScriptDefModule->hasBase())
				flags |= ED_FLAG_BASE_AND_CLIENT;

			if (pCompScriptDefModule->hasCell())
				flags |= (ED_FLAG_ALL_CLIENTS | ED_FLAG_CELL_PUBLIC_AND_OWN | ED_FLAG_OTHER_CLIENTS | ED_FLAG_OWN_CLIENT);
		}

		addComponentProperty(futype, componentTypeName, componentName, flags, isPersistent, isIdentifier,
			indexType, databaseLength, defaultStr, detailLevel, pScriptModule, pCompScriptDefModule);

		pScriptModule->addComponentDescription(componentName.c_str(), pCompScriptDefModule);
	}
	XML_FOR_END(implementsNode);

	return true;
}

//-------------------------------------------------------------------------------------
PropertyDescription* EntityDef::addComponentProperty(ENTITY_PROPERTY_UID utype,
	const std::string& componentTypeName,
	const std::string& componentName,
	uint32 flags,
	bool isPersistent,
	bool isIdentifier,
	std::string indexType,
	uint32 databaseLength,
	const std::string& defaultStr,
	DETAIL_TYPE detailLevel,
	ScriptDefModule* pScriptModule,
	ScriptDefModule* pCompScriptDefModule)
{
	DataType* pEntityComponentType = DataTypes::getDataType(componentTypeName, false);

	if (!pEntityComponentType)
		pEntityComponentType = new EntityComponentType(pCompScriptDefModule);

	PropertyDescription* propertyDescription = PropertyDescription::createDescription(utype, "EntityComponent",
		componentName, flags, isPersistent,
		pEntityComponentType, isIdentifier, indexType,
		databaseLength, defaultStr,
		detailLevel,"");

	bool ret = true;

	int32 hasBaseFlags = 0;
	int32 hasCellFlags = 0;
	int32 hasClientFlags = 0;

	hasBaseFlags = flags & ENTITY_BASE_DATA_FLAGS;
	if (hasBaseFlags > 0)
		pScriptModule->setBase(true);

	hasCellFlags = flags & ENTITY_CELL_DATA_FLAGS;
	if (hasCellFlags > 0)
		pScriptModule->setCell(true);

	hasClientFlags = flags & ENTITY_CLIENT_DATA_FLAGS;
	if (hasClientFlags > 0)
		pScriptModule->setClient(true);

	// 添加到模块中
	if (hasCellFlags > 0)
		ret = pScriptModule->addPropertyDescription(componentName.c_str(),
			propertyDescription, CELLAPP_TYPE);

	if (hasBaseFlags > 0)
		ret = pScriptModule->addPropertyDescription(componentName.c_str(),
			propertyDescription, BASEAPP_TYPE);

	if (hasClientFlags > 0)
		ret = pScriptModule->addPropertyDescription(componentName.c_str(),
			propertyDescription, CLIENT_TYPE);

	if (!ret)
	{
		ERROR_MSG(fmt::format("EntityDef::addComponentProperty({}): {}.\n",
			pScriptModule->getName(), componentName));

		SAFE_RELEASE(propertyDescription);
		return NULL;
	}

	g_logComponentPropertys[pScriptModule->getName()].push_back(propertyDescription);
	return propertyDescription;
}

//-------------------------------------------------------------------------------------
bool EntityDef::loadParentClass(const std::string& defFilePath,
								const std::string& moduleName,
								XML* defxml,
								TiXmlNode* defNode,
								ScriptDefModule* pScriptModule)
{
	TiXmlNode* parentClassNode = defxml->enterNode(defNode, "Parent");
	if(parentClassNode == NULL)
		return true;

	std::string parentClassName = defxml->getKey(parentClassNode);
	std::string parentClassfile = defFilePath + parentClassName + ".def";

	SmartPointer<XML> parentClassXml(new XML());
	if(!parentClassXml->openSection(parentClassfile.c_str()))
		return false;

	TiXmlNode* parentClassdefNode = parentClassXml->getRootNode();
	if(parentClassdefNode == NULL)
	{
		// root节点下没有子节点了
		return true;
	}

	// 加载def文件中的定义
	if(!loadDefInfo(defFilePath, parentClassName, parentClassXml.get(), parentClassdefNode, pScriptModule))
	{
		ERROR_MSG(fmt::format("EntityDef::loadParentClass: failed to load entity:{} parentClass.\n",
			moduleName.c_str()));

		return false;
	}

	return true;
}

//-------------------------------------------------------------------------------------
bool EntityDef::loadAllDefDescriptions(const std::string& moduleName,
									  XML* defxml,
									  TiXmlNode* defNode,
									  ScriptDefModule* pScriptModule)
{
	// 加载属性描述
	if(!loadDefPropertys(moduleName, defxml, defxml->enterNode(defNode, "Properties"), pScriptModule))
		return false;

	// 加载cell方法描述
	if(!loadDefCellMethods(moduleName, defxml, defxml->enterNode(defNode, "CellMethods"), pScriptModule))
	{
		ERROR_MSG(fmt::format("EntityDef::loadAllDefDescription:loadDefCellMethods[{}] is failed!\n",
			moduleName.c_str()));

		return false;
	}

	// 加载base方法描述
	if(!loadDefBaseMethods(moduleName, defxml, defxml->enterNode(defNode, "BaseMethods"), pScriptModule))
	{
		ERROR_MSG(fmt::format("EntityDef::loadAllDefDescription:loadDefBaseMethods[{}] is failed!\n",
			moduleName.c_str()));

		return false;
	}

	// 加载client方法描述
	if(!loadDefClientMethods(moduleName, defxml, defxml->enterNode(defNode, "ClientMethods"), pScriptModule))
	{
		ERROR_MSG(fmt::format("EntityDef::loadAllDefDescription:loadDefClientMethods[{}] is failed!\n",
			moduleName.c_str()));

		return false;
	}

	return true;
}

//-------------------------------------------------------------------------------------
bool EntityDef::validDefPropertyName(const std::string& name)
{
	int i = 0;

	while(true)
	{
		std::string limited = ENTITY_LIMITED_PROPERTYS[i];

		if(limited == "")
			break;

		if(name == limited)
			return false;

		++i;
	};

	PyObject* pyKBEModule =
		PyImport_ImportModule(const_cast<char*>("KBEngine"));

	PyObject* pyEntityModule =
		PyObject_GetAttrString(pyKBEModule, const_cast<char *>("Entity"));

	Py_DECREF(pyKBEModule);

	if (pyEntityModule != NULL)
	{
		PyObject* pyEntityAttr =
			PyObject_GetAttrString(pyEntityModule, const_cast<char *>(name.c_str()));

		if (pyEntityAttr != NULL)
		{
			Py_DECREF(pyEntityAttr);
			Py_DECREF(pyEntityModule);
			return false;
		}
		else
		{
			PyErr_Clear();
		}
	}
	else
	{
		PyErr_Clear();
	}

	Py_XDECREF(pyEntityModule);
	return true;
}

//-------------------------------------------------------------------------------------
bool EntityDef::calcDefPropertyUType(const std::string& moduleName,
	const std::string& name, int iUtype, ScriptDefModule* pScriptModule, ENTITY_PROPERTY_UID& outUtype)
{
	ENTITY_PROPERTY_UID futype = 0;
	outUtype = futype;

	if (iUtype > 0)
	{
		futype = iUtype;

		if (iUtype != int(futype))
		{
			ERROR_MSG(fmt::format("EntityDef::calcDefPropertyUType: 'Utype' has overflowed({} > 65535), is {}.{}!\n",
				iUtype, moduleName, name.c_str()));

			return false;
		}

		// 检查是否有重复的Utype
		std::vector<ENTITY_PROPERTY_UID>::iterator iter =
			std::find(g_propertyUtypes.begin(), g_propertyUtypes.end(), futype);

		if (iter != g_propertyUtypes.end())
		{
			bool foundConflict = false;

			PropertyDescription* pConflictPropertyDescription = pScriptModule->findPropertyDescription(futype, BASEAPP_TYPE);
			if (pConflictPropertyDescription)
			{
				ERROR_MSG(fmt::format("EntityDef::calcDefPropertyUType: {}.{}, 'Utype' {} Conflict({}.{} 'Utype' {})!\n",
					moduleName, name.c_str(), iUtype, moduleName, pConflictPropertyDescription->getName(), iUtype));

				foundConflict = true;
			}

			pConflictPropertyDescription = pScriptModule->findPropertyDescription(futype, CELLAPP_TYPE);
			if (pConflictPropertyDescription)
			{
				ERROR_MSG(fmt::format("EntityDef::calcDefPropertyUType: {}.{}, 'Utype' {} Conflict({}.{} 'Utype' {})!\n",
					moduleName, name.c_str(), iUtype, moduleName, pConflictPropertyDescription->getName(), iUtype));

				foundConflict = true;
			}

			pConflictPropertyDescription = pScriptModule->findPropertyDescription(futype, CLIENT_TYPE);
			if (pConflictPropertyDescription)
			{
				ERROR_MSG(fmt::format("EntityDef::calcDefPropertyUType: {}.{}, 'Utype' {} Conflict({}.{} 'Utype' {})!\n",
					moduleName, name.c_str(), iUtype, moduleName, pConflictPropertyDescription->getName(), iUtype));

				foundConflict = true;
			}

			if (foundConflict)
				return false;
		}

		g_propertyUtypes.push_back(futype);
	}
	else
	{
		while (true)
		{
			futype = g_propertyUtypeAuto++;
			std::vector<ENTITY_PROPERTY_UID>::iterator iter =
				std::find(g_propertyUtypes.begin(), g_propertyUtypes.end(), futype);

			if (iter == g_propertyUtypes.end())
				break;
		}

		g_propertyUtypes.push_back(futype);
	}

	outUtype = futype;
	return true;
}

//-------------------------------------------------------------------------------------
bool EntityDef::loadDefPropertys(const std::string& moduleName,
								 XML* xml,
								 TiXmlNode* defPropertyNode,
								 ScriptDefModule* pScriptModule)
{
	if(defPropertyNode)
	{
		XML_FOR_BEGIN(defPropertyNode)
		{
			ENTITY_PROPERTY_UID			futype = 0;
			uint32						flags = 0;
			int32						hasBaseFlags = 0;
			int32						hasCellFlags = 0;
			int32						hasClientFlags = 0;
			DataType*					dataType = NULL;
			bool						isPersistent = false;
			bool						isIdentifier = false;		// 是否是一个索引键
			uint32						databaseLength = 0;			// 这个属性在数据库中的长度
			std::string					indexType;
			DETAIL_TYPE					detailLevel = DETAIL_LEVEL_FAR;
			std::string					detailLevelStr = "";
			std::string					strType;
			std::string					strisPersistent;
			std::string					strFlags;
			std::string					strIdentifierNode;
			std::string					defaultStr;
			std::string					name = "";
			std::string					descriptionStr = "";

			name = xml->getKey(defPropertyNode);
			if(!validDefPropertyName(name))
			{
				ERROR_MSG(fmt::format("EntityDef::loadDefPropertys: '{}' is limited, in module({})!\n",
					name, moduleName));

				return false;
			}

			TiXmlNode* flagsNode = xml->enterNode(defPropertyNode->FirstChild(), "Flags");
			if(flagsNode)
			{
				strFlags = xml->getValStr(flagsNode);
				std::transform(strFlags.begin(), strFlags.end(), strFlags.begin(), toupper);

				ENTITYFLAGMAP::iterator iter = g_entityFlagMapping.find(strFlags.c_str());
				if(iter == g_entityFlagMapping.end())
				{
					ERROR_MSG(fmt::format("EntityDef::loadDefPropertys: not fount flags[{}], is {}.{}!\n",
						strFlags, moduleName, name));

					return false;
				}

				flags = iter->second;
				hasBaseFlags = flags & ENTITY_BASE_DATA_FLAGS;
				if(hasBaseFlags > 0)
					pScriptModule->setBase(true);

				hasCellFlags = flags & ENTITY_CELL_DATA_FLAGS;
				if(hasCellFlags > 0)
					pScriptModule->setCell(true);

				hasClientFlags = flags & ENTITY_CLIENT_DATA_FLAGS;
				if(hasClientFlags > 0)
					pScriptModule->setClient(true);

				if(hasBaseFlags <= 0 && hasCellFlags <= 0)
				{
					ERROR_MSG(fmt::format("EntityDef::loadDefPropertys: not fount flags[{}], is {}.{}!\n",
						strFlags.c_str(), moduleName, name.c_str()));

					return false;
				}
			}
			else
			{
				ERROR_MSG(fmt::format("EntityDef::loadDefPropertys: not fount flagsNode, is {}.{}!\n",
					moduleName, name.c_str()));

				return false;
			}

			TiXmlNode* persistentNode = xml->enterNode(defPropertyNode->FirstChild(), "Persistent");
			if(persistentNode)
			{
				strisPersistent = xml->getValStr(persistentNode);

				std::transform(strisPersistent.begin(), strisPersistent.end(),
					strisPersistent.begin(), tolower);

				if(strisPersistent == "true")
					isPersistent = true;
			}

			TiXmlNode* typeNode = xml->enterNode(defPropertyNode->FirstChild(), "Type");
			if(typeNode)
			{
				strType = xml->getValStr(typeNode);

				if(strType == "ARRAY")
				{
					FixedArrayType* dataType1 = new FixedArrayType();
					if(dataType1->initialize(xml, typeNode, moduleName + "_" + name))
						dataType = dataType1;
					else
						return false;
				}
				else
				{
					dataType = DataTypes::getDataType(strType);
				}

				if(dataType == NULL)
				{
					return false;
				}
			}
			else
			{
				ERROR_MSG(fmt::format("EntityDef::loadDefPropertys: not fount TypeNode, is {}.{}!\n",
					moduleName, name.c_str()));

				return false;
			}

			TiXmlNode* indexTypeNode = xml->enterNode(defPropertyNode->FirstChild(), "Index");
			if(indexTypeNode)
			{
				indexType = xml->getValStr(indexTypeNode);

				std::transform(indexType.begin(), indexType.end(),
					indexType.begin(), toupper);
			}

			TiXmlNode* identifierNode = xml->enterNode(defPropertyNode->FirstChild(), "Identifier");
			if(identifierNode)
			{
				strIdentifierNode = xml->getValStr(identifierNode);
				std::transform(strIdentifierNode.begin(), strIdentifierNode.end(),
					strIdentifierNode.begin(), tolower);

				if(strIdentifierNode == "true")
					isIdentifier = true;
			}

			TiXmlNode* databaseLengthNode = xml->enterNode(defPropertyNode->FirstChild(), "DatabaseLength");
			if(databaseLengthNode)
			{
				databaseLength = xml->getValInt(databaseLengthNode);
			}

			TiXmlNode* defaultValNode =
				xml->enterNode(defPropertyNode->FirstChild(), "Default");

			if(defaultValNode)
			{
				defaultStr = xml->getValStr(defaultValNode);
			}

			TiXmlNode* detailLevelNode =
				xml->enterNode(defPropertyNode->FirstChild(), "DetailLevel");

			if(detailLevelNode)
			{
				detailLevelStr = xml->getValStr(detailLevelNode);
				if(detailLevelStr == "FAR")
					detailLevel = DETAIL_LEVEL_FAR;
				else if(detailLevelStr == "MEDIUM")
					detailLevel = DETAIL_LEVEL_MEDIUM;
				else if(detailLevelStr == "NEAR")
					detailLevel = DETAIL_LEVEL_NEAR;
				else
					detailLevel = DETAIL_LEVEL_FAR;
			}



			TiXmlNode* descriptionNode = xml->enterNode(defPropertyNode->FirstChild(), "Description");
			if (descriptionNode) {
				//descriptionStr = xml->getValStr(descriptionNode);
				descriptionStr = strutil::kbe_unicodeTrim(descriptionNode->ToText()->Value());
				//descriptionStr = descriptionNode->ToText()->Value();

			}

			TiXmlNode* utypeValNode =
				xml->enterNode(defPropertyNode->FirstChild(), "Utype");

			if (!calcDefPropertyUType(moduleName, name, (utypeValNode ? xml->getValInt(utypeValNode) : -1), pScriptModule, futype))
				return false;

			// 产生一个属性描述实例
			PropertyDescription* propertyDescription = PropertyDescription::createDescription(futype, strType,
															name, flags, isPersistent,
															dataType, isIdentifier, indexType,
															databaseLength, defaultStr,
															detailLevel, descriptionStr);

			bool ret = true;

			// 添加到模块中
			if(hasCellFlags > 0)
				ret = pScriptModule->addPropertyDescription(name.c_str(),
						propertyDescription, CELLAPP_TYPE);

			if(hasBaseFlags > 0)
				ret = pScriptModule->addPropertyDescription(name.c_str(),
						propertyDescription, BASEAPP_TYPE);

			if(hasClientFlags > 0)
				ret = pScriptModule->addPropertyDescription(name.c_str(),
						propertyDescription, CLIENT_TYPE);

			if(!ret)
			{
				ERROR_MSG(fmt::format("EntityDef::addPropertyDescription({}): {}.\n",
					moduleName.c_str(), xml->getTxdoc()->Value()));

				return false;
			}
		}
		XML_FOR_END(defPropertyNode);
	}

	return true;
}

//-------------------------------------------------------------------------------------
bool EntityDef::loadDefCellMethods(const std::string& moduleName,
								   XML* xml,
								   TiXmlNode* defMethodNode,
								   ScriptDefModule* pScriptModule)
{
	if(defMethodNode)
	{
		XML_FOR_BEGIN(defMethodNode)
		{
			std::string name = xml->getKey(defMethodNode);
			MethodDescription* methodDescription = new MethodDescription(0, CELLAPP_TYPE, name);
			TiXmlNode* argNode = defMethodNode->FirstChild();

			// 可能没有参数
			if(argNode)
			{
				XML_FOR_BEGIN(argNode)
				{
					std::string argType = xml->getKey(argNode);

					if(argType == "Exposed")
					{
						methodDescription->setExposed();
					}
					else if(argType == "Arg")
					{
						DataType* dataType = NULL;
						TiXmlNode* typeNode = argNode->FirstChild();
						std::string strType = xml->getValStr(typeNode);

						if(strType == "ARRAY")
						{
							FixedArrayType* dataType1 = new FixedArrayType();
							if(dataType1->initialize(xml, typeNode, moduleName + "_" + name))
								dataType = dataType1;
						}
						else
						{
							dataType = DataTypes::getDataType(strType);
						}

						if(dataType == NULL)
						{
							ERROR_MSG(fmt::format("EntityDef::loadDefCellMethods: dataType[{}] not found, in {}!\n",
								strType.c_str(), name.c_str()));

							return false;
						}

						methodDescription->pushArgType(dataType);
					}
					else if(argType == "Utype")
					{
						TiXmlNode* typeNode = argNode->FirstChild();

						int iUtype = xml->getValInt(typeNode);
						ENTITY_METHOD_UID muid = iUtype;

						if (iUtype != int(muid))
						{
							ERROR_MSG(fmt::format("EntityDef::loadDefCellMethods: 'Utype' has overflowed({} > 65535), is {}.{}!\n",
								iUtype, moduleName, name.c_str()));

							return false;
						}

						methodDescription->setUType(muid);
						g_methodCusUtypes.push_back(muid);
					}
				}
				XML_FOR_END(argNode);
			}

			// 如果配置中没有设置过utype, 则产生
			if(methodDescription->getUType() <= 0)
			{
				ENTITY_METHOD_UID muid = 0;
				while(true)
				{
					muid = g_methodUtypeAuto++;
					std::vector<ENTITY_METHOD_UID>::iterator iterutype =
						std::find(g_methodCusUtypes.begin(), g_methodCusUtypes.end(), muid);

					if(iterutype == g_methodCusUtypes.end())
					{
						break;
					}
				}

				methodDescription->setUType(muid);
				g_methodCusUtypes.push_back(muid);
			}
			else
			{
				// 检查是否有重复的Utype
				ENTITY_METHOD_UID muid = methodDescription->getUType();
				std::vector<ENTITY_METHOD_UID>::iterator iter =
					std::find(g_methodCusUtypes.begin(), g_methodCusUtypes.end(), muid);

				if (iter != g_methodCusUtypes.end())
				{
					bool foundConflict = false;

					MethodDescription* pConflictMethodDescription = pScriptModule->findBaseMethodDescription(muid);
					if (pConflictMethodDescription)
					{
						ERROR_MSG(fmt::format("EntityDef::loadDefCellMethods: {}.{}, 'Utype' {} Conflict({}.{} 'Utype' {})!\n",
							moduleName, name.c_str(), muid, moduleName, pConflictMethodDescription->getName(), muid));

						foundConflict = true;
					}

					pConflictMethodDescription = pScriptModule->findCellMethodDescription(muid);
					if (pConflictMethodDescription)
					{
						ERROR_MSG(fmt::format("EntityDef::loadDefCellMethods: {}.{}, 'Utype' {} Conflict({}.{} 'Utype' {})!\n",
							moduleName, name.c_str(), muid, moduleName, pConflictMethodDescription->getName(), muid));

						foundConflict = true;
					}

					pConflictMethodDescription = pScriptModule->findClientMethodDescription(muid);
					if (pConflictMethodDescription)
					{
						ERROR_MSG(fmt::format("EntityDef::loadDefCellMethods: {}.{}, 'Utype' {} Conflict({}.{} 'Utype' {})!\n",
							moduleName, name.c_str(), muid, moduleName, pConflictMethodDescription->getName(), muid));

						foundConflict = true;
					}

					if (foundConflict)
						return false;
				}
			}

			if(!pScriptModule->addCellMethodDescription(name.c_str(), methodDescription))
				return false;
		}
		XML_FOR_END(defMethodNode);
	}

	return true;
}

//-------------------------------------------------------------------------------------
bool EntityDef::loadDefBaseMethods(const std::string& moduleName, XML* xml,
								   TiXmlNode* defMethodNode, ScriptDefModule* pScriptModule)
{
	if(defMethodNode)
	{
		XML_FOR_BEGIN(defMethodNode)
		{
			std::string name = xml->getKey(defMethodNode);
			MethodDescription* methodDescription = new MethodDescription(0, BASEAPP_TYPE, name);
			TiXmlNode* argNode = defMethodNode->FirstChild();

			// 可能没有参数
			if(argNode)
			{
				XML_FOR_BEGIN(argNode)
				{
					std::string argType = xml->getKey(argNode);

					if(argType == "Exposed")
					{
						methodDescription->setExposed();
					}
					else if(argType == "Arg")
					{
						DataType* dataType = NULL;
						TiXmlNode* typeNode = argNode->FirstChild();
						std::string strType = xml->getValStr(typeNode);

						if(strType == "ARRAY")
						{
							FixedArrayType* dataType1 = new FixedArrayType();
							if(dataType1->initialize(xml, typeNode, moduleName + "_" + name))
								dataType = dataType1;
						}
						else
						{
							dataType = DataTypes::getDataType(strType);
						}

						if(dataType == NULL)
						{
							ERROR_MSG(fmt::format("EntityDef::loadDefBaseMethods: dataType[{}] not found, in {}!\n",
								strType.c_str(), name.c_str()));

							return false;
						}

						methodDescription->pushArgType(dataType);
					}
					else if(argType == "Utype")
					{
						TiXmlNode* typeNode = argNode->FirstChild();

						int iUtype = xml->getValInt(typeNode);
						ENTITY_METHOD_UID muid = iUtype;

						if (iUtype != int(muid))
						{
							ERROR_MSG(fmt::format("EntityDef::loadDefBaseMethods: 'Utype' has overflowed({} > 65535), is {}.{}!\n",
								iUtype, moduleName, name.c_str()));

							return false;
						}

						methodDescription->setUType(muid);
						g_methodCusUtypes.push_back(muid);
					}
				}
				XML_FOR_END(argNode);
			}

			// 如果配置中没有设置过utype, 则产生
			if(methodDescription->getUType() <= 0)
			{
				ENTITY_METHOD_UID muid = 0;
				while(true)
				{
					muid = g_methodUtypeAuto++;
					std::vector<ENTITY_METHOD_UID>::iterator iterutype =
						std::find(g_methodCusUtypes.begin(), g_methodCusUtypes.end(), muid);

					if(iterutype == g_methodCusUtypes.end())
					{
						break;
					}
				}

				methodDescription->setUType(muid);
				g_methodCusUtypes.push_back(muid);
			}
			else
			{
				// 检查是否有重复的Utype
				ENTITY_METHOD_UID muid = methodDescription->getUType();
				std::vector<ENTITY_METHOD_UID>::iterator iter =
					std::find(g_methodCusUtypes.begin(), g_methodCusUtypes.end(), muid);

				if (iter != g_methodCusUtypes.end())
				{
					bool foundConflict = false;

					MethodDescription* pConflictMethodDescription = pScriptModule->findBaseMethodDescription(muid);
					if (pConflictMethodDescription)
					{
						ERROR_MSG(fmt::format("EntityDef::loadDefBaseMethods: {}.{}, 'Utype' {} Conflict({}.{} 'Utype' {})!\n",
							moduleName, name.c_str(), muid, moduleName, pConflictMethodDescription->getName(), muid));

						foundConflict = true;
					}

					pConflictMethodDescription = pScriptModule->findCellMethodDescription(muid);
					if (pConflictMethodDescription)
					{
						ERROR_MSG(fmt::format("EntityDef::loadDefBaseMethods: {}.{}, 'Utype' {} Conflict({}.{} 'Utype' {})!\n",
							moduleName, name.c_str(), muid, moduleName, pConflictMethodDescription->getName(), muid));

						foundConflict = true;
					}

					pConflictMethodDescription = pScriptModule->findClientMethodDescription(muid);
					if (pConflictMethodDescription)
					{
						ERROR_MSG(fmt::format("EntityDef::loadDefBaseMethods: {}.{}, 'Utype' {} Conflict({}.{} 'Utype' {})!\n",
							moduleName, name.c_str(), muid, moduleName, pConflictMethodDescription->getName(), muid));

						foundConflict = true;
					}

					if (foundConflict)
						return false;
				}
			}

			if(!pScriptModule->addBaseMethodDescription(name.c_str(), methodDescription))
				return false;
		}
		XML_FOR_END(defMethodNode);
	}

	return true;
}

//-------------------------------------------------------------------------------------
bool EntityDef::loadDefClientMethods(const std::string& moduleName, XML* xml,
									 TiXmlNode* defMethodNode, ScriptDefModule* pScriptModule)
{
	if(defMethodNode)
	{
		XML_FOR_BEGIN(defMethodNode)
		{
			std::string name = xml->getKey(defMethodNode);
			MethodDescription* methodDescription = new MethodDescription(0, CLIENT_TYPE, name);
			TiXmlNode* argNode = defMethodNode->FirstChild();

			// 可能没有参数
			if(argNode)
			{
				XML_FOR_BEGIN(argNode)
				{
					std::string argType = xml->getKey(argNode);

					if(argType == "Arg")
					{
						DataType* dataType = NULL;
						TiXmlNode* typeNode = argNode->FirstChild();
						std::string strType = xml->getValStr(typeNode);

						if(strType == "ARRAY")
						{
							FixedArrayType* dataType1 = new FixedArrayType();
							if(dataType1->initialize(xml, typeNode, moduleName + "_" + name))
								dataType = dataType1;
						}
						else
						{
							dataType = DataTypes::getDataType(strType);
						}

						if(dataType == NULL)
						{
							ERROR_MSG(fmt::format("EntityDef::loadDefClientMethods: dataType[{}] not found, in {}!\n",
								strType.c_str(), name.c_str()));

							return false;
						}

						methodDescription->pushArgType(dataType);
					}
					else if(argType == "Utype")
					{
						TiXmlNode* typeNode = argNode->FirstChild();

						int iUtype = xml->getValInt(typeNode);
						ENTITY_METHOD_UID muid = iUtype;

						if (iUtype != int(muid))
						{
							ERROR_MSG(fmt::format("EntityDef::loadDefClientMethods: 'Utype' has overflowed({} > 65535), is {}.{}!\n",
								iUtype, moduleName, name.c_str()));

							return false;
						}

						methodDescription->setUType(muid);
						g_methodCusUtypes.push_back(muid);
					}
				}
				XML_FOR_END(argNode);
			}

			// 如果配置中没有设置过utype, 则产生
			if(methodDescription->getUType() <= 0)
			{
				ENTITY_METHOD_UID muid = 0;
				while(true)
				{
					muid = g_methodUtypeAuto++;
					std::vector<ENTITY_METHOD_UID>::iterator iterutype =
						std::find(g_methodCusUtypes.begin(), g_methodCusUtypes.end(), muid);

					if(iterutype == g_methodCusUtypes.end())
					{
						break;
					}
				}

				methodDescription->setUType(muid);
				g_methodCusUtypes.push_back(muid);
			}
			else
			{
				// 检查是否有重复的Utype
				ENTITY_METHOD_UID muid = methodDescription->getUType();
				std::vector<ENTITY_METHOD_UID>::iterator iter =
					std::find(g_methodCusUtypes.begin(), g_methodCusUtypes.end(), muid);

				if (iter != g_methodCusUtypes.end())
				{
					bool foundConflict = false;

					MethodDescription* pConflictMethodDescription = pScriptModule->findBaseMethodDescription(muid);
					if (pConflictMethodDescription)
					{
						ERROR_MSG(fmt::format("EntityDef::loadDefClientMethods: {}.{}, 'Utype' {} Conflict({}.{} 'Utype' {})!\n",
							moduleName, name.c_str(), muid, moduleName, pConflictMethodDescription->getName(), muid));

						foundConflict = true;
					}

					pConflictMethodDescription = pScriptModule->findCellMethodDescription(muid);
					if (pConflictMethodDescription)
					{
						ERROR_MSG(fmt::format("EntityDef::loadDefClientMethods: {}.{}, 'Utype' {} Conflict({}.{} 'Utype' {})!\n",
							moduleName, name.c_str(), muid, moduleName, pConflictMethodDescription->getName(), muid));

						foundConflict = true;
					}

					pConflictMethodDescription = pScriptModule->findClientMethodDescription(muid);
					if (pConflictMethodDescription)
					{
						ERROR_MSG(fmt::format("EntityDef::loadDefClientMethods: {}.{}, 'Utype' {} Conflict({}.{} 'Utype' {})!\n",
							moduleName, name.c_str(), muid, moduleName, pConflictMethodDescription->getName(), muid));

						foundConflict = true;
					}

					if (foundConflict)
						return false;
				}
			}

			if(!pScriptModule->addClientMethodDescription(name.c_str(), methodDescription))
				return false;
		}
		XML_FOR_END(defMethodNode);
	}

	return true;
}

//-------------------------------------------------------------------------------------
bool EntityDef::isLoadScriptModule(ScriptDefModule* pScriptModule)
{
	switch(__loadComponentType)
	{
	case BASEAPP_TYPE:
		{
			if(!pScriptModule->hasBase())
				return false;

			break;
		}
	case CELLAPP_TYPE:
		{
			if(!pScriptModule->hasCell())
				return false;

			break;
		}
	case CLIENT_TYPE:
	case BOTS_TYPE:
		{
			if(!pScriptModule->hasClient())
				return false;

			break;
		}
	case TOOL_TYPE:
	{
		return false;
		break;
	}
	default:
		{
			if(!pScriptModule->hasCell())
				return false;

			break;
		}
	};

	return true;
}

//-------------------------------------------------------------------------------------
bool EntityDef::checkDefMethod(ScriptDefModule* pScriptModule,
							   PyObject* moduleObj, const std::string& moduleName)
{
	ScriptDefModule::METHODDESCRIPTION_MAP* methodDescrsPtr = NULL;

	PyObject* pyInspectModule =
		PyImport_ImportModule(const_cast<char*>("inspect"));

	PyObject* pyGetfullargspec = NULL;
	if (pyInspectModule)
	{
		Py_DECREF(pyInspectModule);

		pyGetfullargspec =
			PyObject_GetAttrString(pyInspectModule, const_cast<char *>("getfullargspec"));
	}
	else
	{
		SCRIPT_ERROR_CHECK();
	}

	switch (__loadComponentType)
	{
	case BASEAPP_TYPE:
		methodDescrsPtr =
			(ScriptDefModule::METHODDESCRIPTION_MAP*)&pScriptModule->getBaseMethodDescriptions();
		break;
	case CELLAPP_TYPE:
		methodDescrsPtr =
			(ScriptDefModule::METHODDESCRIPTION_MAP*)&pScriptModule->getCellMethodDescriptions();
		break;
	case CLIENT_TYPE:
	case BOTS_TYPE:
		methodDescrsPtr =
			(ScriptDefModule::METHODDESCRIPTION_MAP*)&pScriptModule->getClientMethodDescriptions();
		break;
	default:
		methodDescrsPtr =
			(ScriptDefModule::METHODDESCRIPTION_MAP*)&pScriptModule->getCellMethodDescriptions();
		break;
	};

	ScriptDefModule::METHODDESCRIPTION_MAP::iterator iter = methodDescrsPtr->begin();
	for (; iter != methodDescrsPtr->end(); ++iter)
	{
		PyObject* pyMethod =
			PyObject_GetAttrString(moduleObj, const_cast<char *>(iter->first.c_str()));

		if (pyMethod != NULL)
		{
			if (pyGetfullargspec)
			{
				// def方法中的参数个数
				size_t methodArgsSize = iter->second->getArgSize();

				PyObject* pyGetMethodArgs = PyObject_CallFunction(pyGetfullargspec,
					const_cast<char*>("(O)"), pyMethod);

				if (!pyGetMethodArgs)
				{
					SCRIPT_ERROR_CHECK();
				}
				else
				{
					PyObject* pyGetMethodArgsResult = PyObject_GetAttrString(pyGetMethodArgs, const_cast<char *>("args"));
					Py_DECREF(pyGetMethodArgs);

					if (!pyGetMethodArgsResult)
					{
						SCRIPT_ERROR_CHECK();
					}
					else
					{
						size_t argsSize = (size_t)PyObject_Size(pyGetMethodArgsResult);

						// 减去self这个参数
						KBE_ASSERT(argsSize > 0);
						argsSize -= 1;

						Py_DECREF(pyGetMethodArgsResult);

						// 检查参数的个数是否匹配
						if (methodArgsSize != argsSize)
						{
							// 如果不匹配， 并且是一个exposed方法，参数多了一个，可以理解为显示的加入了第一个参数callerID用于脚本检查调用者
							// 如果不是这种情况，一律视为参数不正确
							if (iter->second->isExposed() && methodArgsSize + 1 == argsSize)
							{
								iter->second->setExposed(MethodDescription::EXPOSED_AND_CALLER_CHECK);
							}
							else
							{
								ERROR_MSG(fmt::format("EntityDef::checkDefMethod: {}.{} parameter is incorrect, script argssize({}) != {}! defined in {}.def!\n",
									moduleName.c_str(), iter->first.c_str(), methodArgsSize, argsSize, moduleName));

								Py_DECREF(pyMethod);
								Py_XDECREF(pyGetfullargspec);
								return false;
							}
						}

						if (iter->second->isExposed())
						{
							if (iter->second->isExposed() != MethodDescription::EXPOSED_AND_CALLER_CHECK && iter->second->isCell())
							{
								WARNING_MSG(fmt::format("EntityDef::checkDefMethod: exposed of method: {}.{}{}!\n",
									moduleName.c_str(), iter->first.c_str(), (iter->second->isExposed() == MethodDescription::EXPOSED_AND_CALLER_CHECK ?
										"" : fmt::format(", check the caller can use \"def {}(self, callerID, ...)\", such as: if callerID == self.id", iter->first))));
							}
						}
					}
				}
			}

			Py_DECREF(pyMethod);
		}
		else
		{
			PyErr_Clear();

			PyObject* pyClassStr = PyObject_Str(moduleObj);
			const char* classStr = PyUnicode_AsUTF8AndSize(pyClassStr, NULL);

			ERROR_MSG(fmt::format("EntityDef::checkDefMethod: {} does not have method[{}], defined in {}.def!\n",
				classStr, iter->first.c_str(), moduleName));

			Py_DECREF(pyClassStr);
			Py_XDECREF(pyGetfullargspec);
			return false;
		}
	}

	Py_XDECREF(pyGetfullargspec);
	return true;
}

//-------------------------------------------------------------------------------------
void EntityDef::setScriptModuleHasComponentEntity(ScriptDefModule* pScriptModule,
												  bool has)
{
	switch(__loadComponentType)
	{
	case BASEAPP_TYPE:
		pScriptModule->setBase(has);
		return;
	case CELLAPP_TYPE:
		pScriptModule->setCell(has);
		return;
	case CLIENT_TYPE:
	case BOTS_TYPE:
		pScriptModule->setClient(has);
		return;
	default:
		pScriptModule->setCell(has);
		return;
	};
}

//-------------------------------------------------------------------------------------
PyObject* EntityDef::loadScriptModule(std::string moduleName)
{
	PyObject* pyModule =
		PyImport_ImportModule(const_cast<char*>(moduleName.c_str()));

	if (g_isReload && pyModule)
	{
		// Entity/Component 主模块也走文件版本戳过滤。
		// 文件未变化时只返回当前 sys.modules 中的模块对象，不再执行 PyImport_ReloadModule。
		std::string filePath = getPyModuleFilePath(pyModule);
		uint64 currStamp = 0;
		bool firstTrack = false;
		bool changed = filePath.empty() || isTrackedScriptFileChanged(moduleName, filePath, currStamp, firstTrack);
		rememberReloadFile(changed, moduleName, filePath);

		if (changed)
		{
			// 只有脚本文件确实变化才 reload 主模块。reload 后 EntityApp 会把在线 Entity/Component
			// 的 __class__ 指向新类；未变化模块则继续使用旧类，减少无意义更新和日志噪声。
			PyObject* pyReloadedModule = PyImport_ReloadModule(pyModule);
			Py_DECREF(pyModule);
			pyModule = pyReloadedModule;

			if (pyModule)
			{
				++g_reloadStats.reloadedModules;
				INFO_MSG(fmt::format("EntityDef::loadScriptModule: reload changed module={}, file={}, firstTrack={}, stamp={}.\n",
					moduleName, filePath, firstTrack, currStamp));
			}
			else
			{
				g_reloadStats.ok = false;
			}
		}
	}

	// 检查该模块路径是否是KBE脚本目录下的，防止因用户取名与python模块名称冲突而误导入了系统模块
	if (pyModule)
	{
		std::string userScriptsPath = Resmgr::getSingleton().getPyUserScriptsPath();
		std::string pyModulePath = getPyModuleFilePath(pyModule);

		// 非 reload 的首次导入阶段建立文件版本戳基线；后续 reloadScript 才能只刷新变更文件。
		if (!g_isReload)
			rememberInitialScriptFileStamp(moduleName, pyModulePath);

		strutil::kbe_replace(userScriptsPath, "/", "");
		strutil::kbe_replace(userScriptsPath, "\\", "");
		strutil::kbe_replace(pyModulePath, "/", "");
		strutil::kbe_replace(pyModulePath, "\\", "");

		if (pyModulePath.find(userScriptsPath) == std::string::npos)
		{
			WARNING_MSG(fmt::format("EntityDef::initialize: The script module name[{}] and system module name conflict!\n",
				moduleName.c_str()));

			pyModule = NULL;
		}
	}

	return pyModule;
}

//-------------------------------------------------------------------------------------
bool EntityDef::loadAllComponentScriptModules(std::string entitiesPath, std::vector<PyTypeObject*>& scriptBaseTypes)
{
	std::string entitiesFile = entitiesPath + "entities.xml";

	// 打开这个entities.xml文件
	// 允许纯脚本定义，则可能没有这个文件
	if (access(entitiesFile.c_str(), 0) != 0)
	{
		return true;
	}

	SmartPointer<XML> xml(new XML());
	if (!xml->openSection(entitiesFile.c_str()))
		return false;

	TiXmlNode* node = xml->getRootNode();
	if (node == NULL)
		return true;

	// 所有需要加载脚本的组件类别名称
	std::set<std::string> componentTypes;

	XML_FOR_BEGIN(node)
	{
		std::string moduleName = xml.get()->getKey(node);
		ScriptDefModule* pScriptModule = findScriptModule(moduleName.c_str());

		const ScriptDefModule::COMPONENTDESCRIPTION_MAP& componentDescrs = pScriptModule->getComponentDescrs();
		ScriptDefModule::COMPONENTDESCRIPTION_MAP::const_iterator comp_iter = componentDescrs.begin();
		for (; comp_iter != componentDescrs.end(); ++comp_iter)
		{
			componentTypes.insert(comp_iter->second->getName());
		}
	}
	XML_FOR_END(node);

	// 加载实体组件的脚本
	std::set<std::string>::iterator comp_iter = componentTypes.begin();
	for (; comp_iter != componentTypes.end(); ++comp_iter)
	{
		std::string componentScriptName = (*comp_iter);
		ScriptDefModule* pScriptModule = findScriptModule(componentScriptName.c_str());
		PyObject* pyModule = loadScriptModule(componentScriptName);

		if (pyModule == NULL)
		{
			// 如果当前是工具，如kbcmd， 那么无法加载脚本，如果某个模块没有客户端则删除它
			if (g_componentType == TOOL_TYPE)
			{
				if (!pScriptModule->hasClient())
				{
					goto ERASE_PROPERTYS;
				}
				else
				{
					PyErr_Clear();
					continue;
				}
			}

			// 是否加载这个模块 （取决于是否在def文件中定义了与当前组件相关的方法或者属性）
			if (isLoadScriptModule(pScriptModule))
			{
				ERROR_MSG(fmt::format("EntityDef::initialize: Could not load EntityComponentModule[{}]\n",
					componentScriptName.c_str()));

				PyErr_Print();
				return false;
			}

ERASE_PROPERTYS:
			std::vector<ScriptDefModulePtr>::iterator entityScriptModuleIter = EntityDef::__scriptModules.begin();
			for (; entityScriptModuleIter != EntityDef::__scriptModules.end(); ++entityScriptModuleIter)
			{
				ScriptDefModule::PROPERTYDESCRIPTION_MAP& propertyDescrs = (*entityScriptModuleIter)->getPropertyDescrs();
				ScriptDefModule::PROPERTYDESCRIPTION_MAP::iterator compPropertyInter = propertyDescrs.begin();
				for (; compPropertyInter != propertyDescrs.end();)
				{
					if (compPropertyInter->second->getDataType()->type() == DATA_TYPE_ENTITY_COMPONENT)
					{
						ScriptDefModule* pCompScriptModule = static_cast<EntityComponentType*>(compPropertyInter->second->getDataType())->pScriptDefModule();
						if (pCompScriptModule->getName() == componentScriptName)
						{
							uint32 flags = compPropertyInter->second->getFlags();

							if (g_componentType == BASEAPP_TYPE)
							{
								flags &= ~ENTITY_BASE_DATA_FLAGS;
								flags &= ~ED_FLAG_BASE_AND_CLIENT;
							}
							else if (g_componentType == CELLAPP_TYPE)
							{
								flags &= ~ENTITY_CELL_DATA_FLAGS;
								flags &= ~(ED_FLAG_ALL_CLIENTS | ED_FLAG_CELL_PUBLIC_AND_OWN | ED_FLAG_OTHER_CLIENTS | ED_FLAG_OWN_CLIENT);
							}
							else
							{
								flags &= ~ENTITY_CLIENT_DATA_FLAGS;
							}

							compPropertyInter->second->setFlags(flags);
							compPropertyInter->second->decRef();

							propertyDescrs.erase(compPropertyInter++);
							continue;
						}
					}

					compPropertyInter++;
				}
			}

			PyErr_Clear();

			// 必须在这里才设置， 在这之前设置会导致isLoadScriptModule失效，从而没有错误输出
			setScriptModuleHasComponentEntity(pScriptModule, false);
			continue;
		}

		setScriptModuleHasComponentEntity(pScriptModule, true);

		{
			std::vector<ScriptDefModulePtr>::iterator entityScriptModuleIter = EntityDef::__scriptModules.begin();
			for (; entityScriptModuleIter != EntityDef::__scriptModules.end(); ++entityScriptModuleIter)
			{
				std::vector<PropertyDescription*>& componentPropertys = g_logComponentPropertys[(*entityScriptModuleIter)->getName()];
				std::vector<PropertyDescription*>::iterator componentPropertysIter = componentPropertys.begin();
				for (; componentPropertysIter != componentPropertys.end(); ++componentPropertysIter)
				{
					PropertyDescription* pComponentPropertyDescription = (*componentPropertysIter);
					ScriptDefModule* pCompScriptModule = static_cast<EntityComponentType*>(pComponentPropertyDescription->getDataType())->pScriptDefModule();

					if (pCompScriptModule->getName() != componentScriptName)
						continue;

					uint32 pflags = pComponentPropertyDescription->getFlags();

					if (g_componentType == BASEAPP_TYPE)
					{
						pflags |= ENTITY_BASE_DATA_FLAGS;

						if(pCompScriptModule->hasClient())
							pflags |= ED_FLAG_BASE_AND_CLIENT;
					}
					else if (g_componentType == CELLAPP_TYPE)
					{
						pflags |= ENTITY_CELL_DATA_FLAGS;

						if (pCompScriptModule->hasClient())
							pflags |= (ED_FLAG_ALL_CLIENTS | ED_FLAG_CELL_PUBLIC_AND_OWN | ED_FLAG_OTHER_CLIENTS | ED_FLAG_OWN_CLIENT);
					}
					else
					{
						pflags |= ENTITY_CLIENT_DATA_FLAGS;
					}

					pComponentPropertyDescription->setFlags(pflags);
					if (pComponentPropertyDescription->isPersistent() && pCompScriptModule->numPropertys() == 0)
					{
						pComponentPropertyDescription->isPersistent(false);

						if ((*entityScriptModuleIter)->findPersistentPropertyDescription(pComponentPropertyDescription->getUType()))
						{
							(*entityScriptModuleIter)->getPersistentPropertyDescriptions().erase(pComponentPropertyDescription->getName());
							(*entityScriptModuleIter)->getPersistentPropertyDescriptions_uidmap().erase(pComponentPropertyDescription->getUType());
						}
					}

					if ((*entityScriptModuleIter)->findPropertyDescription(pComponentPropertyDescription->getName(), g_componentType) != pComponentPropertyDescription)
					{
						(*entityScriptModuleIter)->addPropertyDescription(pComponentPropertyDescription->getName(), pComponentPropertyDescription, g_componentType, true);
					}
				}
			}
		}

		PyObject* pyClass =
			PyObject_GetAttrString(pyModule, const_cast<char *>(componentScriptName.c_str()));

		if (pyClass == NULL)
		{
			ERROR_MSG(fmt::format("EntityDef::initialize: Could not find ComponentClass[{}]\n",
				componentScriptName.c_str()));

			return false;
		}
		else
		{
			std::string typeNames = "";
			bool valid = false;
			std::vector<PyTypeObject*>::iterator iter = scriptBaseTypes.begin();
			for (; iter != scriptBaseTypes.end(); ++iter)
			{
				if (!PyObject_IsSubclass(pyClass, (PyObject *)(*iter)))
				{
					typeNames += "'";
					typeNames += (*iter)->tp_name;
					typeNames += "'";
				}
				else
				{
					valid = true;
					break;
				}
			}

			if (!valid)
			{
				ERROR_MSG(fmt::format("EntityDef::initialize: ComponentClass {} is not derived from KBEngine.[{}]\n",
					componentScriptName.c_str(), typeNames.c_str()));

				return false;
			}
		}

		if (!PyType_Check(pyClass))
		{
			ERROR_MSG(fmt::format("EntityDef::initialize: ComponentClass[{}] is valid!\n",
				componentScriptName.c_str()));

			return false;
		}

		if (!checkDefMethod(pScriptModule, pyClass, componentScriptName))
		{
			ERROR_MSG(fmt::format("EntityDef::initialize: ComponentClass[{}] checkDefMethod is failed!\n",
				componentScriptName.c_str()));

			return false;
		}

		DEBUG_MSG(fmt::format("loaded component-script:{}({}).\n", componentScriptName.c_str(),
			pScriptModule->getUType()));

		pScriptModule->setScriptType((PyTypeObject *)pyClass);
		S_RELEASE(pyModule);
	}

	g_logComponentPropertys.clear();
	return true;
}

//-------------------------------------------------------------------------------------
std::string EntityDef::isSubClass(PyObject* pyClass)
{
	std::string typeNames = "";
	bool valid = false;

	std::vector<PyTypeObject*>::iterator iter = __scriptBaseTypes.begin();
	for (; iter != __scriptBaseTypes.end(); ++iter)
	{
		if (!PyObject_IsSubclass(pyClass, (PyObject *)(*iter)))
		{
			typeNames += "'";
			typeNames += (*iter)->tp_name;
			typeNames += "'";
		}
		else
		{
			valid = true;
			break;
		}
	}

	if (!valid)
		return typeNames;

	return "";
}

//-------------------------------------------------------------------------------------
bool EntityDef::loadAllEntityScriptModules(std::string entitiesPath,
									std::vector<PyTypeObject*>& scriptBaseTypes)
{
	std::string entitiesFile = entitiesPath + "entities.xml";

	if (!loadAllComponentScriptModules(entitiesPath, scriptBaseTypes))
		return false;

	// 所有需要加载脚本的组件类别名称
	std::set<std::string> checkedComponentTypes;

	std::vector<std::string> moduleNames;
	const std::vector<PluginEntityDescriptor>& pluginEntities = PluginManager::instance().entities();
	for (std::vector<PluginEntityDescriptor>::const_iterator iter = pluginEntities.begin(); iter != pluginEntities.end(); ++iter)
	{
		// EntityDef 注册已经按插件前置完成，这里导入 Python Entity 脚本时保持相同顺序。
		moduleNames.push_back(iter->name);
	}

	if (access(entitiesFile.c_str(), 0) == 0)
	{
		SmartPointer<XML> xml(new XML());
		if(!xml->openSection(entitiesFile.c_str()))
			return false;

		TiXmlNode* node = xml->getRootNode();
		if(node != NULL)
		{
			XML_FOR_BEGIN(node)
			{
				moduleNames.push_back(xml.get()->getKey(node));
			}
			XML_FOR_END(node);
		}
	}

	for (std::vector<std::string>::iterator moduleIter = moduleNames.begin(); moduleIter != moduleNames.end(); ++moduleIter)
	{
		std::string moduleName = *moduleIter;
		ScriptDefModule* pScriptModule = findScriptModule(moduleName.c_str());

		PyObject* pyModule = loadScriptModule(moduleName);
		if (pyModule == NULL)
		{
			// 是否加载这个模块 （取决于是否在def文件中定义了与当前组件相关的方法或者属性）
			if(isLoadScriptModule(pScriptModule))
			{
				ERROR_MSG(fmt::format("EntityDef::initialize: Could not load EntityModule[{}]\n",
					moduleName.c_str()));

				PyErr_Print();
				return false;
			}

			PyErr_Clear();

			// 必须在这里才设置， 在这之前设置会导致isLoadScriptModule失效，从而没有错误输出
			setScriptModuleHasComponentEntity(pScriptModule, false);
			continue;
		}

		setScriptModuleHasComponentEntity(pScriptModule, true);

		PyObject* pyClass =
			PyObject_GetAttrString(pyModule, const_cast<char *>(moduleName.c_str()));

		if (pyClass == NULL)
		{
			ERROR_MSG(fmt::format("EntityDef::initialize: Could not find EntityClass[{}]\n",
				moduleName.c_str()));

			return false;
		}
		else
		{
			std::string typeNames = isSubClass(pyClass);

			if(typeNames.size() > 0)
			{
				ERROR_MSG(fmt::format("EntityDef::initialize: EntityClass {} is not derived from KBEngine.[{}]\n",
					moduleName.c_str(), typeNames.c_str()));

				return false;
			}
		}

		if(!PyType_Check(pyClass))
		{
			ERROR_MSG(fmt::format("EntityDef::initialize: EntityClass[{}] is valid!\n",
				moduleName.c_str()));

			return false;
		}

		if(!checkDefMethod(pScriptModule, pyClass, moduleName))
		{
			ERROR_MSG(fmt::format("EntityDef::initialize: EntityClass[{}] checkDefMethod is failed!\n",
				moduleName.c_str()));

			return false;
		}

		DEBUG_MSG(fmt::format("loaded entity-script:{}({}).\n", moduleName.c_str(),
			pScriptModule->getUType()));

		pScriptModule->setScriptType((PyTypeObject *)pyClass);
		S_RELEASE(pyModule);

		// 查找实体在该进程上是否有相对应需要实现的实体组件，如果没有则提示错误
		const ScriptDefModule::COMPONENTDESCRIPTION_MAP& componentDescrs = pScriptModule->getComponentDescrs();
		ScriptDefModule::COMPONENTDESCRIPTION_MAP::const_iterator comp_iter = componentDescrs.begin();
		for (; comp_iter != componentDescrs.end(); ++comp_iter)
		{
			std::string componentScriptName = comp_iter->second->getName();

			std::set<std::string>::iterator fiter = checkedComponentTypes.find(componentScriptName);
			if (fiter != checkedComponentTypes.end())
				continue;

			ScriptDefModule* pComponentScriptModule = findScriptModule(componentScriptName.c_str());

			// 是否加载这个模块，如果需要加载且当前没有模块则提示错误
			if (!pComponentScriptModule->getScriptType() && isLoadScriptModule(pComponentScriptModule))
			{
				ERROR_MSG(fmt::format("EntityDef::initialize: Could not load ComponentModule[{}]\n",
					componentScriptName.c_str()));

				PyErr_Print();
				return false;
			}

			checkedComponentTypes.insert(componentScriptName);
		}
	}

	return true;
}

//-------------------------------------------------------------------------------------
ScriptDefModule* EntityDef::findScriptModule(ENTITY_SCRIPT_UID utype, bool notFoundOutErr)
{
	// utype 最小为1
	if (utype == 0 || utype >= __scriptModules.size() + 1)
	{
		if (notFoundOutErr)
		{
			ERROR_MSG(fmt::format("EntityDef::findScriptModule: is not exist(utype:{})!\n", utype));
		}

		return NULL;
	}

	return __scriptModules[utype - 1].get();
}

//-------------------------------------------------------------------------------------
ScriptDefModule* EntityDef::findScriptModule(const char* scriptName, bool notFoundOutErr)
{
	std::map<std::string, ENTITY_SCRIPT_UID>::iterator iter =
		__scriptTypeMappingUType.find(scriptName);

	if(iter == __scriptTypeMappingUType.end())
	{
		if (notFoundOutErr)
		{
			ERROR_MSG(fmt::format("EntityDef::findScriptModule: [{}] not found!\n", scriptName));
		}

		return NULL;
	}

	return findScriptModule(iter->second);
}

//-------------------------------------------------------------------------------------
ScriptDefModule* EntityDef::findOldScriptModule(const char* scriptName, bool notFoundOutErr)
{
	std::map<std::string, ENTITY_SCRIPT_UID>::iterator iter =
		__oldScriptTypeMappingUType.find(scriptName);

	if(iter == __oldScriptTypeMappingUType.end())
	{
		if (notFoundOutErr)
		{
			ERROR_MSG(fmt::format("EntityDef::findOldScriptModule: [{}] not found!\n", scriptName));
		}

		return NULL;
	}

	if (iter->second >= __oldScriptModules.size() + 1)
	{
		if (notFoundOutErr)
		{
			ERROR_MSG(fmt::format("EntityDef::findOldScriptModule: is not exist(utype:{})!\n", iter->second));
		}

		return NULL;
	}

	return __oldScriptModules[iter->second - 1].get();

}

//-------------------------------------------------------------------------------------
bool EntityDef::installScript(PyObject* mod)
{
	if(_isInit)
		return true;

	script::PyMemoryStream::installScript(NULL);
	APPEND_SCRIPT_MODULE_METHOD(mod, MemoryStream, script::PyMemoryStream::py_new, METH_VARARGS, 0);

	EntityCall::installScript(NULL);
	EntityComponentCall::installScript(NULL);
	FixedArray::installScript(NULL);
	FixedDict::installScript(NULL);
	VolatileInfo::installScript(NULL);
	script::entitydef::installModule("EntityDef");

	_isInit = true;
	return true;
}

//-------------------------------------------------------------------------------------
bool EntityDef::uninstallScript()
{
	if(_isInit)
	{
		script::PyMemoryStream::uninstallScript();
		EntityCall::uninstallScript();
		EntityComponentCall::uninstallScript();
		FixedArray::uninstallScript();
		FixedDict::uninstallScript();
		VolatileInfo::uninstallScript();
		script::entitydef::uninstallModule();
	}

	return script::entitydef::finalise() && EntityDef::finalise();
}

//-------------------------------------------------------------------------------------
bool EntityDef::initializeWatcher()
{
	return script::entitydef::initializeWatcher();
}

//-------------------------------------------------------------------------------------
}
