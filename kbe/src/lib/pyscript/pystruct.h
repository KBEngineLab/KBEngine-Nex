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

#ifndef KBE_SCRIPT_STRUCT_H
#define KBE_SCRIPT_STRUCT_H

#include "common/common.h"
#include "scriptobject.h"

namespace KBEngine{ namespace script{

class PyStruct
{						
public:	
	/** 
		���� struct.pack
	*/
	static std::string pack(PyObject* fmt, PyObject* args);

	/** 
		���� struct.unpack
	*/
	static PyObject* unpack(PyObject* fmt, PyObject* args);

	/** 
		��ʼ��pickler 
	*/
	static bool initialize(void);
	static void finalise(void);
	
private:
	static PyObject* pack_;									// struct.pack����ָ��
	static PyObject* unpack_;								// struct.unpack����ָ��

	static bool	isInit;										// �Ƿ��Ѿ�����ʼ��
} ;

}
}
#endif // KBE_SCRIPT_STRUCT_H
