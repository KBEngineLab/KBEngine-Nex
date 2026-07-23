
#include "EntityCall.h"
#include "EntityDef.h"
#include "Bundle.h"
#include "KBEngine.h"
#include "Messages.h"
#include "KBDebug.h"
#include "Method.h"
#include "ScriptModule.h"

namespace KBEngine
{

EntityCall::EntityCall(int32 eid, const KBString& ename):
	id(eid),
	className(ename),
	type(ENTITYCALL_TYPE_CELL),
	pBundle(NULL)
{
}

EntityCall::~EntityCall()
{
}

Bundle* EntityCall::newCall()
{
	if (!pBundle)
		pBundle = Bundle::createObject();

	if (type == ENTITYCALL_TYPE_CELL)
		pBundle->newMessage(Messages::messages[KBTEXT("Baseapp_onRemoteCallCellMethodFromClient"]));
	else
		pBundle->newMessage(Messages::messages[KBTEXT("Entity_onRemoteMethodCall"]));

	(*pBundle) << id;
	return pBundle;
}

Bundle* EntityCall::newCall(const KBString& methodName, uint16 entitycomponentPropertyID)
{
	if(KBEngineApp::getSingleton().currserver() == KBTEXT("loginapp"))
	{
		ERROR_MSG("%s::newCall(%s), currserver=%s!", className.c_str(), methodName.c_str(), KBEngineApp::getSingleton().currserver().c_str());
		return NULL;
	}

	ScriptModule** smFind = EntityDef::moduledefs.Find(className);
	if (!smFind)
	{
		ERROR_MSG("%s::newCall: entity-module(%s) error, can not find from EntityDef.moduledefs", className.c_str(), className.c_str());
		return NULL;
	}
	
	ScriptModule* sm = *smFind;
	Method* pMethod = NULL;

	if(isCell())
	{
		pMethod = sm->cell_methods[methodName];
	}
	else
	{
		pMethod = sm->base_methods[methodName];
	}

	uint16 methodID = pMethod->methodUtype;

	newCall();
	pBundle->writeUint16(entitycomponentPropertyID);
	pBundle->writeUint16(methodID);
	return pBundle;
}

void EntityCall::sendCall(Bundle* inBundle)
{
	if (!inBundle)
		inBundle = pBundle;

	inBundle->send(KBEngineApp::getSingleton().pNetworkInterface());

	if (inBundle == pBundle)
		pBundle = NULL;
}

}