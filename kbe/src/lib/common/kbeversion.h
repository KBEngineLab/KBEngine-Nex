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

#ifndef KBEVERSION_H
#define KBEVERSION_H

#include "common/platform.h"

namespace KBEngine{
	
// 公开握手版本与目标 Nex SDK 保持一致，确保原生 2.8 客户端无需修改版本字符串即可连接。
// Keep the public handshake version aligned with the target Nex SDK so stock 2.8 clients connect without version-string patches.
#define KBE_VERSION_MAJOR 2
#define KBE_VERSION_MINOR 8
#define KBE_VERSION_PATCH 2


namespace KBEVersion
{
	const std::string & versionString();
	void setScriptVersion(const std::string& ver);
	const std::string & scriptVersionString();
}

}
#endif // KBEVERSION_H
