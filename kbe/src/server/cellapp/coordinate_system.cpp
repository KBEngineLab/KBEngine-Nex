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
#include "coordinate_node.h"
#include "coordinate_system.h"
#ifndef KBE_COORDINATE_SYSTEM_DISABLE_PROFILE
#include "profile.h"
#endif

#ifndef CODE_INLINE
#include "coordinate_system.inl"
#endif

namespace KBEngine{	

bool CoordinateSystem::hasY = false;

namespace
{
uint64 g_equalCoordinateCorrectionMoves = 0;
uint64 g_equalCoordinateCallbacksSuppressed = 0;

// 等坐标纠正只恢复既有链表排序；首次穿越已经发布过关系变化，因此纠正阶段
// 再次通知只会重复 AOI/Python 工作。计数仅发生在该稀有分支，不污染普通移动热路径。
// Equal-coordinate correction only restores the established list ordering. The
// first pass has already published the relationship change, so notifying again
// would duplicate AOI/Python work. Counters run only on this uncommon branch and
// do not add writes to ordinary movement.
void recordEqualCoordinateCorrection(CoordinateNode* pNode, CoordinateNode* pCurrNode)
{
	++g_equalCoordinateCorrectionMoves;
	g_equalCoordinateCallbacksSuppressed +=
		(pNode->hasFlags(COORDINATE_NODE_FLAG_HIDE_OR_REMOVED) ? 0 : 1) +
		(pCurrNode->hasFlags(COORDINATE_NODE_FLAG_HIDE_OR_REMOVED) ? 0 : 1);
}
}

//-------------------------------------------------------------------------------------
CoordinateSystem::CoordinateSystem():
size_(0),
first_x_coordinateNode_(NULL),
first_y_coordinateNode_(NULL),
first_z_coordinateNode_(NULL),
dels_(),
dels_count_(0),
updating_(0),
releases_()
{
}

//-------------------------------------------------------------------------------------
CoordinateSystem::~CoordinateSystem()
{
	dels_.clear();
	dels_count_ = 0;

	if(first_x_coordinateNode_)
	{
		CoordinateNode* pNode = first_x_coordinateNode_;
		while(pNode != NULL)
		{
			CoordinateNode* pNextNode = pNode->pNextX();

			if (pNextNode)
				pNextNode->pPrevX(NULL);

			pNode->pCoordinateSystem(NULL);
			pNode->pPrevX(NULL);
			pNode->pNextX(NULL);
			pNode->pPrevY(NULL);
			pNode->pNextY(NULL);
			pNode->pPrevZ(NULL);
			pNode->pNextZ(NULL);

			delete pNode;

			pNode = pNextNode;
		}
		
		// 上面已经销毁过了
		first_x_coordinateNode_ = NULL;
		first_y_coordinateNode_ = NULL;
		first_z_coordinateNode_ = NULL;
	}

	releaseNodes();
}

//-------------------------------------------------------------------------------------
bool CoordinateSystem::insert(CoordinateNode* pNode)
{
	// 如果链表是空的, 初始第一个和最后一个xz节点为该节点
	if(isEmpty())
	{
		first_x_coordinateNode_ = pNode;

		if(CoordinateSystem::hasY)
			first_y_coordinateNode_ = pNode;

		first_z_coordinateNode_ = pNode;

		pNode->pPrevX(NULL);
		pNode->pNextX(NULL);
		pNode->pPrevY(NULL);
		pNode->pNextY(NULL);
		pNode->pPrevZ(NULL);
		pNode->pNextZ(NULL);
		pNode->x(pNode->xx());
		pNode->y(pNode->yy());
		pNode->z(pNode->zz());
		pNode->pCoordinateSystem(this);

		size_ = 1;
		
		// 只有一个节点不需要更新
		// update(pNode);
		pNode->resetOld();
		return true;
	}

	pNode->old_xx(-FLT_MAX);
	pNode->old_yy(-FLT_MAX);
	pNode->old_zz(-FLT_MAX);

	pNode->x(first_x_coordinateNode_->x());
	first_x_coordinateNode_->pPrevX(pNode);
	pNode->pNextX(first_x_coordinateNode_);
	first_x_coordinateNode_ = pNode;

	if(CoordinateSystem::hasY)
	{
		pNode->y(first_y_coordinateNode_->y());
		first_y_coordinateNode_->pPrevY(pNode);
		pNode->pNextY(first_y_coordinateNode_);
		first_y_coordinateNode_ = pNode;
	}
	
	pNode->z(first_z_coordinateNode_->z());
	first_z_coordinateNode_->pPrevZ(pNode);
	pNode->pNextZ(first_z_coordinateNode_);
	first_z_coordinateNode_ = pNode;

	pNode->pCoordinateSystem(this);
	++size_;

	update(pNode);
	return true;
}

//-------------------------------------------------------------------------------------
bool CoordinateSystem::insertNear(CoordinateNode* pNode, CoordinateNode* pAnchorNode)
{
	if (!pNode || !pAnchorNode || pNode == pAnchorNode ||
		!pNode->hasFlags(COORDINATE_NODE_FLAG_HIDE) ||
		pNode->pCoordinateSystem() || pAnchorNode->pCoordinateSystem() != this)
	{
		return false;
	}

	// RangeTrigger 边界以零范围创建，因此可以直接挂在 origin 后方。这样避免从
	// 全局链表头移动到 origin 时扫描无关节点；真实范围随后仍由 update() 展开，
	// 穿越回调的方向、顺序和可重入销毁语义不变。
	// RangeTrigger boundaries are created with zero range and can therefore be
	// linked immediately after the origin. This avoids scanning unrelated nodes
	// from the global head; update() still expands the real range afterwards and
	// preserves pass direction, callback order, and re-entrant destruction semantics.
	CoordinateNode* pNextNode = pAnchorNode->pNextX();
	pNode->pPrevX(pAnchorNode);
	pNode->pNextX(pNextNode);
	pAnchorNode->pNextX(pNode);
	if (pNextNode)
		pNextNode->pPrevX(pNode);
	pNode->x(pAnchorNode->x());

	if (CoordinateSystem::hasY)
	{
		pNextNode = pAnchorNode->pNextY();
		pNode->pPrevY(pAnchorNode);
		pNode->pNextY(pNextNode);
		pAnchorNode->pNextY(pNode);
		if (pNextNode)
			pNextNode->pPrevY(pNode);
		pNode->y(pAnchorNode->y());
	}
	else
	{
		pNode->pPrevY(NULL);
		pNode->pNextY(NULL);
		pNode->y(pAnchorNode->y());
	}

	pNextNode = pAnchorNode->pNextZ();
	pNode->pPrevZ(pAnchorNode);
	pNode->pNextZ(pNextNode);
	pAnchorNode->pNextZ(pNode);
	if (pNextNode)
		pNextNode->pPrevZ(pNode);
	pNode->z(pAnchorNode->z());

	// old_* 必须反映当前挂接位置，而不是辅助节点未来的目标位置。
	// old_* must describe the linked position, not the helper's future target.
	pNode->old_xx(pAnchorNode->x());
	pNode->old_yy(pAnchorNode->y());
	pNode->old_zz(pAnchorNode->z());
	pNode->pCoordinateSystem(this);
	++size_;
	return true;
}

//-------------------------------------------------------------------------------------
bool CoordinateSystem::remove(CoordinateNode* pNode)
{
	pNode->addFlags(COORDINATE_NODE_FLAG_REMOVING);
	pNode->onRemove();
	update(pNode);
	
	pNode->addFlags(COORDINATE_NODE_FLAG_REMOVED);

	// 由于在update过程中可能会因为多级update的进行导致COORDINATE_NODE_FLAG_PENDING标志被取消，因此此处并不能很好的判断
	// 除非实现了标记的计数器，这里强制所有的行为都放入dels_， 由releaseNodes在space中进行调用统一释放
	if(true /*pNode->hasFlags(COORDINATE_NODE_FLAG_PENDING)*/)
	{
		std::list<CoordinateNode*>::iterator iter = std::find(dels_.begin(), dels_.end(), pNode);
		if(iter == dels_.end())
		{
			dels_.push_back(pNode);
			++dels_count_;
		}
	}
	else
	{
		removeReal(pNode);
	}

	return true;
}

//-------------------------------------------------------------------------------------
void CoordinateSystem::removeDelNodes()
{
	if(dels_count_ == 0)
		return;

	std::list<CoordinateNode*>::iterator iter = dels_.begin();
	for(; iter != dels_.end(); ++iter)
	{
		removeReal((*iter));
	}

	dels_.clear();
	dels_count_ = 0;
}

//-------------------------------------------------------------------------------------
void CoordinateSystem::releaseNodes()
{
	removeDelNodes();

	std::list<CoordinateNode*>::iterator iter = releases_.begin();
	for (; iter != releases_.end(); ++iter)
	{
		delete (*iter);
	}

	releases_.clear();
}

//-------------------------------------------------------------------------------------
bool CoordinateSystem::removeReal(CoordinateNode* pNode)
{
	if(pNode->pCoordinateSystem() == NULL)
	{
		return true;
	}

	// 如果是第一个节点
	if(first_x_coordinateNode_ == pNode)
	{
		first_x_coordinateNode_ = first_x_coordinateNode_->pNextX();

		if(first_x_coordinateNode_)
		{
			first_x_coordinateNode_->pPrevX(NULL);
		}
	}
	else
	{
		pNode->pPrevX()->pNextX(pNode->pNextX());

		if(pNode->pNextX())
			pNode->pNextX()->pPrevX(pNode->pPrevX());
	}

	if(CoordinateSystem::hasY)
	{
		// 如果是第一个节点
		if(first_y_coordinateNode_ == pNode)
		{
			first_y_coordinateNode_ = first_y_coordinateNode_->pNextY();

			if(first_y_coordinateNode_)
			{
				first_y_coordinateNode_->pPrevY(NULL);
			}
		}
		else
		{
			pNode->pPrevY()->pNextY(pNode->pNextY());

			if(pNode->pNextY())
				pNode->pNextY()->pPrevY(pNode->pPrevY());
		}
	}

	// 如果是第一个节点
	if(first_z_coordinateNode_ == pNode)
	{
		first_z_coordinateNode_ = first_z_coordinateNode_->pNextZ();

		if(first_z_coordinateNode_)
		{
			first_z_coordinateNode_->pPrevZ(NULL);
		}
	}
	else
	{
		pNode->pPrevZ()->pNextZ(pNode->pNextZ());

		if(pNode->pNextZ())
			pNode->pNextZ()->pPrevZ(pNode->pPrevZ());
	}

	pNode->pPrevX(NULL);
	pNode->pNextX(NULL);
	pNode->pPrevY(NULL);
	pNode->pNextY(NULL);
	pNode->pPrevZ(NULL);
	pNode->pNextZ(NULL);
	pNode->pCoordinateSystem(NULL);
	
	releases_.push_back(pNode);

	--size_;
	return true;
}

//-------------------------------------------------------------------------------------
void CoordinateSystem::moveNodeX(CoordinateNode* pNode, float px, CoordinateNode* pCurrNode)
{
	moveNodeX(pNode, px, pCurrNode, true);
}

//-------------------------------------------------------------------------------------
void CoordinateSystem::moveNodeX(CoordinateNode* pNode, float px, CoordinateNode* pCurrNode,
	bool notifyPassCallbacks)
{
	if (pCurrNode != NULL)
	{
		pNode->x(pCurrNode->x());

#ifdef DEBUG_COORDINATE_SYSTEM
		DEBUG_MSG(fmt::format("CoordinateSystem::update start: [{}X] ({}), pCurrNode=>({})\n",
			(pNode->pPrevX() == pCurrNode ? "-" : "+"), pNode->c_str(), pCurrNode->c_str()));
#endif

		if (pNode->pPrevX() == pCurrNode)
		{
			KBE_ASSERT(pCurrNode->x() >= px);

			CoordinateNode* pPreNode = pCurrNode->pPrevX();
			pCurrNode->pPrevX(pNode);
			if (pPreNode)
			{
				pPreNode->pNextX(pNode);
				if (pNode == first_x_coordinateNode_ && pNode->pNextX())
					first_x_coordinateNode_ = pNode->pNextX();
			}
			else
			{
				first_x_coordinateNode_ = pNode;
			}

			if (pNode->pPrevX())
				pNode->pPrevX()->pNextX(pNode->pNextX());

			if (pNode->pNextX())
				pNode->pNextX()->pPrevX(pNode->pPrevX());

			pNode->pPrevX(pPreNode);
			pNode->pNextX(pCurrNode);
		}
		else
		{
			KBE_ASSERT(pCurrNode->x() <= px);

			CoordinateNode* pNextNode = pCurrNode->pNextX();
			if (pNextNode != pNode)
			{
				pCurrNode->pNextX(pNode);
				if (pNextNode)
					pNextNode->pPrevX(pNode);

				if (pNode->pPrevX())
					pNode->pPrevX()->pNextX(pNode->pNextX());

				if (pNode->pNextX())
				{
					pNode->pNextX()->pPrevX(pNode->pPrevX());

					if (pNode == first_x_coordinateNode_)
						first_x_coordinateNode_ = pNode->pNextX();
				}

				pNode->pPrevX(pCurrNode);
				pNode->pNextX(pNextNode);
			}
		}

		if (notifyPassCallbacks && !pNode->hasFlags(COORDINATE_NODE_FLAG_HIDE_OR_REMOVED))
		{
#ifdef DEBUG_COORDINATE_SYSTEM
			DEBUG_MSG(fmt::format("CoordinateSystem::update1: [{}X] ({}), passNode=>({})\n",
				(pNode->pPrevX() == pCurrNode ? "-" : "+"), pNode->c_str(), pCurrNode->c_str()));
#endif

			pCurrNode->onNodePassX(pNode, true);
		}

		if (notifyPassCallbacks && !pCurrNode->hasFlags(COORDINATE_NODE_FLAG_HIDE_OR_REMOVED))
		{
#ifdef DEBUG_COORDINATE_SYSTEM
			DEBUG_MSG(fmt::format("CoordinateSystem::update2: [{}X] ({}), passNode=>({})\n",
				(pNode->pPrevX() == pCurrNode ? "-" : "+"), pNode->c_str(), pCurrNode->c_str()));
#endif

			pNode->onNodePassX(pCurrNode, false);
		}

		if (!notifyPassCallbacks)
			recordEqualCoordinateCorrection(pNode, pCurrNode);

#ifdef DEBUG_COORDINATE_SYSTEM
		DEBUG_MSG(fmt::format("CoordinateSystem::update end: [{}X] ({}), pCurrNode=>({})\n",
			(pNode->pPrevX() == pCurrNode ? "-" : "+"), pNode->c_str(), pCurrNode->c_str()));
#endif
	}
}

//-------------------------------------------------------------------------------------
void CoordinateSystem::moveNodeY(CoordinateNode* pNode, float py, CoordinateNode* pCurrNode)
{
	moveNodeY(pNode, py, pCurrNode, true);
}

//-------------------------------------------------------------------------------------
void CoordinateSystem::moveNodeY(CoordinateNode* pNode, float py, CoordinateNode* pCurrNode,
	bool notifyPassCallbacks)
{
	if (pCurrNode != NULL)
	{
		pNode->y(pCurrNode->y());

#ifdef DEBUG_COORDINATE_SYSTEM
		DEBUG_MSG(fmt::format("CoordinateSystem::update start: [{}Y] ({}), pCurrNode=>({})\n",
			(pNode->pPrevY() == pCurrNode ? "-" : "+"), pNode->c_str(), pCurrNode->c_str()));
#endif

		if (pNode->pPrevY() == pCurrNode)
		{
			KBE_ASSERT(pCurrNode->y() >= py);

			CoordinateNode* pPreNode = pCurrNode->pPrevY();
			pCurrNode->pPrevY(pNode);
			if (pPreNode)
			{
				pPreNode->pNextY(pNode);
				if (pNode == first_y_coordinateNode_ && pNode->pNextY())
					first_y_coordinateNode_ = pNode->pNextY();
			}
			else
			{
				first_y_coordinateNode_ = pNode;
			}

			if (pNode->pPrevY())
				pNode->pPrevY()->pNextY(pNode->pNextY());

			if (pNode->pNextY())
				pNode->pNextY()->pPrevY(pNode->pPrevY());

			pNode->pPrevY(pPreNode);
			pNode->pNextY(pCurrNode);
		}
		else
		{
			KBE_ASSERT(pCurrNode->y() <= py);

			CoordinateNode* pNextNode = pCurrNode->pNextY();
			if (pNextNode != pNode)
			{
				pCurrNode->pNextY(pNode);
				if (pNextNode)
					pNextNode->pPrevY(pNode);

				if (pNode->pPrevY())
					pNode->pPrevY()->pNextY(pNode->pNextY());

				if (pNode->pNextY())
				{
					pNode->pNextY()->pPrevY(pNode->pPrevY());

					if (pNode == first_y_coordinateNode_)
						first_y_coordinateNode_ = pNode->pNextY();
				}

				pNode->pPrevY(pCurrNode);
				pNode->pNextY(pNextNode);
			}
		}

		if (notifyPassCallbacks && !pNode->hasFlags(COORDINATE_NODE_FLAG_HIDE_OR_REMOVED))
		{
#ifdef DEBUG_COORDINATE_SYSTEM
			DEBUG_MSG(fmt::format("CoordinateSystem::update1: [{}Y] ({}), passNode=>({})\n",
				(pNode->pPrevY() == pCurrNode ? "-" : "+"), pNode->c_str(), pCurrNode->c_str()));
#endif

			pCurrNode->onNodePassY(pNode, true);
		}

		if (notifyPassCallbacks && !pCurrNode->hasFlags(COORDINATE_NODE_FLAG_HIDE_OR_REMOVED))
		{
#ifdef DEBUG_COORDINATE_SYSTEM
			DEBUG_MSG(fmt::format("CoordinateSystem::update2: [{}Y] ({}), passNode=>({})\n",
				(pNode->pPrevY() == pCurrNode ? "-" : "+"), pNode->c_str(), pCurrNode->c_str()));
#endif

			pNode->onNodePassY(pCurrNode, false);
		}

		if (!notifyPassCallbacks)
			recordEqualCoordinateCorrection(pNode, pCurrNode);

#ifdef DEBUG_COORDINATE_SYSTEM
		DEBUG_MSG(fmt::format("CoordinateSystem::update end: [{}Y] ({}), pCurrNode=>({})\n",
			(pNode->pPrevY() == pCurrNode ? "-" : "+"), pNode->c_str(), pCurrNode->c_str()));
#endif
	}
}

//-------------------------------------------------------------------------------------
void CoordinateSystem::moveNodeZ(CoordinateNode* pNode, float pz, CoordinateNode* pCurrNode)
{
	moveNodeZ(pNode, pz, pCurrNode, true);
}

//-------------------------------------------------------------------------------------
void CoordinateSystem::moveNodeZ(CoordinateNode* pNode, float pz, CoordinateNode* pCurrNode,
	bool notifyPassCallbacks)
{
	if (pCurrNode != NULL)
	{
		pNode->z(pCurrNode->z());

#ifdef DEBUG_COORDINATE_SYSTEM
		DEBUG_MSG(fmt::format("CoordinateSystem::update start: [{}Z] ({}), pCurrNode=>({})\n",
			(pNode->pPrevZ() == pCurrNode ? "-" : "+"), pNode->c_str(), pCurrNode->c_str()));
#endif

		if (pNode->pPrevZ() == pCurrNode)
		{
			KBE_ASSERT(pCurrNode->z() >= pz);

			CoordinateNode* pPreNode = pCurrNode->pPrevZ();
			pCurrNode->pPrevZ(pNode);
			if (pPreNode)
			{
				pPreNode->pNextZ(pNode);
				if (pNode == first_z_coordinateNode_ && pNode->pNextZ())
					first_z_coordinateNode_ = pNode->pNextZ();
			}
			else
			{
				first_z_coordinateNode_ = pNode;
			}

			if (pNode->pPrevZ())
				pNode->pPrevZ()->pNextZ(pNode->pNextZ());

			if (pNode->pNextZ())
				pNode->pNextZ()->pPrevZ(pNode->pPrevZ());

			pNode->pPrevZ(pPreNode);
			pNode->pNextZ(pCurrNode);
		}
		else
		{
			KBE_ASSERT(pCurrNode->z() <= pz);

			CoordinateNode* pNextNode = pCurrNode->pNextZ();
			if (pNextNode != pNode)
			{
				pCurrNode->pNextZ(pNode);
				if (pNextNode)
					pNextNode->pPrevZ(pNode);

				if (pNode->pPrevZ())
					pNode->pPrevZ()->pNextZ(pNode->pNextZ());

				if (pNode->pNextZ())
				{
					pNode->pNextZ()->pPrevZ(pNode->pPrevZ());

					if (pNode == first_z_coordinateNode_)
						first_z_coordinateNode_ = pNode->pNextZ();
				}

				pNode->pPrevZ(pCurrNode);
				pNode->pNextZ(pNextNode);
			}
		}

		if (notifyPassCallbacks && !pNode->hasFlags(COORDINATE_NODE_FLAG_HIDE_OR_REMOVED))
		{
#ifdef DEBUG_COORDINATE_SYSTEM
			DEBUG_MSG(fmt::format("CoordinateSystem::update1: [{}Z] ({}), passNode=>({})\n",
				(pNode->pPrevZ() == pCurrNode ? "-" : "+"), pNode->c_str(), pCurrNode->c_str()));
#endif

			pCurrNode->onNodePassZ(pNode, true);
		}

		if (notifyPassCallbacks && !pCurrNode->hasFlags(COORDINATE_NODE_FLAG_HIDE_OR_REMOVED))
		{
#ifdef DEBUG_COORDINATE_SYSTEM
			DEBUG_MSG(fmt::format("CoordinateSystem::update2: [{}Z] ({}), passNode=>({})\n",
				(pNode->pPrevZ() == pCurrNode ? "-" : "+"), pNode->c_str(), pCurrNode->c_str()));
#endif

			pNode->onNodePassZ(pCurrNode, false);
		}

		if (!notifyPassCallbacks)
			recordEqualCoordinateCorrection(pNode, pCurrNode);

#ifdef DEBUG_COORDINATE_SYSTEM
		DEBUG_MSG(fmt::format("CoordinateSystem::update end: [{}Z] ({}), pCurrNode=>({})\n",
			(pNode->pPrevZ() == pCurrNode ? "-" : "+"), pNode->c_str(), pCurrNode->c_str()));
#endif
	}
}

//-------------------------------------------------------------------------------------
void CoordinateSystem::update(CoordinateNode* pNode)
{
#ifndef KBE_COORDINATE_SYSTEM_DISABLE_PROFILE
	AUTO_SCOPED_PROFILE("coordinateSystemUpdates");
#endif

#ifdef DEBUG_COORDINATE_SYSTEM
	DEBUG_MSG(fmt::format("CoordinateSystem::update enter:[{:p}]:  ({}  {}  {})\n", (void*)pNode, pNode->xx(), pNode->yy(), pNode->zz()));
#endif

	// 没有计数器支持，这个标记很可能中途被update子分支取消，因此没有意义
	//pNode->addFlags(COORDINATE_NODE_FLAG_PENDING);

	++updating_;

	if (pNode->xx() != pNode->old_xx())
	{
		CoordinateNode* pEqualCorrectionEnd = NULL;
		CoordinateNode* pCurrNode = pNode->pPrevX();
		while (pCurrNode && pCurrNode != pNode && pCurrNode->x() > pNode->xx())
		{
			moveNodeX(pNode, pNode->xx(), pCurrNode);
			pCurrNode = pNode->pPrevX();
		}
		while (pCurrNode && pCurrNode != pNode && pCurrNode->x() == pNode->xx() &&
			!pCurrNode->hasFlags(COORDINATE_NODE_FLAG_NEGATIVE_BOUNDARY))
		{
			if (!pEqualCorrectionEnd)
				pEqualCorrectionEnd = pCurrNode;
			moveNodeX(pNode, pNode->xx(), pCurrNode);
			pCurrNode = pNode->pPrevX();
		}

		pCurrNode = pNode->pNextX();
		while (pCurrNode && pCurrNode != pNode && pCurrNode->x() < pNode->xx())
		{
			moveNodeX(pNode, pNode->xx(), pCurrNode);
			pCurrNode = pNode->pNextX();
		}
		while (pCurrNode && pCurrNode != pNode && pCurrNode->x() == pNode->xx() &&
			!pCurrNode->hasFlags(COORDINATE_NODE_FLAG_POSITIVE_BOUNDARY))
		{
			const bool isEqualCorrection = pEqualCorrectionEnd != NULL;
			moveNodeX(pNode, pNode->xx(), pCurrNode, !isEqualCorrection);
			if (pCurrNode == pEqualCorrectionEnd)
				pEqualCorrectionEnd = NULL;
			pCurrNode = pNode->pNextX();
		}

		pNode->x(pNode->xx());
	}

	if (CoordinateSystem::hasY && pNode->yy() != pNode->old_yy())
	{
		CoordinateNode* pEqualCorrectionEnd = NULL;
		CoordinateNode* pCurrNode = pNode->pPrevY();
		while (pCurrNode && pCurrNode != pNode && pCurrNode->y() > pNode->yy())
		{
			moveNodeY(pNode, pNode->yy(), pCurrNode);
			pCurrNode = pNode->pPrevY();
		}
		while (pCurrNode && pCurrNode != pNode && pCurrNode->y() == pNode->yy() &&
			!pCurrNode->hasFlags(COORDINATE_NODE_FLAG_NEGATIVE_BOUNDARY))
		{
			if (!pEqualCorrectionEnd)
				pEqualCorrectionEnd = pCurrNode;
			moveNodeY(pNode, pNode->yy(), pCurrNode);
			pCurrNode = pNode->pPrevY();
		}

		pCurrNode = pNode->pNextY();
		while (pCurrNode && pCurrNode != pNode && pCurrNode->y() < pNode->yy())
		{
			moveNodeY(pNode, pNode->yy(), pCurrNode);
			pCurrNode = pNode->pNextY();
		}
		while (pCurrNode && pCurrNode != pNode && pCurrNode->y() == pNode->yy() &&
			!pCurrNode->hasFlags(COORDINATE_NODE_FLAG_POSITIVE_BOUNDARY))
		{
			const bool isEqualCorrection = pEqualCorrectionEnd != NULL;
			moveNodeY(pNode, pNode->yy(), pCurrNode, !isEqualCorrection);
			if (pCurrNode == pEqualCorrectionEnd)
				pEqualCorrectionEnd = NULL;
			pCurrNode = pNode->pNextY();
		}

		pNode->y(pNode->yy());
	}

	if (pNode->zz() != pNode->old_zz())
	{
		CoordinateNode* pEqualCorrectionEnd = NULL;
		CoordinateNode* pCurrNode = pNode->pPrevZ();
		while (pCurrNode && pCurrNode != pNode && pCurrNode->z() > pNode->zz())
		{
			moveNodeZ(pNode, pNode->zz(), pCurrNode);
			pCurrNode = pNode->pPrevZ();
		}
		while (pCurrNode && pCurrNode != pNode && pCurrNode->z() == pNode->zz() &&
			!pCurrNode->hasFlags(COORDINATE_NODE_FLAG_NEGATIVE_BOUNDARY))
		{
			if (!pEqualCorrectionEnd)
				pEqualCorrectionEnd = pCurrNode;
			moveNodeZ(pNode, pNode->zz(), pCurrNode);
			pCurrNode = pNode->pPrevZ();
		}

		pCurrNode = pNode->pNextZ();
		while (pCurrNode && pCurrNode != pNode && pCurrNode->z() < pNode->zz())
		{
			moveNodeZ(pNode, pNode->zz(), pCurrNode);
			pCurrNode = pNode->pNextZ();
		}
		while (pCurrNode && pCurrNode != pNode && pCurrNode->z() == pNode->zz() &&
			!pCurrNode->hasFlags(COORDINATE_NODE_FLAG_POSITIVE_BOUNDARY))
		{
			const bool isEqualCorrection = pEqualCorrectionEnd != NULL;
			moveNodeZ(pNode, pNode->zz(), pCurrNode, !isEqualCorrection);
			if (pCurrNode == pEqualCorrectionEnd)
				pEqualCorrectionEnd = NULL;
			pCurrNode = pNode->pNextZ();
		}

		pNode->z(pNode->zz());
	}

	pNode->resetOld();
	//pNode->removeFlags(COORDINATE_NODE_FLAG_PENDING);
	--updating_;

	//if (updating_ == 0)
	//	releaseNodes();

#ifdef DEBUG_COORDINATE_SYSTEM
	DEBUG_MSG(fmt::format("CoordinateSystem::debugX[ x ]:[{:p}]\n", (void*)pNode));
	first_x_coordinateNode_->debugX();
	DEBUG_MSG(fmt::format("CoordinateSystem::debugY[ y ]:[{:p}]\n", (void*)pNode));
	if (first_y_coordinateNode_)first_y_coordinateNode_->debugY();
	DEBUG_MSG(fmt::format("CoordinateSystem::debugZ[ z ]:[{:p}]\n", (void*)pNode));
	first_z_coordinateNode_->debugZ();
#endif
}

//-------------------------------------------------------------------------------------
uint64 CoordinateSystem::equalCoordinateCorrectionMoves()
{
	return g_equalCoordinateCorrectionMoves;
}

//-------------------------------------------------------------------------------------
uint64 CoordinateSystem::equalCoordinateCallbacksSuppressed()
{
	return g_equalCoordinateCallbacksSuppressed;
}

//-------------------------------------------------------------------------------------
}
