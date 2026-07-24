/*
kbcmd 只需要安装组件脚本类型和基础属性，不执行服务器侧的计时器或远程调用行为。
kbcmd only installs the component script type and core properties; it does not run server timers or remote-call behavior.
*/

#include "entitydef/entity_component.h"

namespace KBEngine {

SCRIPT_GETSET_DECLARE_BEGIN(EntityComponent)
SCRIPT_GET_DECLARE("ownerID", pyGetOwnerID, 0, 0)
SCRIPT_GET_DECLARE("owner", pyGetOwner, 0, 0)
SCRIPT_GET_DECLARE("name", pyName, 0, 0)
SCRIPT_GET_DECLARE("isDestroyed", pyIsDestroyed, 0, 0)
SCRIPT_GETSET_DECLARE_END()
BASE_SCRIPT_INIT(EntityComponent, 0, 0, 0, 0, 0)

// SDK generation does not expose runtime-only component attributes.
// SDK 生成过程不暴露仅运行时可用的组件属性。
PyObject* EntityComponent::onScriptGetAttribute(PyObject* attr)
{
	return ScriptObject::onScriptGetAttribute(attr);
}

}
