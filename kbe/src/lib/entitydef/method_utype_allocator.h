/*
This source file is part of KBEngine
For the latest info, see http://www.kbengine.org/

Copyright (c) 2008-2018 KBEngine.

KBEngine is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/

#ifndef KBE_METHOD_UTYPE_ALLOCATOR_H
#define KBE_METHOD_UTYPE_ALLOCATOR_H

#include "common.h"

#include <bitset>
#include <limits>

namespace KBEngine
{

/**
 * 方法 UType 按通信域独立分配，并允许客户端可见与服务端私有方法从两端增长。
 * Method UTypes are allocated per communication domain, with client-visible and
 * server-private methods growing from opposite ends of the numeric range.
 */
class MethodUTypeAllocator
{
public:
	MethodUTypeAllocator()
	{
		reset();
	}

	void reset()
	{
		used_.reset();
		nextLow_ = 1;
		nextHigh_ = maxUType();
	}

	/**
	 * 显式 UType 可以在不同实体模块中复用；模块内冲突由 ScriptDefModule 检查。
	 * Explicit UTypes may be reused by different entity modules; ScriptDefModule
	 * validates conflicts inside one module.
	 */
	bool reserve(ENTITY_METHOD_UID utype)
	{
		if (utype == 0)
			return false;

		used_.set(static_cast<size_t>(utype));
		return true;
	}

	bool allocateClientVisible(ENTITY_METHOD_UID& utype)
	{
		while (nextLow_ <= maxUType())
		{
			const ENTITY_METHOD_UID candidate = static_cast<ENTITY_METHOD_UID>(nextLow_++);
			if (!used_.test(static_cast<size_t>(candidate)))
			{
				used_.set(static_cast<size_t>(candidate));
				utype = candidate;
				return true;
			}
		}

		return false;
	}

	bool allocateServerPrivate(ENTITY_METHOD_UID& utype)
	{
		while (nextHigh_ > 0)
		{
			const ENTITY_METHOD_UID candidate = static_cast<ENTITY_METHOD_UID>(nextHigh_--);
			if (!used_.test(static_cast<size_t>(candidate)))
			{
				used_.set(static_cast<size_t>(candidate));
				utype = candidate;
				return true;
			}
		}

		return false;
	}

	bool isReserved(ENTITY_METHOD_UID utype) const
	{
		return utype != 0 && used_.test(static_cast<size_t>(utype));
	}

private:
	static constexpr uint32 maxUType()
	{
		return static_cast<uint32>(std::numeric_limits<ENTITY_METHOD_UID>::max());
	}

	std::bitset<static_cast<size_t>(std::numeric_limits<ENTITY_METHOD_UID>::max()) + 1> used_;
	uint32 nextLow_;
	uint32 nextHigh_;
};

}

#endif // KBE_METHOD_UTYPE_ALLOCATOR_H
