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
DBMgr 只注册持久化流程需要的组件标识属性，不暴露 Baseapp 或 Cellapp 的运行时调用接口。
DBMgr registers only component identity properties required by persistence and exposes no Baseapp or Cellapp runtime call endpoints.
*/
SCRIPT_GETSET_DECLARE_BEGIN(EntityComponent)
SCRIPT_GET_DECLARE("ownerID", pyGetOwnerID, 0, 0)
SCRIPT_GET_DECLARE("owner", pyGetOwner, 0, 0)
SCRIPT_GET_DECLARE("name", pyName, 0, 0)
SCRIPT_GET_DECLARE("isDestroyed", pyIsDestroyed, 0, 0)
SCRIPT_GETSET_DECLARE_END()
BASE_SCRIPT_INIT(EntityComponent, 0, 0, 0, 0, 0)

// DBMgr 组件保留基础属性查找语义，数据库字段解析仍由实体定义层统一处理。
// DBMgr components keep base lookup semantics while the entity-definition layer continues to resolve database fields.
PyObject* EntityComponent::onScriptGetAttribute(PyObject* attr)
{
	return ScriptObject::onScriptGetAttribute(attr);
}

}
