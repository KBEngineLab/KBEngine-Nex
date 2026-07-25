/*
This source file is part of KBEngine
*/

#ifndef KBE_CELL_SPACE_ENTITY_H
#define KBE_CELL_SPACE_ENTITY_H

#include "entity.h"

namespace KBEngine
{

// Space 将 Cell 侧脚本对象标记为空间实体。
// Space marks the cell-side script object as a space entity.
class SpaceEntity : public Entity
{
	BASE_SCRIPT_HREADER(SpaceEntity, Entity)

public:
	SpaceEntity(ENTITY_ID id, const ScriptDefModule* pScriptModule);
	~SpaceEntity();
};

}

#endif
