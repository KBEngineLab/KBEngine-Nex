// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved.

#include "resmgr/plugins/plugin_manager.h"
#include "resmgr/plugins/plugin_manifest.h"

#include "helper/debug_helper.h"
#include "resmgr/resmgr.h"
#include "xml/xml.h"
#include "common/stringconv.h"

#include <algorithm>
#include <cctype>
#include <set>

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

static bool fileExists(const std::string& path)
{
	return access(path.c_str(), 0) == 0;
}

static void appendUnique(std::vector<std::string>& values, const std::string& value)
{
	if (value.empty())
		return;

	if (std::find(values.begin(), values.end(), value) == values.end())
		values.push_back(value);
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

	// 插件发现放在 resmgr 层，原因是它本质上只是 assets 资源目录的扩展索引。
	// 这里不导入 Python、不注册实体、不派发生命周期，避免把资源层变成重型运行时管理器。
	initialized_ = true;
	return discover();
}

void PluginManager::finalise()
{
	plugins_.clear();
	entities_.clear();
	entityMap_.clear();
	prefixSet_.clear();
	initialized_ = false;
}

bool PluginManager::discover()
{
	// 插件是否启用由 assets/plugins.xml 显式控制，和 entities.xml 一样属于 assets 配置。
	// 如果 plugins.xml 不存在，则认为当前 assets 没有启用任何插件；即使 plugins/ 目录下有文件也不会自动加载。
	std::string scriptsPath = Resmgr::getSingleton().getPyUserScriptsPath();
	std::string pluginsConfigFile = scriptsPath + "plugins.xml";
	if (!fileExists(pluginsConfigFile))
	{
		INFO_MSG(fmt::format("PluginManager::discover: plugins.xml not found [{}], plugins disabled.\n",
			pluginsConfigFile));
		return true;
	}

	std::vector<std::string> pluginNames;
	if (!loadPluginsConfig(pluginsConfigFile, pluginNames))
		return false;

	INFO_MSG(fmt::format("PluginManager::discover: plugins.xml declares {} plugin(s), file [{}].\n",
		pluginNames.size(), pluginsConfigFile));

	for (std::vector<std::string>::iterator iter = pluginNames.begin(); iter != pluginNames.end(); ++iter)
	{
		// plugins.xml 中的顺序就是插件加载顺序，后续实体 utype、DataTypes 合并顺序、
		// EntityDef MD5 和 Python path 顺序都会跟随这个配置顺序。
		std::string manifestFile = normalizePath(scriptsPath + "plugins/" + *iter + "/plugin.json");
		if (!fileExists(manifestFile))
		{
			ERROR_MSG(fmt::format("PluginManager::discover: plugin [{}] is declared in [{}], but manifest [{}] not found.\n",
				*iter, pluginsConfigFile, manifestFile));
			return false;
		}

		PluginDescriptor descriptor;
		if (!PluginManifest::load(manifestFile, parentPath(manifestFile), descriptor))
			return false;

		descriptor.manifestFile = manifestFile;

		if (descriptor.name != *iter)
		{
			ERROR_MSG(fmt::format("PluginManager::discover: plugin directory name [{}] does not match manifest name [{}] in [{}].\n",
				*iter, descriptor.name, manifestFile));
			return false;
		}

		if (!descriptor.enabled)
		{
			INFO_MSG(fmt::format("PluginManager::discover: plugin [{}] version [{}] is declared in plugins.xml but disabled by plugin.json, skipping.\n",
				descriptor.name, descriptor.version.empty() ? "unknown" : descriptor.version));
			continue;
		}

		if (!addPlugin(descriptor, manifestFile))
			return false;

		INFO_MSG(fmt::format("PluginManager::discover: loaded plugin [{}] prefix [{}] version [{}], entities={}, components={}, manifest=[{}].\n",
			descriptor.name, descriptor.prefix, descriptor.version.empty() ? "unknown" : descriptor.version,
			descriptor.entities.size(), descriptor.components.size(), manifestFile));
	}

	if (!plugins_.empty())
	{
		INFO_MSG(fmt::format("PluginManager::discover: loaded {} plugin(s), {} plugin entity(s).\n",
			plugins_.size(), entities_.size()));

		INFO_MSG("PluginManager::discover: plugin schema order:\n");
		for (std::vector<PluginDescriptor>::size_type i = 0; i < plugins_.size(); ++i)
		{
			std::string typeFile = normalizePath(plugins_[i].rootPath + "/entity_defs/types.xml");
			INFO_MSG(fmt::format("  [{}] plugin=[{}], prefix=[{}], entities={}, types={}, manifest=[{}]\n",
				i, plugins_[i].name, plugins_[i].prefix, plugins_[i].entities.size(),
				fileExists(typeFile) ? "yes" : "no", plugins_[i].manifestFile));
		}
	}

	return true;
}

