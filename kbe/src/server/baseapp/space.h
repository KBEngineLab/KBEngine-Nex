/*
This source file is part of KBEngine
*/

#ifndef KBE_BASE_SPACE_ENTITY_H
#define KBE_BASE_SPACE_ENTITY_H

#include "entity.h"

namespace KBEngine
{

// Space 是脚本可见的 Entity 子类型，供 Base 脚本创建空间实体。
// Space is a script-visible Entity subtype used by base scripts to create spaces.
class Space : public Entity
{
	BASE_SCRIPT_HREADER(Space, Entity)

public:
	Space(ENTITY_ID id, const ScriptDefModule* pScriptModule);
	~Space();

	DECLARE_PY_GETSET_MOTHOD(pyGetCreateToCellappIndex, pySetCreateToCellappIndex);

private:
	uint32 createToCellappIndex_;
};

}

#endif
