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

#ifndef KBE_SERVER_PLUGIN_RUNTIME_H
#define KBE_SERVER_PLUGIN_RUNTIME_H

#include "common/common.h"
#include "pyscript/script.h"
#include "pyscript/pyobject_pointer.h"

namespace KBEngine {

// 管理单个服务进程中的插件入口模块，并保证初始化和关闭回调具有确定顺序。
// Owns plugin entry modules in one server process and gives startup and shutdown callbacks deterministic ordering.
class PluginRuntime
{
public:
	static PluginRuntime& instance();

	// 导入当前组件的所有入口模块；热重载时同时重新执行模块顶层代码。
	// Imports every entry module for the component and re-executes module-level code during a hot reload.
	bool initialize(COMPONENT_TYPE componentType, bool isReload);
	bool reload(COMPONENT_TYPE componentType);
	bool onComponentReady(bool isFirstGroup);
	void finalise();
	bool initialized() const { return initialized_; }

private:
	struct ModuleEntry
	{
		std::string pluginName;
		std::string moduleName;
		PyObjectPtr module;
	};

	PluginRuntime();

	bool callBoolCallback(ModuleEntry& entry, const char* callbackName, bool value);
	bool callNoArgsCallback(ModuleEntry& entry, const char* callbackName);
	std::string buildModuleName(const std::string& pluginName, COMPONENT_TYPE componentType,
		const std::string& entryName) const;

	bool initialized_;
	COMPONENT_TYPE componentType_;
	std::vector<ModuleEntry> modules_;
};

}

#endif // KBE_SERVER_PLUGIN_RUNTIME_H
