// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved.

#ifndef KBE_PLUGIN_MANAGER_H
#define KBE_PLUGIN_MANAGER_H

#include "resmgr/plugins/plugin_descriptor.h"
#include <set>

namespace KBEngine {

class PluginManager
{
public:
	static PluginManager& instance();

	bool initialize();
	void finalise();

	const std::vector<PluginDescriptor>& plugins() const { return plugins_; }
	const std::vector<PluginEntityDescriptor>& entities() const { return entities_; }
	const PluginEntityDescriptor* findEntity(const std::string& name) const;

	// 只返回当前组件应该加入 sys.path 的插件目录，不触碰 Python 解释器。
	// 各 App 仍按 KBE 原有流程自行安装脚本环境，这里只是避免多处重复拼路径。
	std::vector<std::string> getComponentPythonPaths(COMPONENT_TYPE componentType) const;
	std::vector<std::string> getTypeFiles() const;
	std::vector<PluginTypeFileDescriptor> getTypeFileDescriptors() const;

private:
	PluginManager();

	bool discover();
	bool loadPluginsConfig(const std::string& pluginsConfigFile, std::vector<std::string>& pluginNames) const;
	bool addPlugin(const PluginDescriptor& descriptor, const std::string& manifestFile);
	bool validateName(const std::string& name, const char* label, const std::string& manifestFile) const;
	bool validatePrefixedName(const std::string& name, const std::string& prefix, const char* label, const std::string& manifestFile) const;
	bool validateRelativePath(const std::string& path, const char* label, const std::string& manifestFile) const;

	bool initialized_;
	std::vector<PluginDescriptor> plugins_;
	std::vector<PluginEntityDescriptor> entities_;
	std::map<std::string, PluginEntityDescriptor> entityMap_;
	std::set<std::string> prefixSet_;
};

}

#endif // KBE_PLUGIN_MANAGER_H
