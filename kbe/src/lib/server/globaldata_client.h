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
#ifndef KBE_GLOBAL_DATA_CLIENT_H
#define KBE_GLOBAL_DATA_CLIENT_H

#include "globaldata_server.h"
#include "common/common.h"
#include "helper/debug_helper.h"
#include "pyscript/map.h"

namespace KBEngine{

class GlobalDataClient : public script::Map
{	
	/** ���໯ ��һЩpy�������������� */
	INSTANCE_SCRIPT_HREADER(GlobalDataClient, script::Map)
		
public:	
	GlobalDataClient(COMPONENT_TYPE componentType, GlobalDataServer::DATA_TYPE dataType);
	~GlobalDataClient();
	
	/** д���� */
	bool write(PyObject* pyKey, PyObject* pyValue);
	
	/** ɾ������ */
	bool del(PyObject* pyKey);
	
	/** ���ݸı�֪ͨ */
	void onDataChanged(PyObject* key, PyObject* value, bool isDelete = false);
	
	/** ���ø�ȫ�����ݿͻ��˵ķ������������ */
	void setServerComponentType(COMPONENT_TYPE ct){ serverComponentType_ = ct; }
	
private:
	COMPONENT_TYPE					serverComponentType_;				// GlobalDataServer���ڷ��������������
	GlobalDataServer::DATA_TYPE 	dataType_;
} ;

}

#endif // KBE_GLOBAL_DATA_CLIENT_H