bool PluginManager::loadPluginsConfig(const std::string& pluginsConfigFile, std::vector<std::string>& pluginNames) const
{
	SmartPointer<XML> xml(new XML());
	if (!xml->openSection(pluginsConfigFile.c_str()) || !xml->isGood())
		return false;

	TiXmlNode* node = xml->getRootNode();
	if (node == NULL)
		return true;

	std::set<std::string> usedNames;

	XML_FOR_BEGIN(node)
	{
		// 推荐格式是 <plugin>Bag</plugin>，这样节点名固定，插件名放在文本里。
		// 为了更接近 entities.xml 的手感，也允许 <Bag/> 这种直接用节点名声明插件的写法。
		std::string pluginName = xml->getKey(node);
		if (pluginName == "plugin")
		{
			const char* attrName = node->ToElement()->Attribute("name");
			if (attrName)
				pluginName = attrName;
			else if (node->FirstChild())
				pluginName = xml->getValStr(node->FirstChild());
			else
				pluginName = "";
		}

		pluginName = strutil::kbe_trim(pluginName);
		if (pluginName.empty())
		{
			ERROR_MSG(fmt::format("PluginManager::loadPluginsConfig: empty plugin declaration in [{}].\n",
				pluginsConfigFile));
			return false;
		}

		if (!validateName(pluginName, "plugin", pluginsConfigFile))
			return false;

		if (usedNames.find(pluginName) != usedNames.end())
		{
			ERROR_MSG(fmt::format("PluginManager::loadPluginsConfig: duplicate plugin [{}] in [{}].\n",
				pluginName, pluginsConfigFile));
			return false;
		}

		usedNames.insert(pluginName);
		pluginNames.push_back(pluginName);
	}
	XML_FOR_END(node);

	return true;
}

