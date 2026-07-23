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

#ifndef KBE_OBJECTPOOL_H
#define KBE_OBJECTPOOL_H

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <iostream>	
#include <map>	
#include <list>	
#include <vector>
#include <queue> 

#include "common/timestamp.h"
#include "thread/threadmutex.h"

namespace KBEngine{

#define OBJECT_POOL_INIT_SIZE			16
#define OBJECT_POOL_INIT_MAX_SIZE		OBJECT_POOL_INIT_SIZE * 1024

// ÿ5���Ӽ��һ������
#define OBJECT_POOL_REDUCING_TIME_OUT	300 * stampsPerSecondD()

// ׷�ٶ�����䴦
#define OBJECTPOOL_POINT fmt::format("{}#{}", __FUNCTION__, __LINE__).c_str() 

template< typename T >
class SmartPoolObject;

class ObjectPoolLogPoint
{
public:
	ObjectPoolLogPoint() :
		count(0)
	{

	}

	int count;
};

/*
	һЩ�����ǳ�Ƶ���ı������� ���磺MemoryStream, Bundle, TCPPacket�ȵ�
	�������ض�ͨ������˷�ֵ��Ч��Ԥ����ǰ������һЩ���󻺴����������õ���ʱ��ֱ�ӴӶ������
	��ȡһ��δ��ʹ�õĶ��󼴿ɡ�
*/
template< typename T, typename THREADMUTEX = KBEngine::thread::ThreadMutexNull >
class ObjectPool
{
public:
	typedef std::list<T*> OBJECTS;

	ObjectPool(std::string name):
		objects_(),
		max_(OBJECT_POOL_INIT_MAX_SIZE),
		isDestroyed_(false),
		pMutex_(new THREADMUTEX()),
		name_(name),
		total_allocs_(0),
		obj_count_(0),
		lastReducingCheckTime_(timestamp()),
		logPoints_()
	{
	}

	ObjectPool(std::string name, unsigned int preAssignVal, size_t max):
		objects_(),
		max_((max == 0 ? 1 : max)),
		isDestroyed_(false),
		pMutex_(new THREADMUTEX()),
		name_(name),
		total_allocs_(0),
		obj_count_(0),
		lastReducingCheckTime_(timestamp()),
		logPoints_()
	{
	}

	~ObjectPool()
	{
		destroy();
		SAFE_RELEASE(pMutex_);
	}	
	
	void destroy()
	{
		pMutex_->lockMutex();

		isDestroyed_ = true;

		typename OBJECTS::iterator iter = objects_.begin();
		for(; iter!=objects_.end(); ++iter)
		{
			(*iter)->isEnabledPoolObject(false);
			if(!(*iter)->destructorPoolObject())
			{
				delete (*iter);
			}
		}
				
		objects_.clear();	
		obj_count_ = 0;
		pMutex_->unlockMutex();
	}

	const OBJECTS& objects(void) const 
	{ 
		return objects_; 
	}

	void pMutex(KBEngine::thread::ThreadMutexNull* pMutex)
	{
		SAFE_RELEASE(pMutex_);
		pMutex_ = pMutex;
	}

	KBEngine::thread::ThreadMutexNull* pMutex()
	{
		return pMutex_;
	}

	void assignObjs(unsigned int preAssignVal = OBJECT_POOL_INIT_SIZE)
	{
		for(unsigned int i=0; i<preAssignVal; ++i)
		{
			T* t = new T();
			t->isEnabledPoolObject(false);
			objects_.push_back(t);
			++total_allocs_;
			++obj_count_;
		}
	}

	/** 
		����һ������ ����������Ѿ������򷵻����еģ�����
		����һ���µġ�
	*/
	T* createObject(const std::string& logPoint)
	{
		pMutex_->lockMutex();

		while(true)
		{
			if(obj_count_ > 0)
			{
				T* t = static_cast<T*>(*objects_.begin());
				objects_.pop_front();
				--obj_count_;
				incLogPoint(logPoint);
				t->poolObjectCreatePoint(logPoint);
				t->onEabledPoolObject();
				t->isEnabledPoolObject(true);
				pMutex_->unlockMutex();
				return t;
			}

			assignObjs();
		}

		pMutex_->unlockMutex();

		return NULL;
	}

	/**
		����һ������
	*/
	void reclaimObject(T* obj)
	{
		pMutex_->lockMutex();
		reclaimObject_(obj);
		pMutex_->unlockMutex();
	}

	/**
		����һ����������
	*/
	void reclaimObject(std::list<T*>& objs)
	{
		pMutex_->lockMutex();

		typename std::list< T* >::iterator iter = objs.begin();
		for(; iter != objs.end(); ++iter)
		{
			reclaimObject_((*iter));
		}
		
		objs.clear();

		pMutex_->unlockMutex();
	}

	/**
		����һ����������
	*/
	void reclaimObject(std::vector< T* >& objs)
	{
		pMutex_->lockMutex();

		typename std::vector< T* >::iterator iter = objs.begin();
		for(; iter != objs.end(); ++iter)
		{
			reclaimObject_((*iter));
		}
		
		objs.clear();

		pMutex_->unlockMutex();
	}

	/**
		����һ����������
	*/
	void reclaimObject(std::queue<T*>& objs)
	{
		pMutex_->lockMutex();

		while(!objs.empty())
		{
			T* t = objs.front();
			objs.pop();
			reclaimObject_(t);
		}

		pMutex_->unlockMutex();
	}

	size_t size(void) const { return obj_count_; }
	
	std::string c_str()
	{
		char buf[1024];

		pMutex_->lockMutex();

		sprintf(buf, "ObjectPool::c_str(): name=%s, objs=%d/%d, isDestroyed=%s.\n", 
			name_.c_str(), (int)obj_count_, (int)max_, (isDestroyed() ? "true" : "false"));

		pMutex_->unlockMutex();

		return buf;
	}

