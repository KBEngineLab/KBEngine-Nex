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
#include "entity.h"
#include "real_entity_method.h"

namespace KBEngine{

/*
Cellapp 在本模块注册空间相关的组件接口，确保 real、ghost 与客户端广播能力使用同一脚本类型。
Cellapp registers spatial component interfaces here so real, ghost, and client broadcast capabilities share one script type.
*/
SCRIPT_METHOD_DECLARE_BEGIN(EntityComponent)
SCRIPT_METHOD_DECLARE("addTimer", pyAddTimer, METH_VARARGS, 0)
SCRIPT_METHOD_DECLARE("delTimer", pyDelTimer, METH_VARARGS, 0)
SCRIPT_METHOD_DECLARE("clientEntity", pyClientEntity, METH_VARARGS, 0)
SCRIPT_METHOD_DECLARE_END()

SCRIPT_GETSET_DECLARE_BEGIN(EntityComponent)
SCRIPT_GET_DECLARE("ownerID", pyGetOwnerID, 0, 0)
SCRIPT_GET_DECLARE("owner", pyGetOwner, 0, 0)
SCRIPT_GET_DECLARE("name", pyName, 0, 0)
SCRIPT_GET_DECLARE("className", pyGetClassName, 0, 0)
SCRIPT_GET_DECLARE("isDestroyed", pyIsDestroyed, 0, 0)
SCRIPT_GET_DECLARE("base", pyGetBaseEntityCall, 0, 0)
SCRIPT_GET_DECLARE("client", pyGetClientEntityCall, 0, 0)
SCRIPT_GET_DECLARE("allClients", pyGetAllClients, 0, 0)
SCRIPT_GET_DECLARE("otherClients", pyGetOtherClients, 0, 0)
SCRIPT_GETSET_DECLARE_END()
BASE_SCRIPT_INIT(EntityComponent, 0, 0, 0, 0, 0)

PyObject* EntityComponent::onScriptGetAttribute(PyObject* attr)
{
	const char* ccattr = PyUnicode_AsUTF8AndSize(attr, NULL);

	if (ownerID_ > 0)
	{
		Entity* pOwner = static_cast<Entity*>(owner());

		if (pOwner && !pOwner->isReal())
		{
			// Ghost 上的组件方法必须代理到 real 实体，避免在非权威副本执行状态修改。
			// Component methods on a ghost must proxy to the real entity so state changes never execute on a non-authoritative copy.
			MethodDescription* pMethodDescription =
				const_cast<ScriptDefModule*>(pComponentDescrs_)->findCellMethodDescription(ccattr);

			if (pMethodDescription)
			{
				return new RealEntityMethod(pPropertyDescription_, pMethodDescription, pOwner);
			}
		}
	}

	return ScriptObject::onScriptGetAttribute(attr);
}

}
