// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved.

#include "server/plugins/plugin_manager.h"
#include "server/plugins/plugin_manifest.h"

#include "helper/debug_helper.h"
#include "pyscript/scriptstdouterr.h"
#include "resmgr/resmgr.h"
#include "common/stringconv.h"

namespace KBEngine {
namespace {

static std::string normalizePath(std::string path)
{
	std::replace(path.begin(), path.end(), '\\', '/');
	return path;
}

static std::string parentPath(std::string path)
{
	path = normalizePath(path);
	std::string::size_type pos = path.rfind('/');
	if (pos == std::string::npos)
		return "";
	return path.substr(0, pos);
}

static std::string fileName(std::string path)
{
	path = normalizePath(path);
	std::string::size_type pos = path.rfind('/');
	if (pos == std::string::npos)
		return path;
	return path.substr(pos + 1);
}

static bool fileExists(const std::string& path)
{
	return access(path.c_str(), 0) == 0;
}

static std::string componentFolder(COMPONENT_TYPE componentType)
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

static std::string safeModuleName(std::string value)
{
	for (std::string::iterator iter = value.begin(); iter != value.end(); ++iter)
	{
		if (!isalnum((unsigned char)*iter))
			*iter = '_';
	}
	return value;
}

}

PluginManager::PluginManager() :
	initialized_(false)
{
}

PluginManager& PluginManager::instance()
{
	static PluginManager instance;
	return instance;
}

bool PluginManager::initialize()
{
	if (initialized_)
		return true;

	initialized_ = true;
	return discover();
}

void PluginManager::finalise()
{
	for (std::map<COMPONENT_TYPE, std::vector<PyObject*> >::iterator iter = entryModules_.begin(); iter != entryModules_.end(); ++iter)
	{
		for (std::vector<PyObject*>::iterator modIter = iter->second.begin(); modIter != iter->second.end(); ++modIter)
		{
			Py_XDECREF(*modIter);
		}
	}

	entryModules_.clear();
	plugins_.clear();
	entities_.clear();
	entityMap_.clear();
	initialized_ = false;
}

bool PluginManager::discover()
{
	std::string pluginsPath = Resmgr::getSingleton().getPyUserScriptsPath() + "plugins/";
	if (!fileExists(pluginsPath))
		return true;

	wchar_t* wpath = strutil::char2wchar(pluginsPath.c_str());
	std::vector<std::wstring> results;
	bool listed = Resmgr::getSingleton().listPathRes(wpath, L"json", results);
	free(wpath);

	if (!listed)
		return false;

	for (std::vector<std::wstring>::iterator iter = results.begin(); iter != results.end(); ++iter)
	{
		char* cpath = strutil::wchar2char(iter->c_str());
		std::string manifestFile = normalizePath(cpath);
		free(cpath);

		if (fileName(manifestFile) != "plugin.json")
			continue;

		PluginDescriptor descriptor;
		if (!PluginManifest::load(manifestFile, parentPath(manifestFile), descriptor))
			return false;

		if (!descriptor.enabled)
			continue;

		if (!validateAndAdd(descriptor, manifestFile))
			return false;
	}

	if (!plugins_.empty())
	{
		INFO_MSG(fmt::format("PluginManager::discover: loaded {} plugin(s), {} plugin entity(s).\n",
			plugins_.size(), entities_.size()));
	}

	return true;
}

bool PluginManager::validateAndAdd(const PluginDescriptor& descriptor, const std::string& manifestFile)
{
	if (descriptor.name.empty())
	{
		ERROR_MSG(fmt::format("PluginManager::validateAndAdd: plugin name is empty [{}]\n", manifestFile));
		return false;
	}

	for (std::vector<PluginDescriptor>::const_iterator iter = plugins_.begin(); iter != plugins_.end(); ++iter)
	{
		if (iter->name == descriptor.name)
		{
			ERROR_MSG(fmt::format("PluginManager::validateAndAdd: duplicate plugin [{}]\n", descriptor.name));
			return false;
		}
	}

	for (std::vector<PluginEntityDescriptor>::const_iterator iter = descriptor.entities.begin(); iter != descriptor.entities.end(); ++iter)
	{
		if (iter->name.empty())
		{
			ERROR_MSG(fmt::format("PluginManager::validateAndAdd: empty entity name in plugin [{}]\n", descriptor.name));
			return false;
		}

		if (entityMap_.find(iter->name) != entityMap_.end())
		{
			ERROR_MSG(fmt::format("PluginManager::validateAndAdd: duplicate plugin entity [{}]\n", iter->name));
			return false;
		}

		if (!fileExists(iter->defFullPath))
		{
			ERROR_MSG(fmt::format("PluginManager::validateAndAdd: entity [{}] def not found [{}]\n",
				iter->name, iter->defFullPath));
			return false;
		}

		std::string baseFile = normalizePath(descriptor.rootPath + "/base/" + iter->name + ".py");
		std::string cellFile = normalizePath(descriptor.rootPath + "/cell/" + iter->name + ".py");
		std::string clientFile = normalizePath(descriptor.rootPath + "/client/" + iter->name + ".py");

		if (iter->hasBase && !fileExists(baseFile) && !fileExists(baseFile + "c"))
		{
			ERROR_MSG(fmt::format("PluginManager::validateAndAdd: plugin entity [{}] declared base but script not found.\n", iter->name));
			return false;
		}
		if (iter->hasCell && !fileExists(cellFile) && !fileExists(cellFile + "c"))
		{
			ERROR_MSG(fmt::format("PluginManager::validateAndAdd: plugin entity [{}] declared cell but script not found.\n", iter->name));
			return false;
		}
		if (iter->hasClient && !fileExists(clientFile) && !fileExists(clientFile + "c"))
		{
			ERROR_MSG(fmt::format("PluginManager::validateAndAdd: plugin entity [{}] declared client but script not found.\n", iter->name));
			return false;
		}

		entityMap_[iter->name] = *iter;
		entities_.push_back(*iter);
	}

	plugins_.push_back(descriptor);
	return true;
}

const PluginEntityDescriptor* PluginManager::findEntity(const std::string& name) const
{
	std::map<std::string, PluginEntityDescriptor>::const_iterator iter = entityMap_.find(name);
	return iter == entityMap_.end() ? NULL : &iter->second;
}

std::vector<std::string> PluginManager::getTypeFiles() const
{
	std::vector<std::string> files;

	for (std::vector<PluginDescriptor>::const_iterator iter = plugins_.begin(); iter != plugins_.end(); ++iter)
	{
		std::string file = normalizePath(iter->rootPath + "/entity_defs/types.xml");
		if (fileExists(file))
			files.push_back(file);
	}

	return files;
}

std::vector<std::string> PluginManager::getComponentPythonPaths(COMPONENT_TYPE componentType) const
{
	std::vector<std::string> paths;

	for (std::vector<PluginDescriptor>::const_iterator iter = plugins_.begin(); iter != plugins_.end(); ++iter)
	{
		std::map<COMPONENT_TYPE, PluginComponentDescriptor>::const_iterator componentIter = iter->components.find(componentType);
		if (componentIter == iter->components.end())
			continue;

		paths.insert(paths.end(), componentIter->second.scriptPaths.begin(), componentIter->second.scriptPaths.end());
	}

	return paths;
}

bool PluginManager::hasComponentScript(const std::string& entityName, COMPONENT_TYPE componentType) const
{
	const PluginEntityDescriptor* entity = findEntity(entityName);
	if (!entity)
		return false;

	std::string folder;
	if (componentType == BASEAPP_TYPE)
		folder = "base";
	else if (componentType == CELLAPP_TYPE)
		folder = "cell";
	else if (componentType == CLIENT_TYPE)
		folder = "client";
	else if (componentType == BOTS_TYPE)
		folder = "bots";
	else
		return false;

	for (std::vector<PluginDescriptor>::const_iterator iter = plugins_.begin(); iter != plugins_.end(); ++iter)
	{
		for (std::vector<PluginEntityDescriptor>::const_iterator entityIter = iter->entities.begin(); entityIter != iter->entities.end(); ++entityIter)
		{
			if (entityIter->name == entityName)
			{
				std::string file = normalizePath(iter->rootPath + "/" + folder + "/" + entityName + ".py");
				std::string pycFile = file + "c";
				if (fileExists(file) || fileExists(pycFile))
					return true;

				return false;
			}
		}
	}

	return false;
}

bool PluginManager::getComponentAssertion(const std::string& entityName, bool& hasBase, bool& hasCell, bool& hasClient) const
{
	const PluginEntityDescriptor* entity = findEntity(entityName);
	if (!entity)
		return false;

	hasBase = entity->hasBase;
	hasCell = entity->hasCell;
	hasClient = entity->hasClient;
	return true;
}

bool PluginManager::importComponentEntries(COMPONENT_TYPE componentType)
{
	if (entryModules_.find(componentType) != entryModules_.end())
		return true;

	std::vector<PyObject*>& modules = entryModules_[componentType];
	PyObject* importlibUtil = NULL;
	for (std::vector<PluginDescriptor>::const_iterator iter = plugins_.begin(); iter != plugins_.end(); ++iter)
	{
		std::map<COMPONENT_TYPE, PluginComponentDescriptor>::const_iterator componentIter = iter->components.find(componentType);
		if (componentIter == iter->components.end() || componentIter->second.entry.empty())
			continue;

		std::string entryPath;
		std::string entry = componentIter->second.entry;
		if (entry.find('/') != std::string::npos || entry.find('\\') != std::string::npos)
		{
			entryPath = normalizePath(iter->rootPath + "/" + entry);
		}
		else
		{
			std::string folder = componentFolder(componentType);
			entryPath = normalizePath(iter->rootPath + "/" + folder + "/" + entry + ".py");
		}

		if (!fileExists(entryPath))
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

		std::string moduleName = "_kbe_plugin_" + safeModuleName(iter->name) + "_" +
			safeModuleName(COMPONENT_NAME_EX(componentType)) + "_" + safeModuleName(entry);

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
			ERROR_MSG(fmt::format("PluginManager::importComponentEntries: could not import [{}] for plugin [{}]\n",
				entryPath, iter->name));
			SCRIPT_ERROR_CHECK();
			Py_DECREF(pyModule);
			Py_XDECREF(importlibUtil);
			return false;
		}

		Py_DECREF(pyRet);

		if (!pyModule)
		{
			ERROR_MSG(fmt::format("PluginManager::importComponentEntries: could not import [{}] for plugin [{}]\n",
				componentIter->second.entry, iter->name));
			SCRIPT_ERROR_CHECK();
			return false;
		}

		modules.push_back(pyModule);
	}

