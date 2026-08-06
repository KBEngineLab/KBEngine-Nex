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

#include <iterator>
#include "entity_coordinate_node.h"
#include "entity.h"
#include "coordinate_system.h"
#include "range_trigger_node.h"

namespace KBEngine{	


//-------------------------------------------------------------------------------------
class CoordinateNodeWrapX
{
public:
	CoordinateNodeWrapX(CoordinateNode* node, const Position3D& originPos) :
		pNode_(node),
		pCurrentNode_(node),
		originPos_(originPos) {}

	INLINE void reset() { pCurrentNode_ = pNode_; }
	INLINE bool isEntityNode() const { return pCurrentNode_->hasFlags(COORDINATE_NODE_FLAG_ENTITY); }
	INLINE bool valid() const { return !pCurrentNode_->hasFlags(COORDINATE_NODE_FLAG_HIDE_OR_REMOVED); }
	INLINE CoordinateNode* currentNode() const { return pCurrentNode_; }
	INLINE Entity* currentNodeEntity() const { return static_cast<EntityCoordinateNode*>(pCurrentNode_)->pEntity(); }
	
	INLINE CoordinateNode* prev() {
		pCurrentNode_ = pCurrentNode_->pPrevX();
		return pCurrentNode_;
	}

	INLINE CoordinateNode* next() {
		pCurrentNode_ = pCurrentNode_->pNextX();
		return pCurrentNode_;
	}

	INLINE int compare() {
		float v = currentNodeEntity()->position().x;
		if (v == originPos_.x)
			return 0;
		else if (v > originPos_.x)
			return 1;
		else
			return -1;
	}

	INLINE float length() {
		return fabs(currentNodeEntity()->position().x - originPos_.x);
	}

protected:
	CoordinateNode* pNode_;
	CoordinateNode* pCurrentNode_;
	const Position3D& originPos_;
};

class CoordinateNodeWrapZ : public CoordinateNodeWrapX
{
public:
	CoordinateNodeWrapZ(CoordinateNode* node, const Position3D& originPos) :
		CoordinateNodeWrapX(node, originPos) {}

	INLINE CoordinateNode* prev() {
		pCurrentNode_ = pCurrentNode_->pPrevZ();
		return pCurrentNode_;
	}

	INLINE CoordinateNode* next() {
		pCurrentNode_ = pCurrentNode_->pNextZ();
		return pCurrentNode_;
	}

	INLINE int compare() {
		float v = currentNodeEntity()->position().z;
		if (v == originPos_.z)
			return 0;
		else if (v > originPos_.z)
			return 1;
		else
			return -1;
	}

	INLINE float length() {
		return fabs(currentNodeEntity()->position().z - originPos_.z);
	}
};

class CoordinateNodeWrapY : public CoordinateNodeWrapX
{
public:
	CoordinateNodeWrapY(CoordinateNode* node, const Position3D& originPos) :
		CoordinateNodeWrapX(node, originPos) {}

	INLINE CoordinateNode* prev() {
		pCurrentNode_ = pCurrentNode_->pPrevY();
		return pCurrentNode_;
	}

	INLINE CoordinateNode* next() {
		pCurrentNode_ = pCurrentNode_->pNextY();
		return pCurrentNode_;
	}

	INLINE int compare() {
		float v = currentNodeEntity()->position().y;
		if (v == originPos_.y)
			return 0;
		else if (v > originPos_.y)
			return 1;
		else
			return -1;
	}

