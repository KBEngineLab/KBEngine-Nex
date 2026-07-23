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

	
/*
	CallbackMgr( �ص������� )
		����һЩ�ص����������첽�ģ� ����ͨ��һ������������Щ�ص����������� �����ⷵ��һ��
		��ʶ�ûص���Ψһid�� �ⲿ����ͨ����id����������ص���
		
	�÷�:
	typedef CallbackMgr<std::tr1::function<void(Entity*, int64, bool)>> CALLBACK_MGR;
	CALLBACK_MGR callbackMgr;
	void xxx(Entity*, int64, bool){}
	CALLBACK_ID callbackID = callbackMgr.save(&xxx); // ����ʹ��bind����һ�����Ա����
*/

#ifndef KBE_CALLBACKMGR_H
#define KBE_CALLBACKMGR_H
	
#include "Python.h"
#include "idallocate.h"
#include "serverconfig.h"
#include "helper/debug_helper.h"
#include "common/common.h"
#include "common/memorystream.h"
#include "common/timer.h"
#include "pyscript/pyobject_pointer.h"
#include "pyscript/pickler.h"
	
namespace KBEngine{

template<typename T>
class CallbackMgr
{
public:
	typedef std::map< CALLBACK_ID, std::pair< T, uint64 > > CALLBACKS;

	CallbackMgr():
	cbMap_(),
	idAlloc_(),
	lastTimestamp_(0)
	{
	}

	~CallbackMgr()
	{
		finalise();
	}	
	
	void finalise()
	{
		cbMap_.clear();
	}


	void addToStream(KBEngine::MemoryStream& s);

	void createFromStream(KBEngine::MemoryStream& s);

	/** 
		�����������һ���ص� 
	*/
	CALLBACK_ID save(T callback, uint64 timeout = 0/*secs*/)
	{
		if(timeout == 0)
			timeout = uint64(ServerConfig::getSingleton().callback_timeout_);

		CALLBACK_ID cbID = idAlloc_.alloc();
		cbMap_.insert(typename CALLBACKS::value_type(cbID, 
			std::pair< T, uint64 >(callback, timestamp() + (timeout * stampsPerSecond()))));

		tick();
		return cbID;
	}
	
	/** 
		ͨ��callbackIDȡ�߻ص� 
	*/
	T take(CALLBACK_ID cbID)
	{
		typename CALLBACKS::iterator itr = cbMap_.find(cbID);
		if(itr != cbMap_.end()){
			T t = itr->second.first;
			idAlloc_.reclaim(itr->first);
			cbMap_.erase(itr);
			return t;
		}
		
		tick();
		return NULL;
	}

	/**
		tick
	*/
	void tick()
	{
		if(timestamp() - lastTimestamp_ < (ServerConfig::getSingleton().callback_timeout_ * stampsPerSecond()))
			return;

		lastTimestamp_ = timestamp(); 
		typename CALLBACKS::iterator iter = cbMap_.begin();
		for(; iter!= cbMap_.end(); )
		{
			if(lastTimestamp_ > iter->second.second)
			{
				if(processTimeout(iter->first, iter->second.first))
				{
					idAlloc_.reclaim(iter->first);
					cbMap_.erase(iter++);
					continue;
				}
			}

			++iter;
		}
	}

	/**
		��ʱ��callback
	*/
	bool processTimeout(CALLBACK_ID cbID, T callback)
	{
		INFO_MSG(fmt::format("CallbackMgr::processTimeout: {} timeout!\n", cbID));
		return true;
	}

protected:
	CALLBACKS cbMap_;									// ���еĻص����洢�����map��
	IDAllocate<CALLBACK_ID> idAlloc_;					// �ص���id������
	uint64 lastTimestamp_;
};

template<>
inline void CallbackMgr<PyObjectPtr>::addToStream(KBEngine::MemoryStream& s)
{
	uint32 size = (uint32)cbMap_.size();

	s << idAlloc_.lastID() << size;

	CALLBACKS::iterator iter = cbMap_.begin();
	for(; iter != cbMap_.end(); ++iter)
	{
		s << iter->first;
		s.appendBlob(script::Pickler::pickle(iter->second.first.get()));
		s << iter->second.second;
	}
}

template<>
inline void CallbackMgr<PyObjectPtr>::createFromStream(KBEngine::MemoryStream& s)
{
	CALLBACK_ID v;
	s >> v;

	idAlloc_.lastID(v);

	uint32 size;
	s >> size;

	for(uint32 i=0; i<size; ++i)
	{
		CALLBACK_ID cbID;
		s >> cbID;

		std::string data;
		s.readBlob(data);

		PyObject* pyCallback = NULL;
		
		if(data.size() > 0)
			pyCallback = script::Pickler::unpickle(data);
		
		uint64 timeout;
		s >> timeout;

		if(pyCallback == NULL || cbID == 0)
		{
			ERROR_MSG(fmt::format("CallbackMgr::createFromStream: pyCallback({}) error!\n", cbID));
			continue;
		}

		cbMap_.insert(CallbackMgr<PyObjectPtr>::CALLBACKS::value_type(cbID, 
			std::pair< PyObjectPtr, uint64 >(pyCallback, timeout)));

		Py_DECREF(pyCallback);
	}
}

template<>
inline void CallbackMgr<PyObject*>::finalise()
{
	std::map< CALLBACK_ID, std::pair< PyObject*, uint64 > >::iterator iter = cbMap_.begin();
	for(; iter!= cbMap_.end(); ++iter)
	{
		Py_DECREF(iter->second.first);
	}

	cbMap_.clear();
}	

template<>
inline bool CallbackMgr<PyObject*>::processTimeout(CALLBACK_ID cbID, PyObject* callback)
{
	PyObject* pystr = PyObject_Str(callback);
	const char* ccattr = PyUnicode_AsUTF8AndSize(callback, NULL);
	Py_DECREF(pystr);

	INFO_MSG(fmt::format("CallbackMgr::processTimeout: callbackID:{}, callback({}) timeout!\n", cbID,
		ccattr));

	Py_DECREF(callback);
	return true;
}

typedef CallbackMgr<PyObjectPtr> PY_CALLBACKMGR;

}

#endif // KBE_CALLBACKMGR_H
