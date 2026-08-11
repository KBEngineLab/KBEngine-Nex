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


#ifndef KBE_DETAILLEVEL_H
#define KBE_DETAILLEVEL_H

#include <cfloat>
#include <cmath>
#include <cstdint>

namespace KBEngine{

typedef std::uint8_t DETAIL_TYPE;
#define DETAIL_LEVEL_NEAR                                                   0   // lod级别：近
#define DETAIL_LEVEL_MEDIUM                                                 1   // lod级别：中
#define DETAIL_LEVEL_FAR                                                    2   // lod级别：远
#define DETAIL_LEVEL_COUNT                                                  3   // 有效详情级别数量
#define DETAIL_LEVEL_NONE                                                   DETAIL_LEVEL_COUNT // 超出所有详情级别

/** entity 详情级别类型定义 
	默认有3个级别分别为:
	 近， 中， 远
*/
struct DetailLevel
{
	struct Level
	{
		Level():radius(FLT_MAX), hyst(1.0f){};
		float radius;
		float hyst;

		bool inLevel(float dist) const
		{
			return radius >= dist;
		}

		bool inLevelSquared(float distanceSquared, bool useHysteresis) const
		{
			if(radius == FLT_MAX)
				return true;

			const float threshold = useHysteresis ? radius + hyst : radius;
			if(!std::isfinite(threshold))
				return true;

			const double thresholdSquared = static_cast<double>(threshold) * threshold;
			return static_cast<double>(distanceSquared) <= thresholdSquared;
		}
	};
	
	DetailLevel():
	configured_(false)
	{
	}

	~DetailLevel()
	{
	}

	DETAIL_TYPE resolveLevel(float distanceSquared, DETAIL_TYPE previousLevel = DETAIL_LEVEL_NONE) const
	{
		// 向内移动使用配置半径作为进入阈值；向外移动使用当前等级的 radius+hyst，
		// 避免实体在边界附近反复补发属性。
		// Moving inward uses the configured radius, while moving outward uses radius+hyst
		// so boundary jitter cannot repeatedly trigger property catch-up streams.
		for(DETAIL_TYPE detailLevel = DETAIL_LEVEL_NEAR;
			detailLevel < previousLevel && detailLevel < DETAIL_LEVEL_COUNT; ++detailLevel)
		{
			if(level[detailLevel].inLevelSquared(distanceSquared, false))
				return detailLevel;
		}

		if(previousLevel < DETAIL_LEVEL_COUNT &&
			level[previousLevel].inLevelSquared(distanceSquared, true))
		{
			return previousLevel;
		}

		const DETAIL_TYPE firstOuterLevel = previousLevel < DETAIL_LEVEL_COUNT ?
			static_cast<DETAIL_TYPE>(previousLevel + 1) : DETAIL_LEVEL_NEAR;
		for(DETAIL_TYPE detailLevel = firstOuterLevel;
			detailLevel < DETAIL_LEVEL_COUNT; ++detailLevel)
		{
			if(level[detailLevel].inLevelSquared(distanceSquared, previousLevel < DETAIL_LEVEL_COUNT))
				return detailLevel;
		}

		return DETAIL_LEVEL_NONE;
	}

	bool isVisible(DETAIL_TYPE propertyLevel, DETAIL_TYPE relationLevel) const
	{
		return propertyLevel < DETAIL_LEVEL_COUNT &&
			relationLevel < DETAIL_LEVEL_COUNT && relationLevel <= propertyLevel;
	}

	bool configured() const { return configured_; }
	void configured(bool value) { configured_ = value; }

	Level level[DETAIL_LEVEL_COUNT]; // 近， 中， 远

private:
	bool configured_;
};

}


#endif // KBE_DETAILLEVEL_H

