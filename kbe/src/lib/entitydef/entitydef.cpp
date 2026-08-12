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


#include "entitydef.h"
#include "scriptdef_module.h"
#include "method_utype_allocator.h"
#include "resmgr/plugins/plugin_manager.h"
#include "datatypes.h"
#include "common.h"
#include "pyscript/py_memorystream.h"
#include "resmgr/resmgr.h"
#include "common/smartpointer.h"
#include "entitydef/volatileinfo.h"
#include "entitydef/entity_call.h"

#include <cmath>
#include <set>
#include <sys/stat.h>

#ifndef CODE_INLINE
#include "entitydef.inl"
#endif

namespace KBEngine{
std::vector<ScriptDefModulePtr>	EntityDef::__scriptModules;
std::vector<ScriptDefModulePtr>	EntityDef::__oldScriptModules;

std::map<std::string, ENTITY_SCRIPT_UID> EntityDef::__scriptTypeMappingUType;
std::map<std::string, ENTITY_SCRIPT_UID> EntityDef::__oldScriptTypeMappingUType;

COMPONENT_TYPE EntityDef::__loadComponentType;
std::vector<PyTypeObject*> EntityDef::__scriptBaseTypes;
std::string EntityDef::__entitiesPath;
EntityDef::Context EntityDef::__context;

KBE_MD5 EntityDef::__md5;
KBE_MD5 EntityDef::__clientMd5;
bool EntityDef::_isInit = false;
bool g_isReload = false;

bool EntityDef::__entityAliasID = false;
bool EntityDef::__entitydefAliasID = false;

// 文件戳按物理路径记录，避免包名别名绕过变更检测。
// File stamps are keyed by physical path so package aliases cannot bypass change detection.
static std::map<std::string, uint64> g_scriptFileStamps;
static ReloadScriptDefStats g_reloadStats;

static std::string normalizeScriptPath(std::string path)
{
	strutil::kbe_replace(path, "\\", "/");
#if KBE_PLATFORM == PLATFORM_WIN32
	std::transform(path.begin(), path.end(), path.begin(), ::tolower);
#endif
	return path;
}

static uint64 scriptFileStamp(const std::string& filePath)
{
#if KBE_PLATFORM == PLATFORM_WIN32
	WIN32_FILE_ATTRIBUTE_DATA fileInfo;
	if (GetFileAttributesExA(filePath.c_str(), GetFileExInfoStandard, &fileInfo))
	{
		ULARGE_INTEGER modified;
		modified.HighPart = fileInfo.ftLastWriteTime.dwHighDateTime;
		modified.LowPart = fileInfo.ftLastWriteTime.dwLowDateTime;
		ULARGE_INTEGER size;
		size.HighPart = fileInfo.nFileSizeHigh;
		size.LowPart = fileInfo.nFileSizeLow;
		return static_cast<uint64>(modified.QuadPart ^ (size.QuadPart << 1));
	}
#endif

	struct stat st;
	if (stat(filePath.c_str(), &st) != 0)
		return 0;
	return (static_cast<uint64>(st.st_mtime) << 32) ^ static_cast<uint64>(st.st_size);
}

static std::string pythonModuleFile(PyObject* pyModule)
{
	PyObject* pyFile = PyModule_GetFilenameObject(pyModule);
	if (!pyFile)
	{
		PyErr_Clear();
		return "";
	}

	const char* file = PyUnicode_AsUTF8(pyFile);
	std::string result = file ? normalizeScriptPath(file) : "";
	if (!file)
		PyErr_Clear();
	Py_DECREF(pyFile);
	return result;
}

static std::vector<std::string> reloadScriptRoots()
{
	std::vector<std::string> roots;
	std::string componentRoot = Resmgr::getSingleton().getPyUserScriptsPath();
	const char* componentFolder = getComponentFolder(g_componentType);
	if (componentFolder[0] != '\0')
		componentRoot += componentFolder;
	roots.push_back(normalizeScriptPath(componentRoot));

	std::vector<std::string> pluginRoots = PluginManager::instance().getComponentPythonPaths(g_componentType);
	for (std::vector<std::string>::iterator iter = pluginRoots.begin(); iter != pluginRoots.end(); ++iter)
		roots.push_back(normalizeScriptPath(*iter));
	return roots;
}

static bool isReloadableScriptFile(const std::string& filePath, const std::vector<std::string>& roots)
{
	if (filePath.size() < 3 || filePath.substr(filePath.size() - 3) != ".py")
		return false;

	for (std::vector<std::string>::const_iterator iter = roots.begin(); iter != roots.end(); ++iter)
	{
		if (filePath.compare(0, iter->size(), *iter) == 0)
			return true;
	}
	return false;
}

static std::set<std::string> pluginEntryModuleNames()
{
	std::set<std::string> names;
	const std::vector<PluginDescriptor>& plugins = PluginManager::instance().plugins();
	const char* componentFolder = getComponentFolder(g_componentType);
	for (std::vector<PluginDescriptor>::const_iterator plugin = plugins.begin(); plugin != plugins.end(); ++plugin)
	{
		std::map<COMPONENT_TYPE, PluginComponentDescriptor>::const_iterator component =
			plugin->components.find(g_componentType);
		if (component == plugin->components.end() || component->second.entry.empty())
			continue;

		std::string entry = component->second.entry;
		if (entry.size() > 3 && entry.substr(entry.size() - 3) == ".py")
			entry.erase(entry.size() - 3);
		strutil::kbe_replace(entry, "\\", ".");
		strutil::kbe_replace(entry, "/", ".");
		if (entry.compare(0, 8, "plugins.") == 0)
			names.insert(entry);
		else if (componentFolder[0] != '\0')
			names.insert("plugins." + plugin->name + "." + componentFolder + "." + entry);
	}
	return names;
}

static void collectDeclaredTypes(PyObject* module, const std::string& moduleName,
	std::map<std::string, PyObject*>& oldTypes)
{
	PyObject* dict = PyModule_GetDict(module);
	PyObject* key = NULL;
	PyObject* value = NULL;
	Py_ssize_t pos = 0;
	while (dict && PyDict_Next(dict, &pos, &key, &value))
	{
		if (!PyUnicode_Check(key) || !PyType_Check(value))
			continue;
		PyObject* typeModule = PyObject_GetAttrString(value, "__module__");
		if (!typeModule)
		{
			PyErr_Clear();
			continue;
		}
		const char* typeModuleName = PyUnicode_AsUTF8(typeModule);
		bool declaredHere = typeModuleName && moduleName == typeModuleName;
		Py_DECREF(typeModule);
		if (!declaredHere)
			continue;
		const char* typeName = PyUnicode_AsUTF8(key);
		if (!typeName)
		{
			PyErr_Clear();
			continue;
		}
		Py_INCREF(value);
		oldTypes[typeName] = value;
	}
}

static void patchOldTypes(PyObject* reloadedModule, std::map<std::string, PyObject*>& oldTypes)
{
	for (std::map<std::string, PyObject*>::iterator type = oldTypes.begin(); type != oldTypes.end(); ++type)
	{
		PyObject* newType = PyObject_GetAttrString(reloadedModule, type->first.c_str());
		if (!newType)
		{
			PyErr_Clear();
			Py_DECREF(type->second);
			continue;
		}

		if (PyType_Check(newType) && newType != type->second)
		{
			PyObject* newDict = PyObject_GetAttrString(newType, "__dict__");
			PyObject* items = newDict ? PyMapping_Items(newDict) : NULL;
			if (items)
			{
				for (Py_ssize_t i = 0; i < PyList_Size(items); ++i)
				{
					PyObject* item = PyList_GetItem(items, i);
					PyObject* attr = PyTuple_GET_ITEM(item, 0);
					PyObject* value = PyTuple_GET_ITEM(item, 1);
					const char* attrName = PyUnicode_Check(attr) ? PyUnicode_AsUTF8(attr) : NULL;
					if (!attrName || strcmp(attrName, "__dict__") == 0 || strcmp(attrName, "__weakref__") == 0)
						continue;
					if (PyObject_SetAttrString(type->second, attrName, value) == -1)
						PyErr_Clear();
				}
				Py_DECREF(items);
			}
			else
			{
				PyErr_Clear();
			}
			Py_XDECREF(newDict);
			PyType_Modified(reinterpret_cast<PyTypeObject*>(type->second));
		}

		Py_DECREF(newType);
		Py_DECREF(type->second);
	}
	oldTypes.clear();
}

static void releaseOldTypes(std::map<std::string, PyObject*>& oldTypes)
{
	for (std::map<std::string, PyObject*>::iterator type = oldTypes.begin(); type != oldTypes.end(); ++type)
		Py_DECREF(type->second);
	oldTypes.clear();
}

static void rememberLoadedScriptStamps()
{
	const std::vector<std::string> roots = reloadScriptRoots();
	PyObject* modules = PyImport_GetModuleDict();
	PyObject* key = NULL;
	PyObject* value = NULL;
	Py_ssize_t pos = 0;
	while (PyDict_Next(modules, &pos, &key, &value))
	{
		if (!PyModule_Check(value))
			continue;
		std::string filePath = pythonModuleFile(value);
		if (!isReloadableScriptFile(filePath, roots))
			continue;
		uint64 stamp = scriptFileStamp(filePath);
		if (stamp > 0)
			g_scriptFileStamps[filePath] = stamp;
	}
}

static bool reloadChangedScriptModules()
{
	struct Candidate
	{
		std::string moduleName;
		std::string filePath;
		uint64 stamp;
		PyObject* module;
		bool pluginEntry;
		std::map<std::string, PyObject*> oldTypes;
	};

	const std::vector<std::string> roots = reloadScriptRoots();
	const std::set<std::string> pluginEntries = pluginEntryModuleNames();
	std::vector<Candidate> candidates;
	std::set<std::string> changedFiles;
	std::set<std::string> unchangedFiles;
	PyObject* modules = PyImport_GetModuleDict();
	PyObject* key = NULL;
	PyObject* value = NULL;
	Py_ssize_t pos = 0;
	while (PyDict_Next(modules, &pos, &key, &value))
	{
		if (!PyUnicode_Check(key) || !PyModule_Check(value))
			continue;
		std::string filePath = pythonModuleFile(value);
		if (!isReloadableScriptFile(filePath, roots))
			continue;

		uint64 stamp = scriptFileStamp(filePath);
		std::map<std::string, uint64>::const_iterator previous = g_scriptFileStamps.find(filePath);
		if (stamp == 0 || (previous != g_scriptFileStamps.end() && previous->second == stamp))
		{
			unchangedFiles.insert(filePath);
			continue;
		}

		const char* moduleName = PyUnicode_AsUTF8(key);
		if (!moduleName)
		{
			PyErr_Clear();
			continue;
		}
		Candidate candidate = { moduleName, filePath, stamp, value,
			pluginEntries.find(moduleName) != pluginEntries.end(), std::map<std::string, PyObject*>() };
		Py_INCREF(value);
		if (!candidate.pluginEntry)
			collectDeclaredTypes(value, moduleName, candidate.oldTypes);
		candidates.push_back(candidate);
		changedFiles.insert(filePath);
	}

	g_reloadStats.changedFiles = static_cast<uint32>(changedFiles.size());
	g_reloadStats.skippedFiles = static_cast<uint32>(unchangedFiles.size());
	std::map<std::string, bool> fileSucceeded;
	for (std::set<std::string>::const_iterator iter = changedFiles.begin(); iter != changedFiles.end(); ++iter)
		fileSucceeded[*iter] = true;

	for (std::vector<Candidate>::iterator iter = candidates.begin(); iter != candidates.end(); ++iter)
	{
		// 插件入口由 PluginRuntime 统一执行 onFini -> reload -> onInit，通用扫描只负责发现变更。
		// PluginRuntime owns onFini -> reload -> onInit for entry modules; the generic scan only detects their changes.
		if (iter->pluginEntry)
		{
			Py_DECREF(iter->module);
			continue;
		}

		PyObject* reloaded = PyImport_ReloadModule(iter->module);
		Py_DECREF(iter->module);
		if (!reloaded)
		{
			ERROR_MSG(fmt::format("EntityDef::reload: failed to reload module [{}], file=[{}].\n",
				iter->moduleName, iter->filePath));
			PyErr_Print();
			fileSucceeded[iter->filePath] = false;
			g_reloadStats.ok = false;
			releaseOldTypes(iter->oldTypes);
			continue;
		}
		patchOldTypes(reloaded, iter->oldTypes);
		Py_DECREF(reloaded);
		++g_reloadStats.reloadedModules;
	}

	for (std::vector<Candidate>::const_iterator iter = candidates.begin(); iter != candidates.end(); ++iter)
	{
		// 只有同一物理文件的所有模块别名都成功后才提交文件戳，失败项下一轮仍会重试。
		// Commit a file stamp only after every module alias succeeds, so failures remain retryable.
		if (fileSucceeded[iter->filePath])
			g_scriptFileStamps[iter->filePath] = iter->stamp;
	}

	for (std::set<std::string>::const_iterator iter = changedFiles.begin(); iter != changedFiles.end(); ++iter)
		INFO_MSG(fmt::format("EntityDef::reload: changed script file [{}].\n", *iter));
	return g_reloadStats.ok;
}

// 方法 UType 的查找发生在各自通信域，因此三个域必须独立分配。
// Method UType lookup is scoped to its communication domain, so each domain owns an allocator.
struct ModuleMethodUTypeAllocators
{
	MethodUTypeAllocator cell;
	MethodUTypeAllocator base;
	MethodUTypeAllocator client;
};

std::map<ScriptDefModule*, ModuleMethodUTypeAllocators> g_methodUTypeAllocators;

ENTITY_PROPERTY_UID g_propertyUtypeAuto = 1;
std::vector<ENTITY_PROPERTY_UID> g_propertyUtypes;

static MethodUTypeAllocator& methodUTypeAllocator(ScriptDefModule* pScriptModule,
	COMPONENT_TYPE domain)
{
	ModuleMethodUTypeAllocators& allocators = g_methodUTypeAllocators[pScriptModule];
	if (domain == CELLAPP_TYPE)
		return allocators.cell;
	if (domain == BASEAPP_TYPE)
		return allocators.base;

	return allocators.client;
}

static MethodDescription* findMethodDescription(ScriptDefModule* pScriptModule,
	COMPONENT_TYPE domain, ENTITY_METHOD_UID utype)
{
	if (domain == CELLAPP_TYPE)
		return pScriptModule->findCellMethodDescription(utype);
	if (domain == BASEAPP_TYPE)
		return pScriptModule->findBaseMethodDescription(utype);

	return pScriptModule->findClientMethodDescription(utype);
}

static bool assignMethodUType(const std::string& moduleName,
	MethodDescription* pMethodDescription, ScriptDefModule* pScriptModule)
{
	const COMPONENT_TYPE domain = static_cast<COMPONENT_TYPE>(pMethodDescription->domain());
	MethodUTypeAllocator& allocator = methodUTypeAllocator(pScriptModule, domain);
	ENTITY_METHOD_UID utype = pMethodDescription->getUType();

	if (utype != 0)
	{
		MethodDescription* pConflict = findMethodDescription(pScriptModule, domain, utype);
		if (pConflict != NULL)
		{
			ERROR_MSG(fmt::format(
				"EntityDef::assignMethodUType: {}.{}, 'Utype' {} conflicts with {}.{} in the same domain({}).\n",
				moduleName, pMethodDescription->getName(), utype, moduleName,
				pConflict->getName(), domain));
			return false;
		}

		allocator.reserve(utype);
		return true;
	}

	// 暴露方法和 ClientMethods 属于客户端协议，从低位稳定增长；私有方法从高位增长，互不推移。
	// Exposed methods and ClientMethods are client protocol. They grow from the low end,
	// while private server methods grow from the high end so either side cannot renumber the other.
	const bool clientVisible = domain == CLIENT_TYPE || pMethodDescription->isExposed() != MethodDescription::NO_EXPOSED;
	const bool allocated = clientVisible ?
		allocator.allocateClientVisible(utype) : allocator.allocateServerPrivate(utype);
	if (!allocated)
	{
		ERROR_MSG(fmt::format(
			"EntityDef::assignMethodUType: no free method UType remains for {}.{} in domain({}).\n",
			moduleName, pMethodDescription->getName(), domain));
		return false;
	}

	pMethodDescription->setUType(utype);
	return true;
}

template<typename T>
static void appendClientDigestValue(KBE_MD5& digest, const T& value)
{
	digest.append(&value, sizeof(T));
}

static void appendClientDigestString(KBE_MD5& digest, const char* value)
{
	const size_t length = value == NULL ? 0 : std::strlen(value);
	const uint32 wireLength = static_cast<uint32>(length);
	appendClientDigestValue(digest, wireLength);
	if (length > 0)
		digest.append(value, length);
}

static void appendClientDigestDataTypeRef(KBE_MD5& digest, DataType* pDataType)
{
	const DATATYPE_UID dataTypeUType = pDataType->id();
	appendClientDigestValue(digest, dataTypeUType);
	appendClientDigestString(digest, pDataType->getName());
	appendClientDigestString(digest, pDataType->aliasName());
}

static void appendClientDigestDataType(KBE_MD5& digest, DataType* pDataType)
{
	appendClientDigestDataTypeRef(digest, pDataType);
	const DATATYPE dataType = pDataType->type();
	const int32 wireDataType = static_cast<int32>(dataType);
	appendClientDigestValue(digest, wireDataType);

	if (dataType == DATA_TYPE_FIXEDARRAY)
	{
		FixedArrayType* pArrayType = static_cast<FixedArrayType*>(pDataType);
		appendClientDigestDataTypeRef(digest, pArrayType->getDataType());
	}
	else if (dataType == DATA_TYPE_FIXEDDICT)
	{
		FixedDictType* pDictType = static_cast<FixedDictType*>(pDataType);
		FixedDictType::FIXEDDICT_KEYTYPE_MAP& keyTypes = pDictType->getKeyTypes();
		const uint32 keyCount = static_cast<uint32>(keyTypes.size());
		appendClientDigestValue(digest, keyCount);
		for (FixedDictType::FIXEDDICT_KEYTYPE_MAP::const_iterator iter = keyTypes.begin();
			iter != keyTypes.end(); ++iter)
		{
			appendClientDigestString(digest, iter->first.c_str());
			appendClientDigestDataTypeRef(digest, iter->second->dataType);
		}
	}
	else if (dataType == DATA_TYPE_ENTITY_COMPONENT)
	{
		EntityComponentType* pComponentType = static_cast<EntityComponentType*>(pDataType);
		ScriptDefModule* pComponentModule = pComponentType->pScriptDefModule();
		appendClientDigestString(digest, pComponentModule->getName());
		const ENTITY_SCRIPT_UID moduleUType = pComponentModule->getUType();
		appendClientDigestValue(digest, moduleUType);
	}
}

static void appendClientDigestProperty(KBE_MD5& digest, const PropertyDescription* pProperty)
{
	appendClientDigestString(digest, pProperty->getName());
	appendClientDigestString(digest, pProperty->getDefaultValStr());
	appendClientDigestDataTypeRef(digest, pProperty->getDataType());
	const ENTITY_PROPERTY_UID propertyUType = pProperty->getUType();
	const uint32 flags = pProperty->getFlags();
	const int16 aliasID = pProperty->aliasID();
	appendClientDigestValue(digest, propertyUType);
	appendClientDigestValue(digest, flags);
	appendClientDigestValue(digest, aliasID);
}

static void appendClientDigestMethod(KBE_MD5& digest, COMPONENT_TYPE domain,
	const MethodDescription* pMethod)
{
	appendClientDigestValue(digest, domain);
	appendClientDigestString(digest, pMethod->getName());
	const ENTITY_METHOD_UID methodUType = pMethod->getUType();
	const MethodDescription::EXPOSED_TYPE exposedType = pMethod->isExposed();
	const int16 aliasID = pMethod->aliasID();
	appendClientDigestValue(digest, methodUType);
	appendClientDigestValue(digest, exposedType);
	appendClientDigestValue(digest, aliasID);

	std::vector<DataType*>& argTypes = const_cast<MethodDescription*>(pMethod)->getArgTypes();
	const uint32 argCount = static_cast<uint32>(argTypes.size());
	appendClientDigestValue(digest, argCount);
	for (std::vector<DataType*>::const_iterator iter = argTypes.begin(); iter != argTypes.end(); ++iter)
		appendClientDigestDataTypeRef(digest, *iter);
}

void EntityDef::buildClientDigest()
{
	__clientMd5.clear();
	static const uint32 digestFormatVersion = 1;
	appendClientDigestValue(__clientMd5, digestFormatVersion);

	// SDK 会导出所有非内部类型，因此类型表也是客户端协议的一部分。
	// SDKs export every non-internal type, so the exported type table is part of the client protocol.
	const DataTypes::UID_DATATYPE_MAP& dataTypes = DataTypes::uid_dataTypes();
	for (DataTypes::UID_DATATYPE_MAP::const_iterator iter = dataTypes.begin();
		iter != dataTypes.end(); ++iter)
	{
		DataType* pDataType = iter->second;
		if (pDataType->aliasName()[0] == '_')
			continue;

		appendClientDigestDataType(__clientMd5, pDataType);
	}

	for (SCRIPT_MODULES::const_iterator moduleIter = __scriptModules.begin();
		moduleIter != __scriptModules.end(); ++moduleIter)
	{
		ScriptDefModule* pModule = moduleIter->get();
		if (!pModule->hasClient())
			continue;

		appendClientDigestString(__clientMd5, pModule->getName());
		const ENTITY_SCRIPT_UID moduleUType = pModule->getUType();
		appendClientDigestValue(__clientMd5, moduleUType);
	const uint8 componentModule = pModule->isComponentModule() ? 1 : 0;
		appendClientDigestValue(__clientMd5, componentModule);

		ScriptDefModule::PROPERTYDESCRIPTION_MAP& properties = pModule->getClientPropertyDescriptions();
		const uint32 propertyCount = static_cast<uint32>(properties.size());
		appendClientDigestValue(__clientMd5, propertyCount);
		for (ScriptDefModule::PROPERTYDESCRIPTION_MAP::const_iterator iter = properties.begin();
			iter != properties.end(); ++iter)
		{
			appendClientDigestProperty(__clientMd5, iter->second);
		}

		const COMPONENT_TYPE domains[] = { CLIENT_TYPE, BASEAPP_TYPE, CELLAPP_TYPE };
		ScriptDefModule::METHODDESCRIPTION_MAP* methodMaps[] = {
			&pModule->getClientMethodDescriptions(),
			&pModule->getBaseExposedMethodDescriptions(),
			&pModule->getCellExposedMethodDescriptions()
		};
		for (size_t domainIndex = 0; domainIndex < 3; ++domainIndex)
		{
			const uint32 methodCount = static_cast<uint32>(methodMaps[domainIndex]->size());
			appendClientDigestValue(__clientMd5, methodCount);
			for (ScriptDefModule::METHODDESCRIPTION_MAP::const_iterator iter = methodMaps[domainIndex]->begin();
				iter != methodMaps[domainIndex]->end(); ++iter)
			{
				appendClientDigestMethod(__clientMd5, domains[domainIndex], iter->second);
			}
		}
	}

	__clientMd5.final();
}

// Property UIDs are allocated through one path so component fields and ordinary fields share the same collision rules.
// 属性 UID 通过同一路径分配，使组件字段与普通字段遵守相同的冲突规则。
static bool reservePropertyUType(const std::string& moduleName,
	const std::string& propertyName,
	int requestedUType,
	ScriptDefModule* pScriptModule,
	ENTITY_PROPERTY_UID& outUType)
{
	if (requestedUType > 0)
	{
		ENTITY_PROPERTY_UID utype = static_cast<ENTITY_PROPERTY_UID>(requestedUType);
		if (requestedUType != static_cast<int>(utype))
		{
			ERROR_MSG(fmt::format("EntityDef::reservePropertyUType: Utype {} overflow in {}.{}.\n",
				requestedUType, moduleName, propertyName));
			return false;
		}

		if (pScriptModule->findPropertyDescription(utype, BASEAPP_TYPE) != NULL ||
			pScriptModule->findPropertyDescription(utype, CELLAPP_TYPE) != NULL ||
			pScriptModule->findPropertyDescription(utype, CLIENT_TYPE) != NULL)
		{
			ERROR_MSG(fmt::format("EntityDef::reservePropertyUType: Utype {} conflicts in {}.{}.\n",
				requestedUType, moduleName, propertyName));
			return false;
		}

		g_propertyUtypes.push_back(utype);
		outUType = utype;
		return true;
	}

	while (true)
	{
		ENTITY_PROPERTY_UID utype = g_propertyUtypeAuto++;
		if (std::find(g_propertyUtypes.begin(), g_propertyUtypes.end(), utype) == g_propertyUtypes.end())
		{
			g_propertyUtypes.push_back(utype);
			outUType = utype;
			return true;
		}
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

// Entity lookup is centralized in EntityCall so component ownership follows the existing 1.x hook.
// 实体查找统一委托给 EntityCall，组件所有权因此复用现有 1.x 钩子。
PyObject* EntityDef::tryGetEntity(COMPONENT_ID componentID, ENTITY_ID entityID)
{
	return EntityCall::tryGetEntity(componentID, entityID);
}

//-------------------------------------------------------------------------------------
bool EntityDef::finalise(bool isReload)
{
	PropertyDescription::resetDescriptionCount();
	MethodDescription::resetDescriptionCount();

	EntityDef::__md5.clear();
	EntityDef::__clientMd5.clear();
	g_methodUTypeAllocators.clear();
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

	EntityDef::__scriptModules.clear();
	EntityDef::__scriptTypeMappingUType.clear();
	// EntityDef 重建协议表时同步清空插件发现缓存，热重载才能重新读取 plugins.xml 和 manifest。
	// Clear plugin discovery state together with the EntityDef protocol table so reloads reread plugins.xml and manifests.
	PluginManager::instance().finalise();
	DataType::finalise();
	DataTypes::finalise();
	return true;
}

//-------------------------------------------------------------------------------------
bool EntityDef::isReload()
{
	return g_isReload;
}

//-------------------------------------------------------------------------------------
ReloadScriptDefStats EntityDef::reload(bool fullReload)
{
	g_isReload = true;
	g_reloadStats = ReloadScriptDefStats();
	reloadChangedScriptModules();

	if (!g_reloadStats.ok)
	{
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
		if (!ret)
		{
			ERROR_MSG("EntityDef::reload: finalise failed.\n");
			g_reloadStats.ok = false;
			g_isReload = false;
			return g_reloadStats;
		}

		// 变更模块已在上面完成 reload；定义重建阶段只重新绑定类型，避免模块顶层代码执行两次。
		// Changed modules were already reloaded above; definition rebuilding only rebinds types to avoid running module code twice.
		g_isReload = false;
		ret = initialize(EntityDef::__scriptBaseTypes, EntityDef::__loadComponentType);
		if (!ret)
		{
			ERROR_MSG("EntityDef::reload: initialize failed.\n");
			g_reloadStats.ok = false;
			return g_reloadStats;
		}
	}
	else if (g_reloadStats.changedFiles > 0)
	{
		// 增量路径只重新绑定已成功 reload 的类；协议描述与在线数据均保持不变。
		// The incremental path only rebinds successfully reloaded classes; protocol descriptions and live data remain intact.
		g_isReload = false;
		if (!loadAllScriptModules(EntityDef::__entitiesPath, EntityDef::__scriptBaseTypes))
			g_reloadStats.ok = false;
	}

	EntityDef::_isInit = true;
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

	g_entityFlagMapping["CELL_PUBLIC"]							= ED_FLAG_CELL_PUBLIC;
	g_entityFlagMapping["CELL_PRIVATE"]							= ED_FLAG_CELL_PRIVATE;
	g_entityFlagMapping["ALL_CLIENTS"]							= ED_FLAG_ALL_CLIENTS;
	g_entityFlagMapping["CELL_PUBLIC_AND_OWN"]					= ED_FLAG_CELL_PUBLIC_AND_OWN;
	g_entityFlagMapping["BASE_AND_CLIENT"]						= ED_FLAG_BASE_AND_CLIENT;
	g_entityFlagMapping["BASE"]									= ED_FLAG_BASE;
	g_entityFlagMapping["OTHER_CLIENTS"]						= ED_FLAG_OTHER_CLIENTS;
	g_entityFlagMapping["OWN_CLIENT"]							= ED_FLAG_OWN_CLIENT;

	std::string entitiesFile = __entitiesPath + "entities.xml";
	std::string defFilePath = __entitiesPath + "entity_defs/";
	
	// 插件类型按 plugins.xml 顺序先于宿主 assets 类型加载，确保宿主 .def 可以引用插件别名。
	// Plugin types load in plugins.xml order before host asset types so host .def files can reference plugin aliases.
	std::vector<PluginTypeFileDescriptor> pluginTypeFiles;
	if (!PluginManager::instance().initialize())
		return false;

	pluginTypeFiles = PluginManager::instance().getTypeFileDescriptors();
	bool dataTypesInitialized = false;
	for (std::vector<PluginTypeFileDescriptor>::const_iterator iter = pluginTypeFiles.begin();
		iter != pluginTypeFiles.end(); ++iter)
	{
		if (!dataTypesInitialized)
		{
			if (!DataTypes::initialize(iter->file, iter->pluginPrefix, iter->manifestFile))
				return false;
			dataTypesInitialized = true;
		}
		else if (!DataTypes::loadTypes(iter->file, iter->pluginPrefix, iter->manifestFile))
		{
			return false;
		}
	}

	// 初始化宿主 assets 类型；没有插件类型时保持原有 1.x 初始化路径。
	// Initialize host asset types; when no plugin types exist this preserves the original 1.x path.
	if (!dataTypesInitialized && !DataTypes::initialize(defFilePath + "types.xml"))
		return false;
	if (dataTypesInitialized && !DataTypes::loadTypes(defFilePath + "types.xml", "", defFilePath + "types.xml"))
		return false;

	// 打开这个entities.xml文件
	SmartPointer<XML> xml(new XML());
	if(!xml->openSection(entitiesFile.c_str()))
		return false;

	// 插件和宿主实体共用同一套 utype、定义解析和 MD5 累积流程；插件先加载以固定协议顺序。
	// Plugins and host entities share one utype, definition parser, and MD5 accumulation flow; plugins load first for a stable protocol order.
	auto loadEntityDefinition = [&](const std::string& moduleName, const std::string& entityDefPath,
		const std::string& entityDefDirectory) -> bool
	{
		if (__scriptTypeMappingUType.find(moduleName) != __scriptTypeMappingUType.end())
		{
			ERROR_MSG(fmt::format("EntityDef::initialize: duplicate entity module [{}], definition [{}].\n",
				moduleName, entityDefPath));
			return false;
		}

		// 实体和解析期间插入的组件共享模块表，因此编号必须从模块表当前位置分配；独立计数器会在组件插入后复用编号。
		// Entities and components inserted during parsing share one module table, so IDs must follow its current size; a separate counter reuses IDs after component insertion.
		ENTITY_SCRIPT_UID moduleUType = static_cast<ENTITY_SCRIPT_UID>(__scriptModules.size() + 1);
		__scriptTypeMappingUType[moduleName] = moduleUType;
		ScriptDefModule* pScriptModule = new ScriptDefModule(moduleName, moduleUType);
		EntityDef::__scriptModules.push_back(pScriptModule);
		pScriptModule->setDefSourceFile(entityDefPath);

		SmartPointer<XML> defxml(new XML());
		if(!defxml->openSection(entityDefPath.c_str()))
			return false;

		TiXmlNode* defNode = defxml->getRootNode();
		if(defNode == NULL)
			return true;

		if(!loadDefInfo(entityDefDirectory, moduleName, defxml.get(), defNode, pScriptModule))
		{
			ERROR_MSG(fmt::format("EntityDef::initialize: failed to load entity({}) module from [{}].\n",
				moduleName, entityDefPath));
			return false;
		}

		if(!loadDetailLevelInfo(entityDefDirectory, moduleName, defxml.get(), defNode, pScriptModule))
		{
			ERROR_MSG(fmt::format("EntityDef::initialize: failed to load entity({}) DetailLevelInfo from [{}].\n",
				moduleName, entityDefPath));
			return false;
		}

		if(!pScriptModule->getDetailLevel().configured())
		{
			const PropertyDescription* pRestrictedProperty = NULL;
			for(DETAIL_TYPE detailLevel = DETAIL_LEVEL_NEAR;
				detailLevel < DETAIL_LEVEL_FAR && pRestrictedProperty == NULL; ++detailLevel)
			{
				ScriptDefModule::PROPERTYDESCRIPTION_MAP& detailProperties =
					pScriptModule->getCellPropertyDescriptionsByDetailLevel(detailLevel);
				for(ScriptDefModule::PROPERTYDESCRIPTION_MAP::const_iterator propertyIter = detailProperties.begin();
					propertyIter != detailProperties.end(); ++propertyIter)
				{
					if((propertyIter->second->getFlags() & ENTITY_BROADCAST_OTHER_CLIENT_FLAGS) > 0)
					{
						pRestrictedProperty = propertyIter->second;
						break;
					}
				}
			}

			if(pRestrictedProperty != NULL)
			{
				WARNING_MSG(fmt::format("EntityDef::initialize: entity({}) property({}) uses DetailLevel but has no DetailLevels; all ranges remain unlimited.\n",
					moduleName, pRestrictedProperty->getName()));
			}
		}

		pScriptModule->onLoaded();
		return true;
	};

	// 插件实体定义使用插件自己的 entity_defs 目录解析 interfaces/components 相对路径。
	// Plugin entity definitions resolve interfaces/components relative to the plugin's own entity_defs directory.
	const std::vector<PluginEntityDescriptor>& pluginEntities = PluginManager::instance().entities();
	for (std::vector<PluginEntityDescriptor>::const_iterator pluginIter = pluginEntities.begin();
		pluginIter != pluginEntities.end(); ++pluginIter)
	{
		std::string pluginEntityDirectory = pluginIter->pluginRootPath + "/entity_defs/";
		if (!loadEntityDefinition(pluginIter->name, pluginIter->defFullPath, pluginEntityDirectory))
			return false;
	}

	// 获得entities.xml根节点, 如果没有定义一个entity那么直接返回true
	TiXmlNode* node = xml->getRootNode();
	if(node == NULL)
		return true;

	// 开始遍历所有的entity节点
	XML_FOR_BEGIN(node)
	{
		std::string moduleName = xml.get()->getKey(node);
		std::string deffile = defFilePath + moduleName + ".def";
		if (!loadEntityDefinition(moduleName, deffile, defFilePath))
			return false;
	}
	XML_FOR_END(node);

	EntityDef::buildClientDigest();
	EntityDef::md5().final();

	if(loadComponentType == DBMGR_TYPE)
		return true;

	bool initialized = loadAllScriptModules(__entitiesPath, scriptBaseTypes) && initializeWatcher();
	if (initialized)
		rememberLoadedScriptStamps();
	return initialized;
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

	if(!loadComponents(defFilePath, moduleName, defxml, defNode, pScriptModule))
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
	(void)defFilePath;
	TiXmlNode* detailLevelNode = defxml->enterNode(defNode, "DetailLevels");
	if(detailLevelNode == NULL)
		return true;

	DetailLevel::Level configuredLevels[DETAIL_LEVEL_COUNT];
	const char* levelNames[DETAIL_LEVEL_COUNT] = { "NEAR", "MEDIUM", "FAR" };
	for(DETAIL_TYPE detailLevel = DETAIL_LEVEL_NEAR; detailLevel < DETAIL_LEVEL_COUNT; ++detailLevel)
	{
		TiXmlNode* node = defxml->enterNode(detailLevelNode, levelNames[detailLevel]);
		if(node == NULL)
		{
			ERROR_MSG(fmt::format("EntityDef::loadDetailLevelInfo: entity:{} is missing {} in DetailLevels.\n",
				moduleName, levelNames[detailLevel]));
			return false;
		}

		TiXmlNode* radiusNode = defxml->enterNode(node, "radius");
		TiXmlNode* hystNode = defxml->enterNode(node, "hyst");
		if(radiusNode == NULL || hystNode == NULL)
		{
			ERROR_MSG(fmt::format("EntityDef::loadDetailLevelInfo: entity:{} {} requires radius and hyst.\n",
				moduleName, levelNames[detailLevel]));
			return false;
		}

		configuredLevels[detailLevel].radius = static_cast<float>(defxml->getValFloat(radiusNode));
		configuredLevels[detailLevel].hyst = static_cast<float>(defxml->getValFloat(hystNode));
		if(!std::isfinite(configuredLevels[detailLevel].radius) ||
			!std::isfinite(configuredLevels[detailLevel].hyst) ||
			configuredLevels[detailLevel].radius < 0.f || configuredLevels[detailLevel].hyst < 0.f)
		{
			ERROR_MSG(fmt::format("EntityDef::loadDetailLevelInfo: entity:{} {} radius and hyst must be finite non-negative values.\n",
				moduleName, levelNames[detailLevel]));
			return false;
		}
	}

	// radius 使用绝对距离，hyst 只负责向外离开当前等级的滞回。
	// Radius values are absolute distances; hyst is used only when leaving the current level.
	if(configuredLevels[DETAIL_LEVEL_NEAR].radius > configuredLevels[DETAIL_LEVEL_MEDIUM].radius ||
		configuredLevels[DETAIL_LEVEL_MEDIUM].radius > configuredLevels[DETAIL_LEVEL_FAR].radius)
	{
		ERROR_MSG(fmt::format("EntityDef::loadDetailLevelInfo: entity:{} DetailLevels radius must satisfy NEAR <= MEDIUM <= FAR.\n",
			moduleName));
		return false;
	}

	DetailLevel& dlInfo = pScriptModule->getDetailLevel();
	for(DETAIL_TYPE detailLevel = DETAIL_LEVEL_NEAR; detailLevel < DETAIL_LEVEL_COUNT; ++detailLevel)
		dlInfo.level[detailLevel] = configuredLevels[detailLevel];
	dlInfo.configured(true);

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
							   ScriptDefModule* pScriptModule)
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

// 读取实体定义中的组件声明，并把组件定义展开为宿主实体的组件描述索引。
// Load component declarations and index each component definition on its host entity.
bool EntityDef::loadComponents(const std::string& defFilePath,
	const std::string& moduleName,
	XML* defxml,
	TiXmlNode* defNode,
	ScriptDefModule* pScriptModule)
{
	TiXmlNode* componentsNode = defxml->enterNode(defNode, "Components");
	if (componentsNode == NULL)
		return true;

	XML_FOR_BEGIN(componentsNode)
	{
		std::string componentName = defxml->getKey(componentsNode);
		if (componentName.empty() || !validDefPropertyName(componentName))
		{
			ERROR_MSG(fmt::format("EntityDef::loadComponents: invalid component name '{}' in module {}.\n",
				componentName, moduleName));
			return false;
		}

		TiXmlNode* componentNode = defxml->enterNode(componentsNode, componentName.c_str());
		if (componentNode == NULL)
			continue;

		TiXmlNode* typeNode = defxml->enterNode(componentNode, "Type");
		if (typeNode == NULL)
			typeNode = defxml->enterNode(componentNode, "type");

		std::string componentTypeName = typeNode ? defxml->getKey(typeNode) : std::string();
		if (componentTypeName.empty())
		{
			ERROR_MSG(fmt::format("EntityDef::loadComponents: component '{}' has no Type in module {}.\n",
				componentName, moduleName));
			return false;
		}

		ENTITY_PROPERTY_UID componentPropertyUType = 0;
		TiXmlNode* utypeNode = defxml->enterNode(componentNode, "Utype");
		int requestedUType = utypeNode ? defxml->getValInt(utypeNode) : -1;
		if (!reservePropertyUType(moduleName, componentName, requestedUType,
			pScriptModule, componentPropertyUType))
		{
			return false;
		}

		bool isPersistent = true;
		TiXmlNode* persistentNode = defxml->enterNode(componentNode, "Persistent");
		if (persistentNode != NULL)
		{
			std::string persistentValue = defxml->getValStr(persistentNode);
			std::transform(persistentValue.begin(), persistentValue.end(),
				persistentValue.begin(), tolower);
			isPersistent = persistentValue != "false";
		}

		ScriptDefModule* componentModule = NULL;
		SCRIPT_MODULE_UID_MAP::iterator moduleIter = __scriptTypeMappingUType.find(componentTypeName);
		if (moduleIter != __scriptTypeMappingUType.end())
			componentModule = findScriptModule(moduleIter->second);

		if (componentModule == NULL)
		{
			ENTITY_SCRIPT_UID componentUType = static_cast<ENTITY_SCRIPT_UID>(__scriptModules.size() + 1);
			componentModule = new ScriptDefModule(componentTypeName, componentUType);
			componentModule->isComponentModule(true);
			__scriptTypeMappingUType[componentTypeName] = componentUType;
			__scriptModules.push_back(componentModule);

			std::string componentFile = defFilePath + "components/" + componentTypeName + ".def";
			componentModule->setDefSourceFile(componentFile);
			SmartPointer<XML> componentXml(new XML());
			if (!componentXml->openSection(componentFile.c_str()))
			{
				ERROR_MSG(fmt::format("EntityDef::loadComponents: cannot open component definition {}.\n",
					componentFile));
				return false;
			}

			TiXmlNode* componentRootNode = componentXml->getRootNode();
			if (componentRootNode != NULL &&
				(!loadAllDefDescriptions(componentTypeName, componentXml.get(), componentRootNode, componentModule) ||
				 !loadInterfaces(defFilePath + "components/", componentTypeName, componentXml.get(), componentRootNode, componentModule) ||
				 !loadParentClass(defFilePath + "components/", componentTypeName, componentXml.get(), componentRootNode, componentModule) ||
				 !loadDetailLevelInfo(defFilePath + "components/", componentTypeName, componentXml.get(), componentRootNode, componentModule)))
			{
				ERROR_MSG(fmt::format("EntityDef::loadComponents: failed to load component definition {}.\n",
					componentTypeName));
				return false;
			}

			componentModule->autoMatchCompOwn();
			componentModule->onLoaded();
		}

		// Component flags are derived from the loaded component domains, matching the 2.8 wire visibility rules.
		// 组件标志从已加载的组件域推导，与 2.8 线协议的可见性规则保持一致。
		uint32 flags = ED_FLAG_UNKOWN;
		if (componentModule->hasBase())
			flags |= ED_FLAG_BASE;
		if (componentModule->hasCell())
			flags |= ED_FLAG_CELL_PUBLIC;
		if (componentModule->hasClient())
		{
			if (componentModule->hasBase())
				flags |= ED_FLAG_BASE_AND_CLIENT;
			if (componentModule->hasCell())
				flags |= ED_FLAG_ALL_CLIENTS | ED_FLAG_CELL_PUBLIC_AND_OWN |
					ED_FLAG_OTHER_CLIENTS | ED_FLAG_OWN_CLIENT;
		}

		if (flags == ED_FLAG_UNKOWN)
		{
			ERROR_MSG(fmt::format("EntityDef::loadComponents: component {} has no runtime domain.\n",
				componentTypeName));
			return false;
		}

		if (pScriptModule->hasName(componentName))
		{
			ERROR_MSG(fmt::format("EntityDef::loadComponents: component name {} conflicts in module {}.\n",
				componentName, moduleName));
			return false;
		}

		DataType* componentDataType = new EntityComponentType(componentModule);
		std::string componentDataTypeName = "EntityComponent";
		std::string defaultValue;
		PropertyDescription* componentProperty = PropertyDescription::createDescription(
			componentPropertyUType, componentDataTypeName, componentName, flags,
			isPersistent, componentDataType, false, std::string(), 0,
			defaultValue, DETAIL_LEVEL_FAR);

		bool propertyRegistered = true;
		if ((flags & ENTITY_CELL_DATA_FLAGS) > 0)
			propertyRegistered = pScriptModule->addPropertyDescription(
				componentName.c_str(), componentProperty, CELLAPP_TYPE) && propertyRegistered;
		if ((flags & ENTITY_BASE_DATA_FLAGS) > 0)
			propertyRegistered = pScriptModule->addPropertyDescription(
				componentName.c_str(), componentProperty, BASEAPP_TYPE) && propertyRegistered;
		if ((flags & ENTITY_CLIENT_DATA_FLAGS) > 0)
			propertyRegistered = pScriptModule->addPropertyDescription(
				componentName.c_str(), componentProperty, CLIENT_TYPE) && propertyRegistered;

		if (!propertyRegistered)
		{
			ERROR_MSG(fmt::format("EntityDef::loadComponents: failed to register component property {}.{}.\n",
				moduleName, componentName));
			return false;
		}

		if (!pScriptModule->addComponentDescription(componentName.c_str(), componentModule))
		{
			ERROR_MSG(fmt::format("EntityDef::loadComponents: failed to register {}.{} component.\n",
				moduleName, componentName));
			return false;
		}
	}
	XML_FOR_END(componentsNode);

	return true;
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

	while (true)
	{
		std::string limited = ENTITY_LIMITED_PROPERTYS[i];

		if (limited == "")
			break;

		if (name == limited)
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
			std::string					descriptionStr;
			std::string					name = "";

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

			// Description只描述持久化字段，不改变属性类型、Utype或客户端协议摘要。
			// Description documents the persistent field without changing its type, Utype, or client protocol digest.
			TiXmlNode* descriptionNode =
				xml->enterNode(defPropertyNode->FirstChild(), "Description");

			if(descriptionNode)
			{
				descriptionStr = xml->getValStr(descriptionNode);
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
			
			TiXmlNode* utypeValNode = 
				xml->enterNode(defPropertyNode->FirstChild(), "Utype");

			if(utypeValNode)
			{
				int iUtype = xml->getValInt(utypeValNode);
				futype = iUtype;

				if (iUtype != int(futype))
				{
					ERROR_MSG(fmt::format("EntityDef::loadDefPropertys: 'Utype' has overflowed({} > 65535), is {}.{}!\n",
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
						ERROR_MSG(fmt::format("EntityDef::loadDefPropertys: {}.{}, 'Utype' {} Conflict({}.{} 'Utype' {})!\n",
							moduleName, name.c_str(), iUtype, moduleName, pConflictPropertyDescription->getName(), iUtype));

						foundConflict = true;
					}

					pConflictPropertyDescription = pScriptModule->findPropertyDescription(futype, CELLAPP_TYPE);
					if (pConflictPropertyDescription)
					{
						ERROR_MSG(fmt::format("EntityDef::loadDefPropertys: {}.{}, 'Utype' {} Conflict({}.{} 'Utype' {})!\n",
							moduleName, name.c_str(), iUtype, moduleName, pConflictPropertyDescription->getName(), iUtype));

						foundConflict = true;
					}

					pConflictPropertyDescription = pScriptModule->findPropertyDescription(futype, CLIENT_TYPE);
					if (pConflictPropertyDescription)
					{
						ERROR_MSG(fmt::format("EntityDef::loadDefPropertys: {}.{}, 'Utype' {} Conflict({}.{} 'Utype' {})!\n",
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
				while(true)
				{
					futype = g_propertyUtypeAuto++;
					std::vector<ENTITY_PROPERTY_UID>::iterator iter = 
						std::find(g_propertyUtypes.begin(), g_propertyUtypes.end(), futype);

					if (iter == g_propertyUtypes.end())
						break;
				}

				g_propertyUtypes.push_back(futype);
			}

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
					}
				}
				XML_FOR_END(argNode);		
			}

			if (!assignMethodUType(moduleName, methodDescription, pScriptModule))
				return false;

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
					}
				}
				XML_FOR_END(argNode);		
			}

			if (!assignMethodUType(moduleName, methodDescription, pScriptModule))
				return false;

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
					}
				}
				XML_FOR_END(argNode);		
			}

			if (!assignMethodUType(moduleName, methodDescription, pScriptModule))
				return false;

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
bool EntityDef::loadAllScriptModules(std::string entitiesPath,
									std::vector<PyTypeObject*>& scriptBaseTypes)
{
	std::string entitiesFile = entitiesPath + "entities.xml";

	SmartPointer<XML> xml(new XML());
	if(!xml->openSection(entitiesFile.c_str()))
		return false;

	// 插件实体和宿主实体共享同一套 Python 类型校验，插件先处理以匹配实体定义的 utype 顺序。
	// Plugin and host entities share the same Python type validation, with plugins processed first to match entity-definition utype order.
	auto loadScriptModule = [&](const std::string& moduleName) -> bool
	{
		ScriptDefModule* pScriptModule = findScriptModule(moduleName.c_str());
		if (pScriptModule == NULL)
		{
			ERROR_MSG(fmt::format("EntityDef::loadAllScriptModules: module [{}] has no definition.\n", moduleName));
			return false;
		}

		PyObject* pyModule =
			PyImport_ImportModule(const_cast<char*>(moduleName.c_str()));

		if (g_isReload && pyModule)
			pyModule = PyImport_ReloadModule(pyModule);

		// 检查该模块路径是否是KBE脚本目录下的，防止因用户取名与python模块名称冲突而误导入了系统模块
		if (pyModule)
		{
			std::string userScriptsPath = Resmgr::getSingleton().getPyUserScriptsPath();
			std::string pyModulePath = "";
			
			PyObject *fileobj = NULL;

			fileobj = PyModule_GetFilenameObject(pyModule);
			if (fileobj)
				pyModulePath = PyUnicode_AsUTF8(fileobj);

			if (fileobj)
				Py_DECREF(fileobj);

			strutil::kbe_replace(userScriptsPath, "/", "");
			strutil::kbe_replace(userScriptsPath, "\\", "");
			strutil::kbe_replace(pyModulePath, "/", "");
			strutil::kbe_replace(pyModulePath, "\\", "");

			if (pyModulePath.find(userScriptsPath) == std::string::npos)
			{
				WARNING_MSG(fmt::format("EntityDef::initialize: The script module name[{}] and system module name conflict!\n",
					moduleName.c_str()));

				S_RELEASE(pyModule);
				pyModule = NULL;
			}
		}

		if (pyModule == NULL)
		{
			// 是否加载这个模块 （取决于是否在def文件中定义了与当前组件相关的方法或者属性）
			if(isLoadScriptModule(pScriptModule))
			{
				ERROR_MSG(fmt::format("EntityDef::initialize: Could not load module[{}]\n", 
					moduleName.c_str()));

				PyErr_Print();
				return false;
			}

			PyErr_Clear();

			// 必须在这里才设置， 在这之前设置会导致isLoadScriptModule失效，从而没有错误输出
			setScriptModuleHasComponentEntity(pScriptModule, false);
			return true;
		}

		setScriptModuleHasComponentEntity(pScriptModule, true);

		PyObject* pyClass = 
			PyObject_GetAttrString(pyModule, const_cast<char *>(moduleName.c_str()));

		if (pyClass == NULL)
		{
			ERROR_MSG(fmt::format("EntityDef::initialize: Could not find class[{}]\n",
				moduleName.c_str()));

			return false;
		}
		else 
		{
			std::string typeNames = "";
			bool valid = false;
			std::vector<PyTypeObject*>::iterator iter = scriptBaseTypes.begin();
			for(; iter != scriptBaseTypes.end(); ++iter)
			{
				if(!PyObject_IsSubclass(pyClass, (PyObject *)(*iter)))
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
			
			if(!valid)
			{
				ERROR_MSG(fmt::format("EntityDef::initialize: Class {} is not derived from KBEngine.[{}]\n",
					moduleName.c_str(), typeNames.c_str()));

				return false;
			}
		}

		if(!PyType_Check(pyClass))
		{
			ERROR_MSG(fmt::format("EntityDef::initialize: class[{}] is valid!\n",
				moduleName.c_str()));

			return false;
		}
		
		if(!checkDefMethod(pScriptModule, pyClass, moduleName))
		{
			ERROR_MSG(fmt::format("EntityDef::initialize: class[{}] checkDefMethod is failed!\n",
				moduleName.c_str()));

			return false;
		}
		
		DEBUG_MSG(fmt::format("loaded script:{}({}).\n", moduleName.c_str(), 
			pScriptModule->getUType()));

		pScriptModule->setScriptType((PyTypeObject *)pyClass);
		S_RELEASE(pyModule);
		return true;
	};

	// 组件属性初始化会通过 ScriptDefModule::createObject 直接分配对应脚本类，因此必须先绑定所有组件类型。
	// Component-property initialization allocates its script class through ScriptDefModule::createObject, so every component type must be bound first.
	// 直接遍历统一模块表同时覆盖宿主与插件组件，并保持定义阶段确定的模块编号顺序。
	// Iterating the unified module table covers host and plugin components while preserving the module-ID order fixed during definition loading.
	for (SCRIPT_MODULES::const_iterator moduleIter = __scriptModules.begin();
		moduleIter != __scriptModules.end(); ++moduleIter)
	{
		ScriptDefModule* pScriptModule = moduleIter->get();
		if (pScriptModule->isComponentModule() && !loadScriptModule(pScriptModule->getName()))
			return false;
	}

	const std::vector<PluginEntityDescriptor>& pluginEntities = PluginManager::instance().entities();
	for (std::vector<PluginEntityDescriptor>::const_iterator pluginIter = pluginEntities.begin();
		pluginIter != pluginEntities.end(); ++pluginIter)
	{
		if (!loadScriptModule(pluginIter->name))
			return false;
	}

	TiXmlNode* node = xml->getRootNode();
	if(node == NULL)
		return true;

	XML_FOR_BEGIN(node)
	{
		if (!loadScriptModule(xml.get()->getKey(node)))
			return false;
	}
	XML_FOR_END(node);

	return true;
}

//-------------------------------------------------------------------------------------
ScriptDefModule* EntityDef::findScriptModule(ENTITY_SCRIPT_UID utype)
{
	// utype 最小为1
	if (utype == 0 || utype >= __scriptModules.size() + 1)
	{
		ERROR_MSG(fmt::format("EntityDef::findScriptModule: is not exist(utype:{})!\n", utype));
		return NULL;
	}

	return __scriptModules[utype - 1].get();
}

//-------------------------------------------------------------------------------------
ScriptDefModule* EntityDef::findScriptModule(const char* scriptName)
{
	std::map<std::string, ENTITY_SCRIPT_UID>::iterator iter = 
		__scriptTypeMappingUType.find(scriptName);

	if(iter == __scriptTypeMappingUType.end())
	{
		ERROR_MSG(fmt::format("EntityDef::findScriptModule: [{}] not found!\n", scriptName));
		return NULL;
	}

	return findScriptModule(iter->second);
}

//-------------------------------------------------------------------------------------
ScriptDefModule* EntityDef::findOldScriptModule(const char* scriptName)
{
	std::map<std::string, ENTITY_SCRIPT_UID>::iterator iter = 
		__oldScriptTypeMappingUType.find(scriptName);

	if(iter == __oldScriptTypeMappingUType.end())
	{
		ERROR_MSG(fmt::format("EntityDef::findOldScriptModule: [{}] not found!\n", scriptName));
		return NULL;
	}

	if (iter->second >= __oldScriptModules.size() + 1)
	{
		ERROR_MSG(fmt::format("EntityDef::findOldScriptModule: is not exist(utype:{})!\n", iter->second));
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
	FixedArray::installScript(NULL);
	FixedDict::installScript(NULL);
	VolatileInfo::installScript(NULL);

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
		FixedArray::uninstallScript();
		FixedDict::uninstallScript();
		VolatileInfo::uninstallScript();
	}

	return EntityDef::finalise();
}

//-------------------------------------------------------------------------------------
bool EntityDef::initializeWatcher()
{
	return true;
}

//-------------------------------------------------------------------------------------
}
