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

#ifndef KBE_COORDINATE_SYSTEM_H
#define KBE_COORDINATE_SYSTEM_H

#include "helper/debug_helper.h"
#include "common/common.h"	

//#define DEBUG_COORDINATE_SYSTEM

namespace KBEngine{

class CoordinateNode;

class CoordinateSystem
{
public:
	CoordinateSystem();
	~CoordinateSystem();

	/**
		向list中插入节点
	*/
	bool insert(CoordinateNode* pNode);

	/**
		在已安装节点附近插入隐藏的辅助节点，不触发节点穿越回调。
		Insert a hidden helper node beside an installed anchor without firing pass callbacks.

		该入口用于 RangeTrigger 的零范围边界。调用者随后仍需通过 update()
		展开真实范围，确保 Enter/Leave 事件只来自实际覆盖的坐标区间。
		This entry is for zero-range RangeTrigger boundaries. The caller must still
		expand the real range through update(), so Enter/Leave events only come from
		the coordinate interval that is actually covered.
	*/
	bool insertNear(CoordinateNode* pNode, CoordinateNode* pAnchorNode);

	/**
		将节点从list中移除
	*/
	bool remove(CoordinateNode* pNode);
	bool removeReal(CoordinateNode* pNode);
	void removeDelNodes();
	void releaseNodes();

	/**
		当某个节点有变动时，需要更新它在list中的
		相关位置等信息
	*/
	void update(CoordinateNode* pNode);

	/**
		移动节点
	*/
	void moveNodeX(CoordinateNode* pNode, float px, CoordinateNode* pCurrNode);
	void moveNodeY(CoordinateNode* pNode, float py, CoordinateNode* pCurrNode);
	void moveNodeZ(CoordinateNode* pNode, float pz, CoordinateNode* pCurrNode);

	static uint64 equalCoordinateCorrectionMoves();
	static uint64 equalCoordinateCallbacksSuppressed();

	INLINE CoordinateNode * pFirstXNode() const;
	INLINE CoordinateNode * pFirstYNode() const;
	INLINE CoordinateNode * pFirstZNode() const;

	INLINE bool isEmpty() const;

	INLINE uint32 size() const;

	static bool hasY;

	INLINE void incUpdating();
	INLINE void decUpdating();

private:
	void moveNodeX(CoordinateNode* pNode, float px, CoordinateNode* pCurrNode,
		bool notifyPassCallbacks);
	void moveNodeY(CoordinateNode* pNode, float py, CoordinateNode* pCurrNode,
		bool notifyPassCallbacks);
	void moveNodeZ(CoordinateNode* pNode, float pz, CoordinateNode* pCurrNode,
		bool notifyPassCallbacks);

	uint32 size_;

	// 链表的首尾指针
	CoordinateNode* first_x_coordinateNode_;
	CoordinateNode* first_y_coordinateNode_;
	CoordinateNode* first_z_coordinateNode_;

	// 延迟删除队列只在 CellApp 主线程访问，连续存储可避免每个节点的 list 堆分配。
	// Deferred-delete queues are single-threaded; contiguous storage avoids one heap node per entry.
	std::vector<CoordinateNode*> dels_;
	size_t dels_count_;

	int updating_;

	std::vector<CoordinateNode*> releases_;
};

}

#ifdef CODE_INLINE
#include "coordinate_system.inl"
#endif
#endif