	size_t max() const { return max_; }
	size_t totalAllocs() const { return total_allocs_; }

	bool isDestroyed() const { return isDestroyed_; }

	std::map<std::string, ObjectPoolLogPoint>& logPoints() {
		return logPoints_;
	}

	void incLogPoint(const std::string& logPoint)
	{
		++logPoints_[logPoint].count;
	}

	void decLogPoint(const std::string& logPoint)
	{
		--logPoints_[logPoint].count;
	}

protected:
	/**
		����һ������
	*/
	void reclaimObject_(T* obj)
	{
		if(obj != NULL)
		{
			decLogPoint(obj->poolObjectCreatePoint());

			// ������״̬
			obj->onReclaimObject();
			obj->isEnabledPoolObject(false);
			obj->poolObjectCreatePoint("");

			if(size() >= max_ || isDestroyed_)
			{
				delete obj;
				--total_allocs_;
			}
			else
			{
				objects_.push_back(obj);
				++obj_count_;
			}
		}

		uint64 now_timestamp = timestamp();

		if (obj_count_ <= OBJECT_POOL_INIT_SIZE)
		{
			// С�ڵ�����ˢ�¼��ʱ��
			lastReducingCheckTime_ = now_timestamp;
		}
		else if (now_timestamp - lastReducingCheckTime_ > OBJECT_POOL_REDUCING_TIME_OUT)
		{
			// ��ʱ�����OBJECT_POOL_INIT_SIZEδʹ�õĶ�����ʼ����������
			size_t reducing = std::min(objects_.size(), std::min((size_t)OBJECT_POOL_INIT_SIZE, (size_t)(obj_count_ - OBJECT_POOL_INIT_SIZE)));
			
			//printf("ObjectPool::reclaimObject_(): start reducing..., name=%s, currsize=%d, OBJECT_POOL_INIT_SIZE=%d\n", 
			//	name_.c_str(), (int)objects_.size(), OBJECT_POOL_INIT_SIZE);

			while (reducing-- > 0)
			{
				T* t = static_cast<T*>(*objects_.begin());
				objects_.pop_front();
				delete t;

				--obj_count_;
			}

			//printf("ObjectPool::reclaimObject_(): reducing over, name=%s, currsize=%d\n", 
			//	name_.c_str(), (int)objects_.size());

			lastReducingCheckTime_ = now_timestamp;
		}
	}

protected:
	OBJECTS objects_;

	size_t max_;

	bool isDestroyed_;

	// һЩԭ�����������б�Ҫ��
	// ���磺dbmgr�����߳������log��cellapp�м���navmesh����̻߳ص����µ�log���
	THREADMUTEX* pMutex_;

	std::string name_;

	size_t total_allocs_;

	// Linux�����У�list.size()ʹ�õ���std::distance(begin(), end())��ʽ�����
	// ���������Ӱ�죬���������Լ���size��һ����¼
	size_t obj_count_;

	// ���һ���������ʱ��
	// �������OBJECT_POOL_REDUCING_TIME_OUT����OBJECT_POOL_INIT_SIZE�����������OBJECT_POOL_INIT_SIZE��
	uint64 lastReducingCheckTime_;

	// ��¼�Ĵ���λ����Ϣ������׷��й¶��
	std::map<std::string, ObjectPoolLogPoint> logPoints_;
};

/*
	�ض��� ����ʹ�óصĶ������ʵ�ֻ��չ��ܡ�
*/
class PoolObject
{
public:
	PoolObject() : 
		isEnabledPoolObject_(false)
	{

	}

	virtual ~PoolObject(){}
	virtual void onReclaimObject() = 0;
	virtual void onEabledPoolObject() {
	}

	virtual size_t getPoolObjectBytes()
	{ 
		return 0; 
	}

	/**
		�ض�������ǰ��֪ͨ
		ĳЩ��������ڴ���һЩ����
	*/
	virtual bool destructorPoolObject()
	{
		return false;
	}

	bool isEnabledPoolObject() const
	{
		return isEnabledPoolObject_;
	}

	void isEnabledPoolObject(bool v)
	{
		isEnabledPoolObject_ = v;
	}

	void poolObjectCreatePoint(const std::string& logPoint)
	{
		poolObjectCreatePoint_ = logPoint;
	}

	const std::string& poolObjectCreatePoint() const
	{
		return poolObjectCreatePoint_;
	}

protected:

	// �ض����Ƿ��ڼ���ӳ����Ѿ�ȡ����״̬
	bool isEnabledPoolObject_;

	// ��¼���󴴽���λ��
	std::string poolObjectCreatePoint_;
};

template< typename T >
class SmartObjectPool : public ObjectPool<T>
{
public:
};

template< typename T >
class SmartPoolObject
{
public:
	SmartPoolObject(T* pPoolObject, ObjectPool<T>& objectPool):
	  pPoolObject_(pPoolObject),
	  objectPool_(objectPool)
	{
	}

	~SmartPoolObject()
	{
		onReclaimObject();
	}

	void onReclaimObject()
	{
		if(pPoolObject_ != NULL)
		{
			objectPool_.reclaimObject(pPoolObject_);
			pPoolObject_ = NULL;
		}
	}

	T* get()
	{
		return pPoolObject_;
	}

	T* operator->()
	{
		return pPoolObject_;
	}

	T& operator*()
	{
		return *pPoolObject_;
	}

private:
	T* pPoolObject_;
	ObjectPool<T>& objectPool_;
};


#define NEW_POOL_OBJECT(TYPE) TYPE::createPoolObject(OBJECTPOOL_POINT);


}
#endif
