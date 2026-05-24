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
	bool hasBase = false;
	bool hasCell = false;
	bool hasClient = false;
};

struct PluginComponentDescriptor
{
	std::vector<std::string> scriptPaths;
	std::string entry;
};

struct PluginDescriptor
{
	std::string name;
	std::string version;
	std::string rootPath;
	bool enabled = true;
	std::vector<PluginEntityDescriptor> entities;
	std::map<COMPONENT_TYPE, PluginComponentDescriptor> components;
};

}

#endif // KBE_PLUGIN_DESCRIPTOR_H
