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
	�̻߳����壺
	�÷�:
		ThreadMutex tm;
		tm.lockMutex();
		....��ȫ����
		tm.unlockMutex();
		
		��������ThreadGuard��ʹ��
		��һ�����ж��廥�����Ա
		ThreadMutex tm;
		����Ҫ�����ĵط�:
		void XXCLASS::func(void)
		{
			ThreadGuard tg(this->tm);
			����Ĵ��붼�ǰ�ȫ��
			...
		}
*/
#ifndef __THREADMUTEX_H__
#define __THREADMUTEX_H__
	
#include "common/common.h"


namespace KBEngine{ namespace thread{

class ThreadMutexNull
{
public:
	ThreadMutexNull(void)
	{
	}

	virtual ~ThreadMutexNull(void)
	{
	}

	virtual void lockMutex(void)
	{
	}

	virtual void unlockMutex(void)
	{
	}
};

class ThreadMutex : public ThreadMutexNull
{
public:
	ThreadMutex(void)
	{
		THREAD_MUTEX_INIT(mutex_);
	}

	ThreadMutex(const ThreadMutex& v)
	{
		// ���ﲻ������������mutex_�����Ƿǳ�Σ�յ�
		// ����ɶ��THREAD_MUTEX_DELETE
		THREAD_MUTEX_INIT(mutex_);
	}

	virtual ~ThreadMutex(void)
	{ 
		THREAD_MUTEX_DELETE(mutex_);
	}	
	
	virtual void lockMutex(void)
	{
		THREAD_MUTEX_LOCK(mutex_);
	}

	virtual void unlockMutex(void)
	{
		THREAD_MUTEX_UNLOCK(mutex_);
	}

protected:
	THREAD_MUTEX mutex_;
};

}
}
#endif
