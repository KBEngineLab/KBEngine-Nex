// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved.

#ifndef KBE_PLUGIN_MANAGER_H
#define KBE_PLUGIN_MANAGER_H

#include "server/plugins/plugin_descriptor.h"
#include "pyscript/kbe_python.h"

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

	std::vector<std::string> getTypeFiles() const;
	std::vector<std::string> getComponentPythonPaths(COMPONENT_TYPE componentType) const;
	bool hasComponentScript(const std::string& entityName, COMPONENT_TYPE componentType) const;
	bool getComponentAssertion(const std::string& entityName, bool& hasBase, bool& hasCell, bool& hasClient) const;

	bool importComponentEntries(COMPONENT_TYPE componentType);
	void unloadComponentEntries(COMPONENT_TYPE componentType);
	void dispatch(COMPONENT_TYPE componentType, const std::string& eventName);
	void dispatch(COMPONENT_TYPE componentType, const std::string& eventName, bool arg);

private:
	PluginManager();

	bool discover();
	bool validateAndAdd(const PluginDescriptor& descriptor, const std::string& manifestFile);
	void callEntry(PyObject* pyEntry, const std::string& eventName, const char* format, bool arg);

	bool initialized_;
	std::vector<PluginDescriptor> plugins_;
	std::vector<PluginEntityDescriptor> entities_;
	std::map<std::string, PluginEntityDescriptor> entityMap_;
	std::map<COMPONENT_TYPE, std::vector<PyObject*> > entryModules_;
};

}

#endif // KBE_PLUGIN_MANAGER_H
