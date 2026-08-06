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


namespace KBEngine{

//-------------------------------------------------------------------------------------
INLINE bool RangeTriggerNode::isInXRange(CoordinateNode * pNode)
{
	const float originX = pRangeTrigger_->origin()->xx();
	const float nodeX = pNode->xx();
	const float radius = fabs(range_xz_);

	const float lowerBound = originX - radius;
	const float upperBound = originX + radius;
	return (nodeX >= lowerBound) && (nodeX <= upperBound);
}

//-------------------------------------------------------------------------------------
INLINE bool RangeTriggerNode::isInYRange(CoordinateNode * pNode)
{
	const float originY = pRangeTrigger_->origin()->yy();
	const float nodeY = pNode->yy();
	const float radius = fabs(range_y_);

	const float lowerBound = originY - radius;
	const float upperBound = originY + radius;
	return (nodeY >= lowerBound) && (nodeY <= upperBound);
}

//-------------------------------------------------------------------------------------
INLINE bool RangeTriggerNode::isInZRange(CoordinateNode * pNode)
{
	const float originZ = pRangeTrigger_->origin()->zz();
	const float nodeZ = pNode->zz();
	const float radius = fabs(range_xz_);

	const float lowerBound = originZ - radius;
	const float upperBound = originZ + radius;
	return (nodeZ >= lowerBound) && (nodeZ <= upperBound);
}

//-------------------------------------------------------------------------------------
INLINE bool RangeTriggerNode::wasInXRange(CoordinateNode * pNode)
{
	const float originX = old_xx() - old_range_xz_;
	const float nodeX = pNode->old_xx();
	const float radius = fabs(old_range_xz_);

	const float lowerBound = originX - radius;
	const float upperBound = originX + radius;
	return (nodeX >= lowerBound) && (nodeX <= upperBound);
}

//-------------------------------------------------------------------------------------
INLINE bool RangeTriggerNode::wasInZRange(CoordinateNode * pNode)
{
	const float originZ = old_zz() - old_range_xz_;
	const float nodeZ = pNode->old_zz();
	const float radius = fabs(old_range_xz_);

	const float lowerBound = originZ - radius;
	const float upperBound = originZ + radius;
	return (nodeZ >= lowerBound) && (nodeZ <= upperBound);
}

//-------------------------------------------------------------------------------------
INLINE void RangeTriggerNode::range(float xz, float y)
{
	range_xz_ = xz;
	range_y_ = y;
}

//-------------------------------------------------------------------------------------
INLINE void RangeTriggerNode::old_range(float xz, float y)
{
	old_range_xz_ = xz;
	old_range_y_ = y;
}

//-------------------------------------------------------------------------------------
INLINE float RangeTriggerNode::range_xz() const
{
	return range_xz_;
}

//-------------------------------------------------------------------------------------
INLINE float RangeTriggerNode::range_y() const
{
	return range_y_;
}

//-------------------------------------------------------------------------------------
INLINE RangeTrigger* RangeTriggerNode::pRangeTrigger() const
{
	return pRangeTrigger_;
}

//-------------------------------------------------------------------------------------
INLINE void RangeTriggerNode::pRangeTrigger(RangeTrigger* pRangeTrigger)
{
	pRangeTrigger_ = pRangeTrigger;
}

//-------------------------------------------------------------------------------------
INLINE bool RangeTriggerNode::isPositive() const
{
	return hasFlags(COORDINATE_NODE_FLAG_POSITIVE_BOUNDARY);
}

//-------------------------------------------------------------------------------------
}
