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

// Stale local EntityCalls are lifecycle diagnostics rather than transport failures.
// 本地 EntityCall 指向已销毁实体属于生命周期诊断，不应与远端网络故障混为一谈。
void recordStaleLocalEntityCallResolution();
uint64 staleLocalEntityCallResolutionCount();
uint64 staleLocalEntityCallSendAttemptCount();
uint64 invalidRemoteEntityCallSendAttemptCount();

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
	/** 子类化 将一些py操作填充进派生类 */
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
		获取entityID 
	*/
	INLINE ENTITY_ID id() const;

	INLINE void id(int v);

	DECLARE_PY_GET_MOTHOD(pyGetID);

	/** 
		获得组件ID 
	*/
	INLINE COMPONENT_ID componentID(void) const;

	/** 
		设置组件的ID 
	*/
	INLINE void componentID(COMPONENT_ID cid);

	/** 
		获得utype 
	*/
	INLINE ENTITY_SCRIPT_UID utype(void) const;

	/** 
		获得type 
	*/
	INLINE ENTITYCALL_TYPE type(void) const;

	/** 
		支持pickler 方法 
	*/
	static PyObject* __py_reduce_ex__(PyObject* self, PyObject* protocol);
	
	virtual Network::Channel* getChannel(void) = 0;

	virtual bool sendCall(Network::Bundle* pBundle);

	virtual void newCall(Network::Bundle& bundle);

	// 新虚函数追加在既有槽位之后，避免增量构建时改变旧 newCall 的虚表索引。
	// Appending the new virtual after existing slots preserves newCall's vtable index during incremental builds.
	// 仅构造目标消息和实体寻址头，供已经携带父组件 UID 的转发数据使用。
	// Builds only the destination message and entity address header for forwarded data that already carries a parent component UID.
	virtual void newCall_(Network::Bundle& bundle);
	
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
	COMPONENT_ID							componentID_;			// 远端机器组件的ID
	Network::Address						addr_;					// 频道地址
	ENTITYCALL_TYPE							type_;					// 该entityCall的类型
	ENTITY_ID								id_;					// entityID
	ENTITY_SCRIPT_UID						utype_;					// entity的utype按照entities.xml中的定义顺序
};

}

#ifdef CODE_INLINE
#include "entitycallabstract.inl"
#endif
#endif // KBE_ENTITYCALL_BASE_H