	Py_XDECREF(importlibUtil);

	return true;
}

void PluginManager::unloadComponentEntries(COMPONENT_TYPE componentType)
{
	std::map<COMPONENT_TYPE, std::vector<PyObject*> >::iterator iter = entryModules_.find(componentType);
	if (iter == entryModules_.end())
		return;

	for (std::vector<PyObject*>::iterator modIter = iter->second.begin(); modIter != iter->second.end(); ++modIter)
	{
		Py_XDECREF(*modIter);
	}

	entryModules_.erase(iter);
}

void PluginManager::callEntry(PyObject* pyEntry, const std::string& eventName, const char* format, bool arg)
{
	if (PyObject_HasAttrString(pyEntry, eventName.c_str()) <= 0)
		return;

	PyObject* pyResult = NULL;
	if (format && strlen(format) > 0)
		pyResult = PyObject_CallMethod(pyEntry, const_cast<char*>(eventName.c_str()), const_cast<char*>(format), arg ? 1 : 0);
	else
		pyResult = PyObject_CallMethod(pyEntry, const_cast<char*>(eventName.c_str()), const_cast<char*>(""));

	if (pyResult)
	{
		Py_DECREF(pyResult);
	}
	else
	{
		SCRIPT_ERROR_CHECK();
	}
}

void PluginManager::dispatch(COMPONENT_TYPE componentType, const std::string& eventName)
{
	if (!importComponentEntries(componentType))
		return;

	std::map<COMPONENT_TYPE, std::vector<PyObject*> >::iterator iter = entryModules_.find(componentType);
	if (iter == entryModules_.end())
		return;

	for (std::vector<PyObject*>::iterator modIter = iter->second.begin(); modIter != iter->second.end(); ++modIter)
		callEntry(*modIter, eventName, "", false);
}

void PluginManager::dispatch(COMPONENT_TYPE componentType, const std::string& eventName, bool arg)
{
	if (!importComponentEntries(componentType))
		return;

	std::map<COMPONENT_TYPE, std::vector<PyObject*> >::iterator iter = entryModules_.find(componentType);
	if (iter == entryModules_.end())
		return;

	for (std::vector<PyObject*>::iterator modIter = iter->second.begin(); modIter != iter->second.end(); ++modIter)
		callEntry(*modIter, eventName, "i", arg);
}

}
