/*
This source file is part of KBEngine
*/

#include "space_entity.h"

namespace KBEngine
{

SCRIPT_METHOD_DECLARE_BEGIN(SpaceEntity)
SCRIPT_METHOD_DECLARE_END()
SCRIPT_MEMBER_DECLARE_BEGIN(SpaceEntity)
SCRIPT_MEMBER_DECLARE_END()
SCRIPT_GETSET_DECLARE_BEGIN(SpaceEntity)
SCRIPT_GETSET_DECLARE_END()
BASE_SCRIPT_INIT(SpaceEntity, 0, 0, 0, 0, 0)

SpaceEntity::SpaceEntity(ENTITY_ID id, const ScriptDefModule* pScriptModule) :
	Entity(id, pScriptModule, getScriptType(), true)
{
}

SpaceEntity::~SpaceEntity()
{
}

}
