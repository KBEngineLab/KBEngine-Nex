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

#include "range_trigger.h"
#include "coordinate_system.h"
#include "entity_coordinate_node.h"
#include "range_trigger_node.h"

#ifndef CODE_INLINE
#include "range_trigger.inl"
#endif

namespace KBEngine{	

//-------------------------------------------------------------------------------------
RangeTrigger::RangeTrigger(CoordinateNode* origin, float xz, float y):
range_xz_(fabs(xz)),
range_y_(fabs(y)),
origin_(origin),
positiveBoundary_(NULL),
negativeBoundary_(NULL),
removing_(false)
{
}

//-------------------------------------------------------------------------------------
RangeTrigger::~RangeTrigger()
{
	uninstall();
}

//-------------------------------------------------------------------------------------
bool RangeTrigger::reinstall(CoordinateNode* pCoordinateNode)
{
	uninstall();
	origin_ = pCoordinateNode;
	return install();
}

//-------------------------------------------------------------------------------------
bool RangeTrigger::install()
{
	if(positiveBoundary_ == NULL)
		positiveBoundary_ = new RangeTriggerNode(this, 0, 0, true);
	else
		positiveBoundary_->range(0.0f, 0.0f);

	if(negativeBoundary_ == NULL)
		negativeBoundary_ = new RangeTriggerNode(this, 0, 0, false);
	else
		negativeBoundary_->range(0.0f, 0.0f);

	positiveBoundary_->addFlags(COORDINATE_NODE_FLAG_INSTALLING);
	negativeBoundary_->addFlags(COORDINATE_NODE_FLAG_INSTALLING);

	origin_->pCoordinateSystem()->insert(positiveBoundary_);
	origin_->pCoordinateSystem()->insert(negativeBoundary_);
	
	/*
	ע�⣺�˴��������Ȱ�װnegativeBoundary_�ٰ�װpositiveBoundary_���������˳����ᵼ��View��BUG�����磺��һ��ʵ��enterView����ʱ�����˽���View��ʵ��
	��ʱʵ������ʱ��δ�����뿪View�¼�����δ����View�¼���������ʵ���View�б������õĸ����ٵ�ʵ����һ����Чָ�롣

	ԭ�����£�
	�����������Ȱ�װ��positiveBoundary_�����߽��ڰ�װ�����е�����һ��ʵ�����View�ˣ� Ȼ��������������п��������ˣ� ����һ���߽�negativeBoundary_��û�а�װ�� 
	���ڵ�ɾ��ʱ�����ýڵ��xxΪ-FLT_MAX��������negativeBoundary_�����뿪������positiveBoundary_���ܼ�鵽����߽�Ҳ�Ͳ��ᴥ��View�뿪�¼���
	*/
	negativeBoundary_->old_xx(-FLT_MAX);
	negativeBoundary_->old_yy(-FLT_MAX);
	negativeBoundary_->old_zz(-FLT_MAX);
	negativeBoundary_->range(-range_xz_, -range_y_);
	negativeBoundary_->old_range(-range_xz_, -range_y_);
	negativeBoundary_->update();

	// update���ܵ���ʵ�����ټ�ӵ����Լ������ã���ʱӦ�÷��ذ�װʧ��
	if (!negativeBoundary_)
		return false;

	negativeBoundary_->removeFlags(COORDINATE_NODE_FLAG_INSTALLING);

	positiveBoundary_->old_xx(FLT_MAX);
	positiveBoundary_->old_yy(FLT_MAX);
	positiveBoundary_->old_zz(FLT_MAX);

	positiveBoundary_->range(range_xz_, range_y_);
	positiveBoundary_->old_range(range_xz_, range_y_);
	positiveBoundary_->update();

	if (positiveBoundary_)
	{
		positiveBoundary_->removeFlags(COORDINATE_NODE_FLAG_INSTALLING);
		return true;
	}

	return false;
}

//-------------------------------------------------------------------------------------
bool RangeTrigger::uninstall()
{
	if (removing_)
		return false;

	removing_ = true;
	if(positiveBoundary_ && positiveBoundary_->pCoordinateSystem())
	{
		positiveBoundary_->pCoordinateSystem()->remove(positiveBoundary_);
		positiveBoundary_->onTriggerUninstall();
	}

	if(negativeBoundary_ && negativeBoundary_->pCoordinateSystem())
	{
		negativeBoundary_->pCoordinateSystem()->remove(negativeBoundary_);
		negativeBoundary_->onTriggerUninstall();
	}
	
	// �˴�����release node�� �ڵ���ͷ�ͳһ����CoordinateSystem
	positiveBoundary_ = NULL;
	negativeBoundary_ = NULL;
	removing_ = false;
	return true;
}

//-------------------------------------------------------------------------------------
void RangeTrigger::onNodePassX(RangeTriggerNode* pRangeTriggerNode, CoordinateNode* pNode, bool isfront)
{
	if(pNode == origin())
		return;

	bool wasInZ = pRangeTriggerNode->wasInZRange(pNode);
	bool isInZ = pRangeTriggerNode->isInZRange(pNode);

	// ���Z������б仯����Z�����жϣ����ȼ�Ϊzyx�������ſ��Ա�ֻ֤��һ��enter����leave
	if(wasInZ != isInZ)
		return;

	bool wasIn = false;
	bool isIn = false;

	// ����ͬʱ��������ᣬ ����ڵ�x���ڷ�Χ�ڣ�������������Ҳ�ڷ�Χ��
	if(CoordinateSystem::hasY)
	{
		bool wasInY = pRangeTriggerNode->wasInYRange(pNode);
		bool isInY = pRangeTriggerNode->isInYRange(pNode);

		if(wasInY != isInY)
			return;

		wasIn = pRangeTriggerNode->wasInXRange(pNode) && wasInY && wasInZ;
		isIn = pRangeTriggerNode->isInXRange(pNode) && isInY && isInZ;
	}
	else
	{
		wasIn = pRangeTriggerNode->wasInXRange(pNode) && wasInZ;
		isIn = pRangeTriggerNode->isInXRange(pNode) && isInZ;
	}

	// ������û�з����仯�����
	if(wasIn == isIn)
		return;

	if(isIn)
	{
		this->onEnter(pNode);
	}
	else
	{
		this->onLeave(pNode);
	}
}

//-------------------------------------------------------------------------------------
void RangeTrigger::onNodePassY(RangeTriggerNode* pRangeTriggerNode, CoordinateNode* pNode, bool isfront)
{
	if(pNode == origin() || !CoordinateSystem::hasY)
		return;

	bool wasInZ = pRangeTriggerNode->wasInZRange(pNode);
	bool isInZ = pRangeTriggerNode->isInZRange(pNode);

	// ���Z������б仯����Z�����жϣ����ȼ�Ϊzyx�������ſ��Ա�ֻ֤��һ��enter����leave
	if(wasInZ != isInZ)
		return;

	bool wasInY = pRangeTriggerNode->wasInYRange(pNode);
	bool isInY = pRangeTriggerNode->isInYRange(pNode);

	if(wasInY == isInY)
		return;

	// ����ͬʱ��������ᣬ ����ڵ�x���ڷ�Χ�ڣ�������������Ҳ�ڷ�Χ��
	bool wasIn = pRangeTriggerNode->wasInXRange(pNode) && wasInY && wasInZ;
	bool isIn = pRangeTriggerNode->isInXRange(pNode) && isInY && isInZ;

	if(wasIn == isIn)
		return;

	if(isIn)
	{
		this->onEnter(pNode);
	}
	else
	{
		this->onLeave(pNode);
	}
}

//-------------------------------------------------------------------------------------
void RangeTrigger::onNodePassZ(RangeTriggerNode* pRangeTriggerNode, CoordinateNode* pNode, bool isfront)
{
	if(pNode == origin())
		return;

	if(CoordinateSystem::hasY)
	{
		bool wasInZ = pRangeTriggerNode->wasInZRange(pNode);
		bool isInZ = pRangeTriggerNode->isInZRange(pNode);

		if(wasInZ == isInZ)
			return;

		bool wasIn = pRangeTriggerNode->wasInXRange(pNode) && 
			pRangeTriggerNode->wasInYRange(pNode) && 
			wasInZ;

		bool isIn = pRangeTriggerNode->isInXRange(pNode) && 
			pRangeTriggerNode->isInYRange(pNode) && 
			isInZ;

		if(wasIn == isIn)
			return;

		if(isIn)
		{
			this->onEnter(pNode);
		}
		else
		{
			this->onLeave(pNode);
		}
	}
	else
	{
		bool wasInZ = pRangeTriggerNode->wasInZRange(pNode);
		bool isInZ = pRangeTriggerNode->isInZRange(pNode);

		if(wasInZ == isInZ)
			return;

		bool wasIn = pRangeTriggerNode->wasInXRange(pNode) && wasInZ;
		bool isIn = pRangeTriggerNode->isInXRange(pNode) && isInZ;

		if(wasIn == isIn)
			return;

		if(isIn)
		{
			this->onEnter(pNode);
		}
		else
		{
			this->onLeave(pNode);
		}
	}
}

//-------------------------------------------------------------------------------------
void RangeTrigger::update(float xz, float y)
{
	float old_range_xz_ = range_xz_;
	float old_range_y_ = range_y_;

	range(xz, y);

	if (positiveBoundary_)
	{
		positiveBoundary_->range(range_xz_, range_y_);
		positiveBoundary_->old_range(old_range_xz_, old_range_y_);
		positiveBoundary_->update();
	}

	if (negativeBoundary_)
	{
		negativeBoundary_->range(-range_xz_, -range_y_);
		negativeBoundary_->old_range(-old_range_xz_, -old_range_y_);
		negativeBoundary_->update();
	}
}

//-------------------------------------------------------------------------------------
}
