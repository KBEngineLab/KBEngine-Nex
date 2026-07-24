/*
实体组件运行时负责承载组件脚本对象、属性编解码和生命周期关联。
The entity-component runtime owns component script objects, property serialization, and lifecycle binding.
*/


#ifndef KBE_ENTITY_COMPONENT_CELL_BASE_CLIENT__CALL_H
#define KBE_ENTITY_COMPONENT_CELL_BASE_CLIENT__CALL_H

#include "common/common.h"
#include "pyscript/scriptobject.h"
#include "entitydef/entitycallabstract.h"

#ifdef KBE_SERVER
#include "server/components.h"
#endif


namespace KBEngine{

namespace Network
{
class Channel;
}

class EntityCall;
class ScriptDefModule;
class RemoteEntityMethod;
class MethodDescription;
class PropertyDescription;

class EntityComponentCall : public EntityCallAbstract
{
	/** 子类化 将一些py操作填充进派生类 */
	INSTANCE_SCRIPT_HREADER(EntityComponentCall, EntityCallAbstract)
public:
	typedef std::function<RemoteEntityMethod* (MethodDescription* pMethodDescription, EntityComponentCall* pEntityCall)> EntityCallCallHookFunc;
	typedef std::function<Network::Channel* (EntityComponentCall&)> FindChannelFunc;

	EntityComponentCall(EntityCall* pEntityCall, PropertyDescription* pComponentPropertyDescription);

	~EntityComponentCall();

	/**
		脚本请求获取属性或者方法
	*/
	PyObject* onScriptGetAttribute(PyObject* attr);

	/**
		获得对象的描述
	*/
	PyObject* tp_repr();
	PyObject* tp_str();

	void c_str(char* s, size_t size);

	/**
		脚本被安装时被调用
	*/
	static void onInstallScript(PyObject* mod);

	virtual RemoteEntityMethod* createRemoteMethod(MethodDescription* pMethodDescription);

	/**
		unpickle方法
	*/
	static PyObject* __unpickle__(PyObject* self, PyObject* args);

	ScriptDefModule* pComponentScriptDefModule();

	virtual void newCall(Network::Bundle& bundle);

	static std::vector<EntityComponentCall*> getComponents(const std::string& name, EntityCall* pEntity, ScriptDefModule* pEntityScriptDescrs);

protected:
	EntityCall*								pEntityCall_;
	PropertyDescription*					pComponentPropertyDescription_;
};

}
#endif // KBE_ENTITY_COMPONENT_CELL_BASE_CLIENT__CALL_H
