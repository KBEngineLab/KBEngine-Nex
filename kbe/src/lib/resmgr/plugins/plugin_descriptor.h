// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved.

#ifndef KBE_PLUGIN_DESCRIPTOR_H
#define KBE_PLUGIN_DESCRIPTOR_H

#include "common/common.h"

namespace KBEngine {

struct PluginEntityDescriptor
{
	std::string name;
	std::string def;
	std::string defFullPath;
	std::string pluginRootPath;
	std::string pluginName;
	std::string pluginPrefix;
	std::string manifestFile;
	bool hasBase = false;
	bool hasCell = false;
	bool hasClient = false;
};

struct PluginTypeFileDescriptor
{
	std::string pluginName;
	std::string pluginPrefix;
	std::string file;
	std::string manifestFile;
};

struct PluginComponentDescriptor
{
	// manifest 中的原始相对路径，用于校验是否越出插件目录。
	std::vector<std::string> rawScriptPaths;
	// 已经拼成绝对路径的脚本路径，供各组件安装 sys.path 时直接使用。
	std::vector<std::string> scriptPaths;
	std::string entry;
};

struct PluginDescriptor
{
	std::string name;
	std::string prefix;
	std::string version;
	std::string rootPath;
	std::string manifestFile;
	bool enabled = true;
	std::vector<PluginEntityDescriptor> entities;
	std::map<COMPONENT_TYPE, PluginComponentDescriptor> components;
};

}

#endif // KBE_PLUGIN_DESCRIPTOR_H
