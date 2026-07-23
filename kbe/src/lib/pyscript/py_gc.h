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

#ifndef KBE_PY_GC_H
#define KBE_PY_GC_H

#include "common/common.h"
#include "scriptobject.h"

namespace KBEngine{ namespace script{

class PyGC
{						
public:	
	static uint32 DEBUG_STATS;
	static uint32 DEBUG_COLLECTABLE;
	static uint32 DEBUG_UNCOLLECTABLE;
	static uint32 DEBUG_SAVEALL;
	static uint32 DEBUG_LEAK;
	
	/** 
		��ʼ��pickler 
	*/
	static bool initialize(void);
	static void finalise(void);
	
	/** 
		ǿ�ƻ�������
	*/
	static void collect(int8 generations = -1);

	/** 
		���õ��Ա�־
	*/
	static void set_debug(uint32 flags);
	
	/**
		���Ӽ���
	*/
	static void incTracing(std::string name);

	/**
		���ټ���
	*/
	static void decTracing(std::string name);

	/**
		debug׷��kbe��װ��py�������
	*/
	static void debugTracing(bool shuttingdown = true);

	/**
		�ű�����
	*/
	static PyObject* __py_debugTracing(PyObject* self, PyObject* args);

private:
	static PyObject* collectMethod_;							// cPicket.dumps����ָ��
	static PyObject* set_debugMethod_;							// cPicket.loads����ָ��

	static bool	isInit;											// �Ƿ��Ѿ�����ʼ��

	static KBEUnordered_map<std::string, int> tracingCountMap_;	// ׷���ض��Ķ��������
} ;

}
}

#endif // KBE_PY_GC_H
