/*
This source file is part of KBEngine
For the latest info, see http://www.kbengine.org/

Copyright (c) 2008-2018 KBEngine.

KBEngine is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

KBEngine is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License
along with KBEngine.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "entitydef/entity_component.h"

namespace KBEngine{

/*
Baseapp 在本模块注册组件的脚本类型，使 base 与 client 调用入口只暴露给服务端组件对象。
Baseapp registers the component script type here so base and client call endpoints are exposed only to server-side component objects.
*/
SCRIPT_METHOD_DECLARE_BEGIN(EntityComponent)
SCRIPT_METHOD_DECLARE("addTimer", pyAddTimer, METH_VARARGS, 0)
SCRIPT_METHOD_DECLARE("delTimer", pyDelTimer, METH_VARARGS, 0)
SCRIPT_METHOD_DECLARE_END()

SCRIPT_GETSET_DECLARE_BEGIN(EntityComponent)
SCRIPT_GET_DECLARE("ownerID", pyGetOwnerID, 0, 0)
SCRIPT_GET_DECLARE("owner", pyGetOwner, 0, 0)
SCRIPT_GET_DECLARE("name", pyName, 0, 0)
SCRIPT_GET_DECLARE("className", pyGetClassName, 0, 0)
SCRIPT_GET_DECLARE("isDestroyed", pyIsDestroyed, 0, 0)
SCRIPT_GET_DECLARE("cell", pyGetCellEntityCall, 0, 0)
SCRIPT_GET_DECLARE("client", pyGetClientEntityCall, 0, 0)
SCRIPT_GETSET_DECLARE_END()
BASE_SCRIPT_INIT(EntityComponent, 0, 0, 0, 0, 0)

// Baseapp 组件沿用 ScriptObject 的属性查找规则，具体属性仍由组件描述负责解析。
// Baseapp components retain ScriptObject lookup while component descriptions resolve concrete properties.
PyObject* EntityComponent::onScriptGetAttribute(PyObject* attr)
{
	return ScriptObject::onScriptGetAttribute(attr);
}

}
