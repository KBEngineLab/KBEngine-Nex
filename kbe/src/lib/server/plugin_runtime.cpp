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

#include "server/plugin_runtime.h"

#include "helper/debug_helper.h"
#include "resmgr/plugins/plugin_manager.h"

#include <algorithm>

namespace KBEngine {

PluginRuntime::PluginRuntime() :
	initialized_(false),
	componentType_(UNKNOWN_COMPONENT_TYPE)
{
}

PluginRuntime& PluginRuntime::instance()
{
	static PluginRuntime instance;
	return instance;
}

// 使用插件包全名导入入口模块，避免多个插件都声明 plugin_entry 时共享同一个 sys.modules 项。
// Import entry modules by package-qualified name so plugins declaring plugin_entry never share one sys.modules entry.
std::string PluginRuntime::buildModuleName(const std::string& pluginName, COMPONENT_TYPE componentType,
	const std::string& entryName) const
{
	std::string moduleName = entryName;
	if (moduleName.size() > 3 && moduleName.compare(moduleName.size() - 3, 3, ".py") == 0)
		moduleName.erase(moduleName.size() - 3);

	std::replace(moduleName.begin(), moduleName.end(), '\\', '.');
	std::replace(moduleName.begin(), moduleName.end(), '/', '.');
	if (moduleName.compare(0, 8, "plugins.") == 0)
		return moduleName;

	const char* componentFolder = getComponentFolder(componentType);
	if (componentFolder[0] == '\0')
		return "";

	return "plugins." + pluginName + "." + componentFolder + "." + moduleName;
}

bool PluginRuntime::callBoolCallback(ModuleEntry& entry, const char* callbackName, bool value)
{
	int hasCallback = PyObject_HasAttrString(entry.module.get(), callbackName);
	if (hasCallback < 0)
	{
		PyErr_Print();
		return false;
	}
	if (hasCallback == 0)
		return true;

	PyObject* result = PyObject_CallMethod(entry.module.get(), const_cast<char*>(callbackName),
		const_cast<char*>("i"), value ? 1 : 0);
	if (result == NULL)
	{
		ERROR_MSG(fmt::format("PluginRuntime: plugin [{}] callback [{}] failed in module [{}].\n",
			entry.pluginName, callbackName, entry.moduleName));
		PyErr_Print();
		return false;
	}

	Py_DECREF(result);
	return true;
}

bool PluginRuntime::callNoArgsCallback(ModuleEntry& entry, const char* callbackName)
{
	int hasCallback = PyObject_HasAttrString(entry.module.get(), callbackName);
	if (hasCallback < 0)
	{
		PyErr_Print();
		return false;
	}
	if (hasCallback == 0)
		return true;

	PyObject* result = PyObject_CallMethod(entry.module.get(), const_cast<char*>(callbackName), NULL);
	if (result == NULL)
	{
		ERROR_MSG(fmt::format("PluginRuntime: plugin [{}] callback [{}] failed in module [{}].\n",
			entry.pluginName, callbackName, entry.moduleName));
		PyErr_Print();
		return false;
	}

	Py_DECREF(result);
	return true;
}

bool PluginRuntime::initialize(COMPONENT_TYPE componentType, bool isReload)
{
	if (initialized_)
		return componentType_ == componentType;

	if (!PluginManager::instance().initialize())
		return false;

	componentType_ = componentType;
	const std::vector<PluginDescriptor>& plugins = PluginManager::instance().plugins();
	for (std::vector<PluginDescriptor>::const_iterator pluginIter = plugins.begin();
		pluginIter != plugins.end(); ++pluginIter)
	{
		std::map<COMPONENT_TYPE, PluginComponentDescriptor>::const_iterator componentIter =
			pluginIter->components.find(componentType);
		if (componentIter == pluginIter->components.end() || componentIter->second.entry.empty())
			continue;

		ModuleEntry entry;
		entry.pluginName = pluginIter->name;
		entry.moduleName = buildModuleName(pluginIter->name, componentType, componentIter->second.entry);
		if (entry.moduleName.empty())
		{
			ERROR_MSG(fmt::format("PluginRuntime::initialize: plugin [{}] uses an unsupported component type [{}].\n",
				entry.pluginName, static_cast<int>(componentType)));
			finalise();
			return false;
		}

		PyObject* module = PyImport_ImportModule(entry.moduleName.c_str());
		if (module != NULL && isReload)
		{
			PyObject* reloadedModule = PyImport_ReloadModule(module);
			Py_DECREF(module);
			module = reloadedModule;
		}

		if (module == NULL)
		{
			ERROR_MSG(fmt::format("PluginRuntime::initialize: failed to import plugin [{}] entry module [{}].\n",
				entry.pluginName, entry.moduleName));
			PyErr_Print();
			// 已成功初始化的插件必须按逆序退出，避免导入链中途失败后残留注册项。
			// Already initialized plugins must unwind in reverse order so a partial import chain leaves no registrations behind.
			finalise();
			return false;
		}

		entry.module = PyObjectPtr(module, PyObjectPtr::STEAL_REF);
		modules_.push_back(entry);
		if (!callBoolCallback(modules_.back(), "onInit", isReload))
		{
			finalise();
			return false;
		}
	}

	initialized_ = true;
	return true;
}

bool PluginRuntime::reload(COMPONENT_TYPE componentType)
{
	finalise();
	return initialize(componentType, true);
}

bool PluginRuntime::onComponentReady(bool isFirstGroup)
{
	for (std::vector<ModuleEntry>::iterator iter = modules_.begin(); iter != modules_.end(); ++iter)
	{
		if (!callBoolCallback(*iter, "onComponentReady", isFirstGroup))
			return false;
	}
	return true;
}

void PluginRuntime::finalise()
{
	// 关闭顺序与初始化相反，允许后加载的插件先释放对早期插件的依赖。
	// Shutdown runs in reverse initialization order so later plugins release dependencies on earlier plugins first.
	for (std::vector<ModuleEntry>::reverse_iterator iter = modules_.rbegin(); iter != modules_.rend(); ++iter)
		callNoArgsCallback(*iter, "onFini");

	modules_.clear();
	componentType_ = UNKNOWN_COMPONENT_TYPE;
	initialized_ = false;
}

}
