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


#ifndef KBE_ENTITYCALL_BASE_H
#define KBE_ENTITYCALL_BASE_H
	
#include "common/common.h"
#include "pyscript/scriptobject.h"
#include "entitydef/common.h"
#include "network/address.h"
	
namespace KBEngine{

class ScriptDefModule;
class RemoteEntityMethod;
class MethodDescription;

namespace Network
{
class Channel;
class Bundle;
}

class EntityCallAbstract : public script::ScriptObject
{
	/** ���໯ ��һЩpy�������������� */
	INSTANCE_SCRIPT_HREADER(EntityCallAbstract, ScriptObject)
public:
	EntityCallAbstract(PyTypeObject* scriptType, 
		const Network::Address* pAddr, 
		COMPONENT_ID componentID, 
		ENTITY_ID eid, 
		uint16 utype, 
		ENTITYCALL_TYPE type);
	
	virtual ~EntityCallAbstract();

	/** 
		��ȡentityID 
	*/
	INLINE ENTITY_ID id() const;

	INLINE void id(int v);

	DECLARE_PY_GET_MOTHOD(pyGetID);

	/** 
		������ID 
	*/
	INLINE COMPONENT_ID componentID(void) const;

	/** 
		���������ID 
	*/
	INLINE void componentID(COMPONENT_ID cid);

	/** 
		���utype 
	*/
	INLINE ENTITY_SCRIPT_UID utype(void) const;

	/** 
		���type 
	*/
	INLINE ENTITYCALL_TYPE type(void) const;

	/** 
		֧��pickler ���� 
	*/
	static PyObject* __py_reduce_ex__(PyObject* self, PyObject* protocol);
	
	virtual Network::Channel* getChannel(void) = 0;

	virtual bool sendCall(Network::Bundle* pBundle);

	virtual void newCall(Network::Bundle& bundle);
	
	const Network::Address& addr() const{ return addr_; }
	void addr(const Network::Address& saddr){ addr_ = saddr; }

	INLINE bool isClient() const;
	INLINE bool isCell() const;
	INLINE bool isCellReal() const;
	INLINE bool isCellViaBase() const;
	INLINE bool isBase() const;
	INLINE bool isBaseReal() const;
	INLINE bool isBaseViaCell() const;
	
	ScriptDefModule* pScriptDefModule();

protected:
	COMPONENT_ID							componentID_;			// Զ�˻��������ID
	Network::Address						addr_;					// Ƶ����ַ
	ENTITYCALL_TYPE							type_;					// ��entityCall������
	ENTITY_ID								id_;					// entityID
	ENTITY_SCRIPT_UID						utype_;					// entity��utype����entities.xml�еĶ���˳��
};

}

#ifdef CODE_INLINE
#include "entitycallabstract.inl"
#endif
#endif // KBE_ENTITYCALL_BASE_H
