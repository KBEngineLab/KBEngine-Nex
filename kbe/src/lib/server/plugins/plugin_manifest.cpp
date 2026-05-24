// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved.

#include "server/plugins/plugin_manifest.h"
#include "helper/debug_helper.h"

#include <fstream>
#include <sstream>

namespace KBEngine {
namespace {

struct JsonValue
{
	enum Type
	{
		NIL,
		BOOL,
		STRING,
		ARRAY,
		OBJECT
	};

	Type type = NIL;
	bool boolValue = false;
	std::string stringValue;
	std::vector<JsonValue> arrayValue;
	std::map<std::string, JsonValue> objectValue;

	const JsonValue* get(const std::string& key) const
	{
		std::map<std::string, JsonValue>::const_iterator iter = objectValue.find(key);
		return iter == objectValue.end() ? NULL : &iter->second;
	}
};

class JsonParser
{
public:
	JsonParser(const std::string& text) : text_(text), pos_(0) {}

	bool parse(JsonValue& value)
	{
		skip();
		if (!parseValue(value))
			return false;
		skip();
		return pos_ == text_.size();
	}

private:
	void skip()
	{
		while (pos_ < text_.size() && isspace((unsigned char)text_[pos_]))
			++pos_;
	}

	bool consume(char ch)
	{
		skip();
		if (pos_ >= text_.size() || text_[pos_] != ch)
			return false;
		++pos_;
		return true;
	}

	bool parseValue(JsonValue& value)
	{
		skip();
		if (pos_ >= text_.size())
			return false;

		char ch = text_[pos_];
		if (ch == '"')
		{
			value.type = JsonValue::STRING;
			return parseString(value.stringValue);
		}
		if (ch == '{')
			return parseObject(value);
		if (ch == '[')
			return parseArray(value);
		if (text_.compare(pos_, 4, "true") == 0)
		{
			pos_ += 4;
			value.type = JsonValue::BOOL;
			value.boolValue = true;
			return true;
		}
		if (text_.compare(pos_, 5, "false") == 0)
		{
			pos_ += 5;
			value.type = JsonValue::BOOL;
			value.boolValue = false;
			return true;
		}
		if (text_.compare(pos_, 4, "null") == 0)
		{
			pos_ += 4;
			value.type = JsonValue::NIL;
			return true;
		}
		return false;
	}

	bool parseString(std::string& out)
	{
		if (!consume('"'))
			return false;

		out.clear();
		while (pos_ < text_.size())
		{
			char ch = text_[pos_++];
			if (ch == '"')
				return true;

			if (ch == '\\')
			{
				if (pos_ >= text_.size())
					return false;

				char esc = text_[pos_++];
				switch (esc)
				{
				case '"': out.push_back('"'); break;
				case '\\': out.push_back('\\'); break;
				case '/': out.push_back('/'); break;
				case 'b': out.push_back('\b'); break;
				case 'f': out.push_back('\f'); break;
				case 'n': out.push_back('\n'); break;
				case 'r': out.push_back('\r'); break;
				case 't': out.push_back('\t'); break;
				default: return false;
				}
			}
			else
			{
				out.push_back(ch);
			}
		}

		return false;
	}

	bool parseObject(JsonValue& value)
	{
		if (!consume('{'))
			return false;

		value.type = JsonValue::OBJECT;
		value.objectValue.clear();
		skip();
		if (consume('}'))
			return true;

		while (true)
		{
			std::string key;
			if (!parseString(key) || !consume(':'))
				return false;

			JsonValue child;
			if (!parseValue(child))
				return false;

			value.objectValue[key] = child;
			if (consume('}'))
				return true;
			if (!consume(','))
				return false;
		}
	}

	bool parseArray(JsonValue& value)
	{
		if (!consume('['))
			return false;

		value.type = JsonValue::ARRAY;
		value.arrayValue.clear();
		skip();
		if (consume(']'))
			return true;

		while (true)
		{
			JsonValue child;
			if (!parseValue(child))
				return false;

			value.arrayValue.push_back(child);
			if (consume(']'))
				return true;
			if (!consume(','))
				return false;
		}
	}

