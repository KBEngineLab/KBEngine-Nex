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
客户端库负责注册组件对象对脚本公开的方法和属性，避免服务端应用类型污染客户端运行时。
The client library owns the methods and attributes exposed by component objects so server application types do not leak into the client runtime.
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
SCRIPT_GET_DECLARE("base", pyGetBaseEntityCall, 0, 0)
SCRIPT_GETSET_DECLARE_END()
BASE_SCRIPT_INIT(EntityComponent, 0, 0, 0, 0, 0)

// 客户端组件没有额外的动态属性规则，统一沿用 ScriptObject 的查找语义。
// Client components add no dynamic attribute policy and therefore retain ScriptObject lookup semantics.
PyObject* EntityComponent::onScriptGetAttribute(PyObject* attr)
{
	return ScriptObject::onScriptGetAttribute(attr);
}

}
