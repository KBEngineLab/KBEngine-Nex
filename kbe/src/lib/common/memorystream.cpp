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

#include "memorystream.h"
namespace KBEngine
{
static ObjectPool<MemoryStream> _g_objPool("MemoryStream");
//-------------------------------------------------------------------------------------
ObjectPool<MemoryStream>& MemoryStream::ObjPool()
{
	return _g_objPool;
}

//-------------------------------------------------------------------------------------
MemoryStream* MemoryStream::createPoolObject(const std::string& logPoint)
{
	return _g_objPool.createObject(logPoint);
}

//-------------------------------------------------------------------------------------
void MemoryStream::reclaimPoolObject(MemoryStream* obj)
{
	_g_objPool.reclaimObject(obj);
}

//-------------------------------------------------------------------------------------
void MemoryStream::destroyObjPool()
{
	DEBUG_MSG(fmt::format("MemoryStream::destroyObjPool(): size {}.\n", 
		_g_objPool.size()));

	_g_objPool.destroy();
}

//-------------------------------------------------------------------------------------
MemoryStream::SmartPoolObjectPtr MemoryStream::createSmartPoolObj(const std::string& logPoint)
{
	return SmartPoolObjectPtr(new SmartPoolObject<MemoryStream>(ObjPool().createObject(logPoint), _g_objPool));
}

//-------------------------------------------------------------------------------------
size_t MemoryStream::getPoolObjectBytes()
{
	size_t bytes = sizeof(rpos_) + sizeof(wpos_) + data_.capacity();
	return bytes;
}

//-------------------------------------------------------------------------------------
void MemoryStream::onReclaimObject()
{
	if(data_.capacity() > MAX_RETAINED_CAPACITY)
	{
		// reserve 不能缩容；交换新缓冲才能确定性释放高水位，同时恢复对象池流的默认小容量起点。
		// reserve cannot shrink; swapping with a fresh buffer deterministically releases the high-water allocation and restores the pool stream's small default starting capacity.
		std::vector<uint8> compact;
		compact.reserve(DEFAULT_SIZE);
		data_.swap(compact);
	}

	clear(false);
}

//-------------------------------------------------------------------------------------
MemoryStream::~MemoryStream()
{
	clear(true);
}

//-------------------------------------------------------------------------------------
} 


