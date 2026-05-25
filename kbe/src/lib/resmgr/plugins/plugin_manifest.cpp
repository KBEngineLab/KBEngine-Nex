// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved.

#include "resmgr/plugins/plugin_manifest.h"
#include "helper/debug_helper.h"

// common/strutil.h 为了兼容旧平台会把 strtoll/strtoull 宏替换成 _strtoi64/_strtoui64。
// nlohmann-json 内部使用 std::strtoll/std::strtoull，宏替换会把它变成 std::_strtoi64，
// MSVC 的 std 命名空间里没有这个非标准名字，所以在引入第三方头之前撤销这组兼容宏。
#ifdef strtoll
#undef strtoll
#endif
#ifdef strtoull
#undef strtoull
#endif
#ifdef strtoq
#undef strtoq
#endif
#ifdef strtouq
#undef strtouq
#endif

#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>

namespace KBEngine {
namespace {

static std::string joinPath(const std::string& a, const std::string& b)
{
	if (a.empty())
		return b;

	char tail = a[a.size() - 1];
	if (tail == '/' || tail == '\\')
		return a + b;

	return a + "/" + b;
}

static bool getString(const nlohmann::json& object, const std::string& key, std::string& out, bool required)
{
	nlohmann::json::const_iterator value = object.find(key);
	if (value == object.end())
		return !required;

	if (!value->is_string())
		return false;

	out = value->get<std::string>();
	return true;
}

static bool getBool(const nlohmann::json& object, const std::string& key, bool& out, bool defaultValue)
{
	nlohmann::json::const_iterator value = object.find(key);
	if (value == object.end())
	{
		out = defaultValue;
		return true;
	}

	if (!value->is_boolean())
		return false;

	out = value->get<bool>();
	return true;
}

}

bool PluginManifest::load(const std::string& file, const std::string& pluginRoot, PluginDescriptor& descriptor)
{
	// manifest 是插件系统的入口配置，使用 nlohmann-json 解析可以获得完整 JSON 语法支持和明确的错误信息。
	// 这里仍只读取插件系统需要的字段，额外字段会被忽略，方便后续扩展 manifest 格式。
	std::ifstream stream(file.c_str(), std::ios::in | std::ios::binary);
	if (!stream.is_open())
	{
		ERROR_MSG(fmt::format("PluginManifest::load: could not open manifest [{}]\n", file));
		return false;
	}

	std::stringstream buffer;
	buffer << stream.rdbuf();

	nlohmann::json root;
	try
	{
		root = nlohmann::json::parse(buffer.str());
	}
	catch (const nlohmann::json::exception& e)
	{
		ERROR_MSG(fmt::format("PluginManifest::load: invalid json [{}], error: {}\n", file, e.what()));
		return false;
	}

	if (!root.is_object())
	{
		ERROR_MSG(fmt::format("PluginManifest::load: manifest root must be object [{}]\n", file));
		return false;
	}

	descriptor = PluginDescriptor();
	descriptor.rootPath = pluginRoot;

	if (!getString(root, "name", descriptor.name, true) ||
		!getString(root, "prefix", descriptor.prefix, true) ||
		!getString(root, "version", descriptor.version, false) ||
		!getBool(root, "enabled", descriptor.enabled, true))
	{
		ERROR_MSG(fmt::format("PluginManifest::load: invalid manifest header [{}]\n", file));
		return false;
	}

	nlohmann::json::const_iterator entities = root.find("entities");
	if (entities != root.end())
	{
		if (!entities->is_array())
		{
			ERROR_MSG(fmt::format("PluginManifest::load: entities must be array [{}]\n", file));
			return false;
		}

		// entities 只描述插件提供哪些 EntityDef。
		// 实体真正的加载仍发生在 EntityDef::initialize，manifest 这里不直接注册 ScriptDefModule。
		for (nlohmann::json::const_iterator iter = entities->begin(); iter != entities->end(); ++iter)
		{
			if (!iter->is_object())
			{
				ERROR_MSG(fmt::format("PluginManifest::load: entity item must be object [{}]\n", file));
				return false;
			}

			PluginEntityDescriptor entity;
			if (!getString(*iter, "name", entity.name, true) ||
				!getString(*iter, "def", entity.def, true) ||
				!getBool(*iter, "hasBase", entity.hasBase, false) ||
				!getBool(*iter, "hasCell", entity.hasCell, false) ||
				!getBool(*iter, "hasClient", entity.hasClient, false))
			{
				ERROR_MSG(fmt::format("PluginManifest::load: invalid entity in [{}]\n", file));
				return false;
			}

			entity.defFullPath = joinPath(pluginRoot, entity.def);
			entity.pluginRootPath = pluginRoot;
			descriptor.entities.push_back(entity);
		}
	}

	nlohmann::json::const_iterator components = root.find("components");
	if (components != root.end())
	{
		if (!components->is_object())
		{
			ERROR_MSG(fmt::format("PluginManifest::load: components must be object [{}]\n", file));
			return false;
		}

		for (nlohmann::json::const_iterator iter = components->begin(); iter != components->end(); ++iter)
		{
			COMPONENT_TYPE componentType = getComponentTypeFromFolder(iter.key());
			if (componentType == UNKNOWN_COMPONENT_TYPE || !iter.value().is_object())
			{
				ERROR_MSG(fmt::format("PluginManifest::load: invalid component [{}] in [{}]\n", iter.key(), file));
				return false;
			}

			PluginComponentDescriptor component;
			if (!getString(iter.value(), "entry", component.entry, false))
			{
				ERROR_MSG(fmt::format("PluginManifest::load: invalid entry in component [{}], file [{}]\n", iter.key(), file));
				return false;
			}

			nlohmann::json::const_iterator scriptPaths = iter.value().find("scriptPaths");
			if (scriptPaths != iter.value().end())
			{
				if (!scriptPaths->is_array())
				{
					ERROR_MSG(fmt::format("PluginManifest::load: scriptPaths must be array in component [{}], file [{}]\n", iter.key(), file));
					return false;
				}

				// rawScriptPaths 用于后续安全校验，scriptPaths 是拼好的绝对路径，方便组件安装 sys.path。
				for (nlohmann::json::const_iterator pathIter = scriptPaths->begin(); pathIter != scriptPaths->end(); ++pathIter)
				{
					if (!pathIter->is_string())
					{
						ERROR_MSG(fmt::format("PluginManifest::load: scriptPath must be string in component [{}], file [{}]\n", iter.key(), file));
						return false;
					}

					std::string scriptPath = pathIter->get<std::string>();
					component.rawScriptPaths.push_back(scriptPath);
					component.scriptPaths.push_back(joinPath(pluginRoot, scriptPath));
				}
			}

			descriptor.components[componentType] = component;
		}
	}

	return true;
}

}