	INLINE float length() {
		return fabs(currentNodeEntity()->position().y - originPos_.y);
	}
};

//-------------------------------------------------------------------------------------
/**
 查找离中心点最近的节点
 模版参数 NODEWRAP 取值为以下三者之一：
   - CoordinateNodeWrapX
   - CoordinateNodeWrapZ
   - CoordinateNodeWrapY
*/
template <class NODEWRAP>
CoordinateNode* findNearestNode(CoordinateNode* rootNode, const Position3D& originPos)
{
	CoordinateNode* pRN = NULL;
	CoordinateNode* pCoordinateNode = rootNode;

	// 先找到一个EntityNode做支点
	{
		// 先找当前节点，找不到则往左边遍历寻找
		NODEWRAP wrap(rootNode, originPos);
		do
		{
			if (wrap.isEntityNode() && wrap.valid())
			{
				pRN = wrap.currentNode();
				break;
			}
		} while (wrap.prev());

		// 如果找不到，则往右边编历寻找
		if (!pRN)
		{
			wrap.reset();
			while (wrap.next())
			{
				if (wrap.isEntityNode() && wrap.valid())
				{
					pRN = wrap.currentNode();
					break;
				}
			}

			// 理论上不可能找不到
			if (!pRN)
				return NULL;
		}
	}

	// 能来到这里，表示一定是找到了，开始找离目标位置最近的Node
	NODEWRAP wrap(pRN, originPos);
	int v = wrap.compare();
	
	if (v == 0)  // 相等
	{
		return wrap.currentNode();
	}
	else if (v > 0)  // Entity Node在中心点的右边
	{
		pCoordinateNode = wrap.currentNode();
		while (wrap.prev())
		{
			if (wrap.isEntityNode() && wrap.valid())
			{
				// 由于是从中心点的右边往左边遍历，
				// 因此第一个position小于中心点的entity就一定是离中心点最近的
				if (wrap.compare() <= 0)
				{
					return wrap.currentNode();
				}
			}

			pCoordinateNode = wrap.currentNode();
		}
		return pCoordinateNode;
	}
	else   // Entity Node在中心点的左边
	{
		pCoordinateNode = wrap.currentNode();
		while (wrap.next())
		{
			if (wrap.isEntityNode() && wrap.valid())
			{
				// 由于是从中心点的左边往右边遍历，
				// 因此第一个position大于中心点的entity就一定是离中心点最近的
				if (wrap.compare() >= 0)
				{
					return wrap.currentNode();
				}
			}

			pCoordinateNode = wrap.currentNode();
		}

		return pCoordinateNode;
	}
}

//-------------------------------------------------------------------------------------
/**
 查找一个轴上符合范围的entity
 模版参数 NODEWRAP 取值为以下三者之一：
   - CoordinateNodeWrapX
   - CoordinateNodeWrapZ
   - CoordinateNodeWrapY
*/
template <class NODEWRAP>
void entitiesInAxisRange(std::set<Entity*>& foundEntities, CoordinateNode* rootNode,
	const Position3D& originPos, float radius, int entityUType)
{
	CoordinateNode* pCoordinateNode = findNearestNode<NODEWRAP>(rootNode, originPos);
	if (!pCoordinateNode)
		return;

	NODEWRAP wrap(pCoordinateNode, originPos);

	// 如果节点自己也符合条件，则把自己加进去
	if (wrap.isEntityNode() && wrap.valid())
	{
		Entity* pEntity = wrap.currentNodeEntity();

		if (entityUType == -1 || pEntity->pScriptModule()->getUType() == (ENTITY_SCRIPT_UID)entityUType)
		{
			if (wrap.length() <= radius)
			{
				foundEntities.insert(pEntity);
			}
		}
	}

	while (wrap.prev())
	{
		if (wrap.isEntityNode() && wrap.valid())
		{
			Entity* pEntity = wrap.currentNodeEntity();

			if (entityUType == -1 || pEntity->pScriptModule()->getUType() == (ENTITY_SCRIPT_UID)entityUType)
			{
				if (wrap.length() <= radius)
				{
					foundEntities.insert(pEntity);
				}
				else
				{
					break;
				}
			}
		}
	};

	wrap.reset();

	while (wrap.next())
	{
		if (wrap.isEntityNode() && wrap.valid())
		{
			Entity* pEntity = wrap.currentNodeEntity();

			if (entityUType == -1 || pEntity->pScriptModule()->getUType() == (ENTITY_SCRIPT_UID)entityUType)
			{
				if (wrap.length() <= radius)
				{
					foundEntities.insert(pEntity);
				}
				else
				{
					break;
				}
			}
		}
	};
}

//-------------------------------------------------------------------------------------
EntityCoordinateNode::EntityCoordinateNode(Entity* pEntity):
CoordinateNode(NULL),
pEntity_(pEntity),
watcherNodes_(),
delWatcherNodeNum_(0),
entityNodeUpdating_(0)
{
	flags(COORDINATE_NODE_FLAG_ENTITY);

#ifdef _DEBUG
	descr_ = (fmt::format("EntityCoordinateNode({}_{})", pEntity->scriptName(), pEntity->id()));
#endif

	weight_ = 1;

	Py_INCREF(pEntity_);
}

//-------------------------------------------------------------------------------------
EntityCoordinateNode::~EntityCoordinateNode()
{
	watcherNodes_.clear();

	pEntity_->onCoordinateNodesDestroy(this);
	Py_DECREF(pEntity_);
}

//-------------------------------------------------------------------------------------
float EntityCoordinateNode::xx() const
{
	if (pEntity_ == NULL || hasFlags((COORDINATE_NODE_FLAG_REMOVED | COORDINATE_NODE_FLAG_REMOVING)))
		return -FLT_MAX;

	return pEntity_->position().x;
}

//-------------------------------------------------------------------------------------
float EntityCoordinateNode::yy() const
{
	if(pEntity_ == NULL /*|| hasFlags((COORDINATE_NODE_FLAG_REMOVED | COORDINATE_NODE_FLAG_REMOVING))*/)
		return -FLT_MAX;

	return pEntity_->position().y;
}

//-------------------------------------------------------------------------------------
float EntityCoordinateNode::zz() const
{
	if(pEntity_ == NULL /*|| hasFlags((COORDINATE_NODE_FLAG_REMOVED | COORDINATE_NODE_FLAG_REMOVING))*/)
		return -FLT_MAX;

	return pEntity_->position().z;
}

//-------------------------------------------------------------------------------------
void EntityCoordinateNode::update()
{
	// 在这里做一下更新的原因是，很可能在CoordinateNode::update()的过程中导致实体位置被移动
	// 而导致次数update被调用，在某种情况下会出现问题
	// 例如：// A->B, B-A（此时old_*是B）, A->B（此时old_*是B，而xx等目的地就是B）,此时update中会误判为没有移动。
	// https://github.com/kbengine/kbengine/issues/407
	//
	// CoordinateNode::update() 可能通过观察关系回调递归触发本节点。递归入口
	// 已经看到同一组坐标时，不需要再次扫描全部 watcher；否则一个移动会把 AOI
	// 扇出重复放大。位置真正变化时仍保留原有排序和回调流程。
	//
	// CoordinateNode::update() can recursively re-enter this node through an AOI
	// callback. When the recursive entry sees the same coordinates, scanning every
	// watcher again only amplifies fan-out. Real movement keeps the original ordering
	// and callback flow unchanged.
	const float currentX = x();
	const float currentY = y();
	const float currentZ = z();
	if (currentX == old_xx() && currentY == old_yy() && currentZ == old_zz())
		return;

	old_xx(x());
	old_yy(y());
	old_zz(z());

	CoordinateNode::update();

	addFlags(COORDINATE_NODE_FLAG_ENTITY_NODE_UPDATING);
	++entityNodeUpdating_;

	// 此处必须使用watcherNodes_.size()而不能使用迭代器遍历，防止在update中导致增加了watcherNodes_数量而破坏迭代器
	for (WATCHER_NODES::size_type i = 0; i < watcherNodes_.size(); ++i)
	{
		CoordinateNode* pCoordinateNode = watcherNodes_[i];
		if (!pCoordinateNode)
			continue;

		pCoordinateNode->update();
	}

	--entityNodeUpdating_;
	if (entityNodeUpdating_ == 0)
		removeFlags(COORDINATE_NODE_FLAG_ENTITY_NODE_UPDATING);

	clearDelWatcherNodes();
}

//-------------------------------------------------------------------------------------
void EntityCoordinateNode::clearDelWatcherNodes()
{
	if (hasFlags((COORDINATE_NODE_FLAG_ENTITY_NODE_UPDATING | COORDINATE_NODE_FLAG_REMOVED | COORDINATE_NODE_FLAG_REMOVING)))
		return;

	if (delWatcherNodeNum_ > 0)
	{
		// 批量 Leave 时不能逐项 erase：每次 erase 都会搬移后续指针，
		// 使一次清理退化为 O(n^2)。使用读写游标一次压紧，保持原有顺序。
		// Batch Leave must not erase one element at a time: each erase shifts the
		// remaining pointers and turns cleanup into O(n^2). Compact once in linear
		// time while preserving the established watcher order.
		WATCHER_NODES::size_type writeIndex = 0;
		for (WATCHER_NODES::size_type readIndex = 0; readIndex < watcherNodes_.size(); ++readIndex)
		{
			CoordinateNode* pNode = watcherNodes_[readIndex];
			if (!pNode)
				continue;

			watcherNodes_[writeIndex++] = pNode;
		}

		watcherNodes_.resize(writeIndex);
		delWatcherNodeNum_ = 0;
	}
}

//-------------------------------------------------------------------------------------
void EntityCoordinateNode::onRemove()
{
	for (WATCHER_NODES::size_type i = 0; i < watcherNodes_.size(); ++i)
	{
		CoordinateNode* pCoordinateNode = watcherNodes_[i];

		if (!pCoordinateNode)
			continue;

		// 先设置为NULL， 在后面update时进行删除
		// 此处不能对watcherNodes_做大小做修改，因为可能由EntityCoordinateNode::update()中导致该处调用
		// 那么可能导致EntityCoordinateNode::update()在循环watcherNodes_中被修改而出错。
		watcherNodes_[i] = NULL;
		++delWatcherNodeNum_;

		pCoordinateNode->onParentRemove(this);
	}

	CoordinateNode::onRemove();
}

//-------------------------------------------------------------------------------------
bool EntityCoordinateNode::addWatcherNode(CoordinateNode* pNode)
{
	clearDelWatcherNodes();

	WATCHER_NODES::iterator iter = std::find(watcherNodes_.begin(), watcherNodes_.end(), pNode);
	if(iter != watcherNodes_.end())
		return false;

	watcherNodes_.push_back(pNode);
	
	onAddWatcherNode(pNode);
	return true;
}

//-------------------------------------------------------------------------------------
void EntityCoordinateNode::onAddWatcherNode(CoordinateNode* pNode)
{
}

//-------------------------------------------------------------------------------------
bool EntityCoordinateNode::delWatcherNode(CoordinateNode* pNode)
{
	WATCHER_NODES::iterator iter = std::find(watcherNodes_.begin(), watcherNodes_.end(), pNode);
	if(iter == watcherNodes_.end())
		return false;

	if (hasFlags((COORDINATE_NODE_FLAG_ENTITY_NODE_UPDATING | COORDINATE_NODE_FLAG_REMOVED | COORDINATE_NODE_FLAG_REMOVING)))
	{
		(*iter) = NULL;
		++delWatcherNodeNum_;
	}
	else
	{
		watcherNodes_.erase(iter);
	}

	return true;
}

//-------------------------------------------------------------------------------------
void EntityCoordinateNode::entitiesInRange(std::vector<Entity*>& foundEntities, CoordinateNode* rootNode,
									  const Position3D& originPos, float radius, int entityUType)
{
	std::set<Entity*> entities_X;
	std::set<Entity*> entities_Z;

	entitiesInAxisRange<CoordinateNodeWrapX>(entities_X, rootNode, originPos, radius, entityUType);
	entitiesInAxisRange<CoordinateNodeWrapZ>(entities_Z, rootNode, originPos, radius, entityUType);

	// 查找Y
	if (CoordinateSystem::hasY)
	{
		std::set<Entity*> entities_Y;
		entitiesInAxisRange<CoordinateNodeWrapY>(entities_Y, rootNode, originPos, radius, entityUType);


		std::set<Entity*> res_set;
		set_intersection(entities_X.begin(), entities_X.end(), entities_Z.begin(), entities_Z.end(), std::inserter(res_set, res_set.end()));
		set_intersection(res_set.begin(), res_set.end(), entities_Y.begin(), entities_Y.end(), std::back_inserter(foundEntities));
	}
	else
	{
		set_intersection(entities_X.begin(), entities_X.end(), entities_Z.begin(), entities_Z.end(), std::back_inserter(foundEntities));
	}
}

//-------------------------------------------------------------------------------------
}