bool PluginManager::addPlugin(const PluginDescriptor& descriptor, const std::string& manifestFile)
{
	// 这里做跨插件的全局校验：插件名和实体名不能重复。
	// .def 是否存在、base/cell/client 脚本是否存在由 EntityDef/ScriptDefModule 在各自读取点校验，
	// 这样保持 KBE 原有“谁读取谁负责”的结构。
	if (descriptor.name.empty())
	{
		ERROR_MSG(fmt::format("PluginManager::addPlugin: plugin name is empty [{}]\n", manifestFile));
		return false;
	}

	if (!validateName(descriptor.name, "plugin", manifestFile))
		return false;

	if (descriptor.prefix.empty())
	{
		ERROR_MSG(fmt::format("PluginManager::addPlugin: plugin [{}] prefix is required in [{}]\n",
			descriptor.name, manifestFile));
		return false;
	}

	if (!validateName(descriptor.prefix, "plugin prefix", manifestFile))
		return false;

	if (prefixSet_.find(descriptor.prefix) != prefixSet_.end())
	{
		ERROR_MSG(fmt::format("PluginManager::addPlugin: duplicate plugin prefix [{}], manifest [{}]\n",
			descriptor.prefix, manifestFile));
		return false;
	}

	for (std::vector<PluginDescriptor>::const_iterator iter = plugins_.begin(); iter != plugins_.end(); ++iter)
	{
		if (iter->name == descriptor.name)
		{
			ERROR_MSG(fmt::format("PluginManager::addPlugin: duplicate plugin [{}]\n", descriptor.name));
			return false;
		}
	}

	PluginDescriptor normalizedDescriptor = descriptor;
	normalizedDescriptor.manifestFile = manifestFile;
	normalizedDescriptor.entities.clear();

	for (std::vector<PluginEntityDescriptor>::const_iterator iter = descriptor.entities.begin(); iter != descriptor.entities.end(); ++iter)
	{
		if (iter->name.empty())
		{
			ERROR_MSG(fmt::format("PluginManager::addPlugin: empty entity name in plugin [{}]\n", descriptor.name));
			return false;
		}

		if (!validateName(iter->name, "entity", manifestFile))
			return false;

		if (!validatePrefixedName(iter->name, descriptor.prefix, "plugin entity", manifestFile))
			return false;

		if (!validateRelativePath(iter->def, "entity def", manifestFile))
			return false;

		if (entityMap_.find(iter->name) != entityMap_.end())
		{
			const PluginEntityDescriptor& oldEntity = entityMap_[iter->name];
			ERROR_MSG(fmt::format("PluginManager::addPlugin: duplicate plugin entity [{}], oldPlugin=[{}], newPlugin=[{}], oldManifest=[{}], newManifest=[{}]\n",
				iter->name, oldEntity.pluginName, descriptor.name, oldEntity.manifestFile, manifestFile));
			return false;
		}

		PluginEntityDescriptor normalizedEntity = *iter;
		normalizedEntity.pluginName = descriptor.name;
		normalizedEntity.pluginPrefix = descriptor.prefix;
		normalizedEntity.manifestFile = manifestFile;

		entityMap_[iter->name] = normalizedEntity;
		entities_.push_back(normalizedEntity);
		normalizedDescriptor.entities.push_back(normalizedEntity);
	}

	for (std::map<COMPONENT_TYPE, PluginComponentDescriptor>::const_iterator componentIter = descriptor.components.begin();
		componentIter != descriptor.components.end(); ++componentIter)
	{
		if (!componentIter->second.entry.empty() &&
			!validateRelativePath(componentIter->second.entry, "component entry", manifestFile))
		{
			return false;
		}

		for (std::vector<std::string>::const_iterator pathIter = componentIter->second.rawScriptPaths.begin();
			pathIter != componentIter->second.rawScriptPaths.end(); ++pathIter)
		{
			if (!validateRelativePath(*pathIter, "component scriptPath", manifestFile))
				return false;
		}
	}

	plugins_.push_back(normalizedDescriptor);
	prefixSet_.insert(descriptor.prefix);

	INFO_MSG(fmt::format("PluginManager::addPlugin: registered plugin [{}] prefix [{}], {} entity(s), {} component(s), manifest [{}]\n",
		descriptor.name, descriptor.prefix, normalizedDescriptor.entities.size(),
		descriptor.components.size(), manifestFile));

	return true;
}

bool PluginManager::validateName(const std::string& name, const char* label, const std::string& manifestFile) const
{
	if (name.empty())
		return false;

	for (std::string::const_iterator iter = name.begin(); iter != name.end(); ++iter)
	{
		unsigned char ch = static_cast<unsigned char>(*iter);
		if (!(isalnum(ch) || *iter == '_'))
		{
			ERROR_MSG(fmt::format("PluginManager::validateName: invalid {} name [{}] in [{}]\n",
				label, name, manifestFile));
			return false;
		}
	}

	return true;
}

