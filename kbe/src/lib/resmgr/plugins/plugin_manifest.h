// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved.

#ifndef KBE_PLUGIN_MANIFEST_H
#define KBE_PLUGIN_MANIFEST_H

#include "resmgr/plugins/plugin_descriptor.h"

namespace KBEngine {

class PluginManifest
{
public:
	static bool load(const std::string& file, const std::string& pluginRoot, PluginDescriptor& descriptor);
};

}

#endif // KBE_PLUGIN_MANIFEST_H
