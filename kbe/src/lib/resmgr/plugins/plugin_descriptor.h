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

// 组件类型与目录名的统一映射表。componentFolder() 和 componentTypeFromName() 均由此推导，
// 新增组件类型时只需改这一处。当前只列出 Python 脚本层会出现的组件。
struct ComponentFolderMapping
{
	COMPONENT_TYPE type;
	const char* folder;
};

static const ComponentFolderMapping kComponentFolderMappings[] =
{
	{ BASEAPP_TYPE,    "base" },
	{ CELLAPP_TYPE,    "cell" },
	{ DBMGR_TYPE,      "db" },
	{ INTERFACES_TYPE, "interface" },
	{ LOGINAPP_TYPE,   "login" },
	{ LOGGER_TYPE,     "logger" },
	{ BOTS_TYPE,       "bots" },
	{ CLIENT_TYPE,     "client" },
};

inline const char* getComponentFolder(COMPONENT_TYPE componentType)
{
	for (size_t i = 0; i < sizeof(kComponentFolderMappings) / sizeof(kComponentFolderMappings[0]); ++i)
	{
		if (kComponentFolderMappings[i].type == componentType)
			return kComponentFolderMappings[i].folder;
	}
	return "";
}

inline COMPONENT_TYPE getComponentTypeFromFolder(const std::string& folder)
{
	for (size_t i = 0; i < sizeof(kComponentFolderMappings) / sizeof(kComponentFolderMappings[0]); ++i)
	{
		if (folder == kComponentFolderMappings[i].folder)
			return kComponentFolderMappings[i].type;
	}
	return UNKNOWN_COMPONENT_TYPE;
}

}

#endif // KBE_PLUGIN_DESCRIPTOR_H