	const std::string& text_;
	size_t pos_;
};

static std::string joinPath(const std::string& a, const std::string& b)
{
	if (a.empty())
		return b;

	char tail = a[a.size() - 1];
	if (tail == '/' || tail == '\\')
		return a + b;

	return a + "/" + b;
}

static COMPONENT_TYPE componentTypeFromName(const std::string& name)
{
	if (name == "base")
		return BASEAPP_TYPE;
	if (name == "cell")
		return CELLAPP_TYPE;
	if (name == "db")
		return DBMGR_TYPE;
	if (name == "interface")
		return INTERFACES_TYPE;
	if (name == "login")
		return LOGINAPP_TYPE;
	if (name == "logger")
		return LOGGER_TYPE;
	if (name == "bots")
		return BOTS_TYPE;
	if (name == "client")
		return CLIENT_TYPE;

	return UNKNOWN_COMPONENT_TYPE;
}

static bool getString(const JsonValue& object, const std::string& key, std::string& out, bool required)
{
	const JsonValue* value = object.get(key);
	if (!value)
		return !required;

	if (value->type != JsonValue::STRING)
		return false;

	out = value->stringValue;
	return true;
}

static bool getBool(const JsonValue& object, const std::string& key, bool& out, bool defaultValue)
{
	const JsonValue* value = object.get(key);
	if (!value)
	{
		out = defaultValue;
		return true;
	}

	if (value->type != JsonValue::BOOL)
		return false;

	out = value->boolValue;
	return true;
}

}

bool PluginManifest::load(const std::string& file, const std::string& pluginRoot, PluginDescriptor& descriptor)
{
	std::ifstream stream(file.c_str(), std::ios::in | std::ios::binary);
	if (!stream.is_open())
	{
		ERROR_MSG(fmt::format("PluginManifest::load: could not open manifest [{}]\n", file));
		return false;
	}

	std::stringstream buffer;
	buffer << stream.rdbuf();

	JsonValue root;
	JsonParser parser(buffer.str());
	if (!parser.parse(root) || root.type != JsonValue::OBJECT)
	{
		ERROR_MSG(fmt::format("PluginManifest::load: invalid json [{}]\n", file));
		return false;
	}

	descriptor = PluginDescriptor();
	descriptor.rootPath = pluginRoot;

	if (!getString(root, "name", descriptor.name, true) ||
		!getString(root, "version", descriptor.version, false) ||
		!getBool(root, "enabled", descriptor.enabled, true))
	{
		ERROR_MSG(fmt::format("PluginManifest::load: invalid manifest header [{}]\n", file));
		return false;
	}

	const JsonValue* entities = root.get("entities");
	if (entities && entities->type == JsonValue::ARRAY)
	{
		for (std::vector<JsonValue>::const_iterator iter = entities->arrayValue.begin(); iter != entities->arrayValue.end(); ++iter)
		{
			if (iter->type != JsonValue::OBJECT)
				return false;

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
			descriptor.entities.push_back(entity);
		}
	}

	const JsonValue* components = root.get("components");
	if (components && components->type == JsonValue::OBJECT)
	{
		for (std::map<std::string, JsonValue>::const_iterator iter = components->objectValue.begin(); iter != components->objectValue.end(); ++iter)
		{
			COMPONENT_TYPE componentType = componentTypeFromName(iter->first);
			if (componentType == UNKNOWN_COMPONENT_TYPE || iter->second.type != JsonValue::OBJECT)
			{
				ERROR_MSG(fmt::format("PluginManifest::load: invalid component [{}] in [{}]\n", iter->first, file));
				return false;
			}

			PluginComponentDescriptor component;
			getString(iter->second, "entry", component.entry, false);

			const JsonValue* scriptPaths = iter->second.get("scriptPaths");
			if (scriptPaths && scriptPaths->type == JsonValue::ARRAY)
			{
				for (std::vector<JsonValue>::const_iterator pathIter = scriptPaths->arrayValue.begin(); pathIter != scriptPaths->arrayValue.end(); ++pathIter)
				{
					if (pathIter->type != JsonValue::STRING)
						return false;

					component.scriptPaths.push_back(joinPath(pluginRoot, pathIter->stringValue));
				}
			}

			descriptor.components[componentType] = component;
		}
	}

	return true;
}

}