bool PluginManager::validatePrefixedName(const std::string& name, const std::string& prefix, const char* label, const std::string& manifestFile) const
{
	if (name.size() <= prefix.size() || name.compare(0, prefix.size(), prefix) != 0)
	{
		ERROR_MSG(fmt::format("PluginManager::validatePrefixedName: {} name [{}] must be plugin prefix [{}] plus a non-empty suffix, valid examples: [{}Item], [{}_Item], invalid: [{}], manifest [{}]\n",
			label, name, prefix, prefix, prefix, prefix, manifestFile));
		return false;
	}

	char boundary = name[prefix.size()];
	if (boundary == '_' || (boundary >= 'A' && boundary <= 'Z'))
		return true;

	ERROR_MSG(fmt::format("PluginManager::validatePrefixedName: {} name [{}] uses prefix [{}] but has invalid boundary char (0x{:02x}), valid examples: [{}Item], [{}_Item], manifest [{}]\n",
		label, name, prefix, static_cast<unsigned char>(boundary), prefix, prefix, manifestFile));
	return false;
}

bool PluginManager::validateRelativePath(const std::string& path, const char* label, const std::string& manifestFile) const
{
	std::string normalized = normalizePath(path);
	if (normalized.empty())
		return true;

	if (normalized[0] == '/' || normalized.find(':') != std::string::npos ||
		normalized.find("../") != std::string::npos || normalized == ".." ||
		normalized.find("/..") != std::string::npos)
	{
		ERROR_MSG(fmt::format("PluginManager::validateRelativePath: invalid {} path [{}] in [{}]\n",
			label, path, manifestFile));
		return false;
	}

	return true;
}

const PluginEntityDescriptor* PluginManager::findEntity(const std::string& name) const
{
	std::map<std::string, PluginEntityDescriptor>::const_iterator iter = entityMap_.find(name);
	return iter == entityMap_.end() ? NULL : &iter->second;
}

std::vector<std::string> PluginManager::getComponentPythonPaths(COMPONENT_TYPE componentType) const
{
	std::vector<std::string> paths;

	for (std::vector<PluginDescriptor>::const_iterator iter = plugins_.begin(); iter != plugins_.end(); ++iter)
	{
		std::string commonPath = normalizePath(iter->rootPath + "/common");
		if (fileExists(commonPath))
			appendUnique(paths, commonPath);

		for (std::vector<PluginEntityDescriptor>::const_iterator entityIter = iter->entities.begin(); entityIter != iter->entities.end(); ++entityIter)
		{
			bool needsComponentPath =
				(componentType == BASEAPP_TYPE && entityIter->hasBase) ||
				(componentType == CELLAPP_TYPE && entityIter->hasCell) ||
				(componentType == CLIENT_TYPE && entityIter->hasClient);

			if (needsComponentPath)
				appendUnique(paths, normalizePath(iter->rootPath + "/" + getComponentFolder(componentType)));
		}

		if (componentType == BOTS_TYPE)
		{
			std::string botsPath = normalizePath(iter->rootPath + "/bots");
			if (fileExists(botsPath))
				appendUnique(paths, botsPath);
		}

		std::map<COMPONENT_TYPE, PluginComponentDescriptor>::const_iterator componentIter = iter->components.find(componentType);
		if (componentIter == iter->components.end())
			continue;

		for (std::vector<std::string>::const_iterator pathIter = componentIter->second.scriptPaths.begin(); pathIter != componentIter->second.scriptPaths.end(); ++pathIter)
			appendUnique(paths, *pathIter);
	}

	return paths;
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

std::vector<PluginTypeFileDescriptor> PluginManager::getTypeFileDescriptors() const
{
	std::vector<PluginTypeFileDescriptor> files;

	for (std::vector<PluginDescriptor>::const_iterator iter = plugins_.begin(); iter != plugins_.end(); ++iter)
	{
		std::string file = normalizePath(iter->rootPath + "/entity_defs/types.xml");
		if (fileExists(file))
		{
			PluginTypeFileDescriptor descriptor;
			descriptor.pluginName = iter->name;
			descriptor.pluginPrefix = iter->prefix;
			descriptor.file = file;
			descriptor.manifestFile = iter->manifestFile;
			files.push_back(descriptor);
		}
	}

	return files;
}

}
