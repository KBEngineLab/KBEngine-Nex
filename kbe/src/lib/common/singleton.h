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
	用法:
		class A:public Singleton<A>
		{
		};
		在cpp文件中:
		template<> A* Singleton<A>::singleton_ = 0;
*/
#ifndef KBE_SINGLETON_H
#define KBE_SINGLETON_H

#if _MSC_VER > 1000
#pragma once
#endif // _MSC_VER > 1000

#include "common/platform.h"

namespace KBEngine{
	
template <typename T> 
class Singleton
{
protected:
	// C++17 inline 静态成员：定义位于头文件，任何翻译单元都能看到定义，
	// 避免 clang -Wundefined-var-template（测试单元以 -Werror 编译时尤为关键）。
	// C++17 inline static member: the definition lives in the header so every
	// translation unit sees it, avoiding clang -Wundefined-var-template (critical
	// for test units compiled with -Werror).
	inline static T* singleton_ = nullptr;

public:
	Singleton(void)
	{
		assert(!singleton_);
#if defined(_MSC_VER) && _MSC_VER < 1200	 
		int offset = (int)(T*)1 - (int)(Singleton <T>*)(T*)1;
		singleton_ = (T*)((int)this + offset);
#else
		singleton_ = static_cast< T* >(this);
#endif
	}
	
	
	~Singleton(void){  assert(singleton_);  singleton_ = 0; }
	
	static T& getSingleton(void) { assert(singleton_);  return (*singleton_); }
	static T* getSingletonPtr(void){ return singleton_; }
};

// 历史宏保留以兼容旧源码；C++17 inline 静态成员已提供头文件内定义，无需再显式特化。
// Legacy macro kept for source compatibility; the C++17 inline static member is
// already defined in the header, so no explicit specialization is needed.
#define KBE_SINGLETON_INIT( TYPE )

}
#endif // KBE_SINGLETON_H
