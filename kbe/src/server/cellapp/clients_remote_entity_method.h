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


#ifndef KBENGINE_CLIENTS_REMOTE_ENTITY_METHOD_H
#define KBENGINE_CLIENTS_REMOTE_ENTITY_METHOD_H

#include "common/common.h"
#if KBE_PLATFORM == PLATFORM_WIN32
#pragma warning (disable : 4910)
#pragma warning (disable : 4251)
#endif
// common include	
#include "entitydef/datatype.h"
#include "entitydef/datatypes.h"
#include "helper/debug_helper.h"
#include "pyscript/scriptobject.h"	
//#define NDEBUG
// windows include	
#if KBE_PLATFORM == PLATFORM_WIN32
#else
// linux include
#endif

namespace KBEngine{

class ClientsRemoteEntityMethod : public script::ScriptObject
{
	/** ���໯ ��һЩpy�������������� */
	INSTANCE_SCRIPT_HREADER(ClientsRemoteEntityMethod, script::ScriptObject)	
public:	
	ClientsRemoteEntityMethod(MethodDescription* methodDescription, 
		bool otherClients,
		ENTITY_ID id);
	
	virtual ~ClientsRemoteEntityMethod();

	const char* getName(void) const
	{ 
		return methodDescription_->getName(); 
	};

	MethodDescription* getDescription(void) const
	{ 
		return methodDescription_; 
	}

	static PyObject* tp_call(PyObject* self, 
			PyObject* args, PyObject* kwds);

	PyObject* callmethod(PyObject* args, PyObject* kwds);

protected:	
	MethodDescription*		methodDescription_;		// �������������

	bool					otherClients_;			// �Ƿ�ֻ�������ͻ��ˣ� �������Լ�

	ENTITY_ID				id_;					// entityID
};

}

#endif // KBENGINE_CLIENTS_REMOTE_ENTITY_METHOD_H
