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

#include "witness.h"
#include "cellapp.h"
#include "space.h"
#include "entitydef/method.h"
#include "entitydef/property.h"
#include "entitydef/scriptdef_module.h"
#include "clients_remote_entity_method.h"
#include "network/bundle.h"
#include "network/network_stats.h"
#include "helper/eventhistory_stats.h"

#include "client_lib/client_interface.h"
#include "../../server/baseapp/baseapp_interface.h"

namespace KBEngine{

SCRIPT_METHOD_DECLARE_BEGIN(ClientsRemoteEntityMethod)
SCRIPT_METHOD_DECLARE_END()

SCRIPT_MEMBER_DECLARE_BEGIN(ClientsRemoteEntityMethod)
SCRIPT_MEMBER_DECLARE_END()

SCRIPT_GETSET_DECLARE_BEGIN(ClientsRemoteEntityMethod)
SCRIPT_GETSET_DECLARE_END()
SCRIPT_INIT(ClientsRemoteEntityMethod, tp_call, 0, 0, 0, 0)	

//-------------------------------------------------------------------------------------
ClientsRemoteEntityMethod::ClientsRemoteEntityMethod(PropertyDescription* pComponentPropertyDescription,
													 const ScriptDefModule* pScriptModule,
													 MethodDescription* methodDescription,
													 bool otherClients,
													 ENTITY_ID id):
script::ScriptObject(getScriptType(), false),
pComponentPropertyDescription_(pComponentPropertyDescription),
pScriptModule_(pScriptModule),
methodDescription_(methodDescription),
otherClients_(otherClients),
id_(id)
{
}

//-------------------------------------------------------------------------------------
ClientsRemoteEntityMethod::~ClientsRemoteEntityMethod()
{
}

//-------------------------------------------------------------------------------------
PyObject* ClientsRemoteEntityMethod::tp_call(PyObject* self, PyObject* args, 
	PyObject* kwds)	
{
	ClientsRemoteEntityMethod* rmethod = static_cast<ClientsRemoteEntityMethod*>(self);
	return rmethod->callmethod(args, kwds);	
}		

//-------------------------------------------------------------------------------------
PyObject* ClientsRemoteEntityMethod::callmethod(PyObject* args, PyObject* kwds)
{
	// 获取entityView范围内其他entity
	// 向这些entity的client推送这个方法的调用
	MethodDescription* methodDescription = getDescription();

	Entity* pEntity = Cellapp::getSingleton().findEntity(id_);
	if(pEntity == NULL || /*pEntity->pWitness() == NULL ||*/
		pEntity->isDestroyed() || pEntity->isBoundaryGhost() /*|| pEntity->clientEntityCall() == NULL*/)
	{
		//WARNING_MSG(fmt::format("EntityRemoteMethod::callClientMethod: not found entity({}).\n", 
		//	entitycall->id()));

		S_Return;
	}
	
	Space* space = Cellapp::getSingleton().spacePartitioningEnabled() ? Spaces::findSpace(pEntity->spaceID()) : NULL;
	const bool remoteWitnesses = space && space->boundaryGhosts().hasCopies(pEntity->id());
	if (otherClients_ && pEntity->witnessesSize() == 0 && !remoteWitnesses)
		S_Return;
	
	// 先发给自己
	if(methodDescription->checkArgs(args))
	{
		MemoryStream* mstream = MemoryStream::createPoolObject(OBJECTPOOL_POINT);

		// 客户端 RPC 数据始终以父组件属性 UID 开头；实体方法使用 0，组件方法使用其真实 UID/alias。
		// Client RPC data always starts with the parent component-property UID; entity methods use zero and component methods use their real UID/alias.
		if (pComponentPropertyDescription_ != NULL)
		{
			if (pScriptModule_->usePropertyDescrAlias())
				(*mstream) << pComponentPropertyDescription_->aliasIDAsUint8();
			else
				(*mstream) << pComponentPropertyDescription_->getUType();
		}
		else
		{
			if (pScriptModule_->usePropertyDescrAlias())
				(*mstream) << static_cast<uint8>(0);
			else
				(*mstream) << static_cast<ENTITY_PROPERTY_UID>(0);
		}

		try
		{
			methodDescription->addToStream(mstream, args);
		}
		catch (MemoryStreamWriteOverflow & err)
		{
			ERROR_MSG(fmt::format("ClientsRemoteEntityMethod::callmethod: {}::{} {}, error={}!\n",
				pEntity->scriptName(), methodDescription->getName(), pEntity->id(), err.what()));

			MemoryStream::reclaimPoolObject(mstream);
			S_Return;
		}

		if((!otherClients_ && (pEntity->pWitness() && (pEntity->clientEntityCall()))))
		{
			Network::Bundle* pSendBundle = NULL;
			Network::Channel* pChannel = pEntity->clientEntityCall()->getChannel();

			if (!pChannel)
				pSendBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
			else
				pSendBundle = pChannel->createSendBundle();

			pEntity->clientEntityCall()->newCall_((*pSendBundle));

			if(mstream->wpos() > 0)
				(*pSendBundle).append(mstream->data(), (int)mstream->wpos());

			if(Network::g_trace_packet > 0)
			{
				if(Network::g_trace_packet_use_logfile)
					DebugHelper::getSingleton().changeLogger("packetlogs");

				DEBUG_MSG(fmt::format("ClientsRemoteEntityMethod::callmethod: pushUpdateData: ClientInterface::onRemoteMethodCall({}::{})\n", 
					pEntity->scriptName(), methodDescription->getName()));

				switch(Network::g_trace_packet)
				{
				case 1:
					mstream->hexlike();
					break;
				case 2:
					mstream->textlike();
					break;
				default:
					mstream->print_storage();
					break;
				};

				if(Network::g_trace_packet_use_logfile)	
					DebugHelper::getSingleton().changeLogger(COMPONENT_NAME_EX(g_componentType));
			}

			// 记录这个事件产生的数据量大小
			g_publicClientEventHistoryStats.trackEvent(pEntity->scriptName(),
				methodDescription->getName(),
				pSendBundle->currMsgLength(),
				"::");

			//entitycall->sendCall((*pBundle));
			pEntity->pWitness()->sendToClient(ClientInterface::onRemoteMethodCall, pSendBundle);
		}

		forwardToWitnesses(*pEntity, *mstream, methodDescription->getName());
		if (remoteWitnesses)
			space->boundaryGhosts().broadcast(*space, *pEntity, *mstream);

		MemoryStream::reclaimPoolObject(mstream);
	}

	S_Return;
}

//-------------------------------------------------------------------------------------
void ClientsRemoteEntityMethod::forwardToWitnesses(Entity& entity, MemoryStream& data, const char* methodName)
{
	Entity* pEntity = &entity;
	const Entity::WITNESS_IDS& entities = pEntity->witnesses();
	MemoryStream* mstream = &data;
	const char* eventName = methodName ? methodName : "boundaryGhost";
	const bool boundaryGhost = pEntity->isBoundaryGhost();
	{
		Entity::WITNESS_IDS::const_iterator iter = entities.begin();
		for(; iter != entities.end(); ++iter)
		{
			Entity* pViewEntity = Cellapp::getSingleton().findEntity((*iter));
			if(pViewEntity == NULL || pViewEntity->pWitness() == NULL || pViewEntity->isDestroyed())
				continue;
			
			EntityCall* entitycall = pViewEntity->clientEntityCall();
			if(entitycall == NULL)
				continue;

			Network::Channel* pChannel = entitycall->getChannel();
			if(pChannel == NULL)
				continue;

			// 只对已经在客户端创建、且仍绑定到当前实体的关系转发。
			Witness* pWitness = pViewEntity->pWitness();
			EntityRef* pEntityRef = pWitness->getViewEntityRef(pEntity->id());
			if (!pEntityRef || pEntityRef->pEntity() != pEntity ||
				pEntityRef->flags() != ENTITYREF_FLAG_NORMAL)
				continue;
			
			Network::Bundle* pSendBundle = pChannel->createSendBundle();
			NETWORK_ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pViewEntity->id(), (*pSendBundle));

			const auto forwardHeaderLength = pSendBundle->currMsgLength();
			// 换源后 RPC 不能越过 Witness 队列中尚未发送的快照。在同一转发消息内先写属性。
			if (boundaryGhost)
				pWitness->appendPendingBoundaryGhostSnapshot(pSendBundle, *pEntityRef);
			const auto snapshotLength = pSendBundle->currMsgLength() - forwardHeaderLength;
			
			int ialiasID = -1;
			const Network::MessageHandler& msgHandler = 
			pWitness->getViewEntityMessageHandler(ClientInterface::onRemoteMethodCall,
					ClientInterface::onRemoteMethodCallOptimized, pEntity->id(), ialiasID);

			ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(pSendBundle, msgHandler, viewEntityMessage);

			if(ialiasID != -1)
			{
				KBE_ASSERT(msgHandler.msgID == ClientInterface::onRemoteMethodCallOptimized.msgID);
				(*pSendBundle)  << (uint8)ialiasID;
			}
			else
			{
				KBE_ASSERT(msgHandler.msgID == ClientInterface::onRemoteMethodCall.msgID);
				(*pSendBundle)  << pEntity->id();
			}

			if(mstream->length() > 0)
				pSendBundle->append(mstream->data() + mstream->rpos(), static_cast<int>(mstream->length()));

			if(Network::g_trace_packet > 0)
			{
				if(Network::g_trace_packet_use_logfile)
					DebugHelper::getSingleton().changeLogger("packetlogs");

				DEBUG_MSG(fmt::format("ClientsRemoteEntityMethod::callmethod: pushUpdateData: ClientInterface::onRemoteOtherEntityMethodCall({}::{})\n", 
					pViewEntity->scriptName(), eventName));

				switch(Network::g_trace_packet)	
				{
				case 1:
					mstream->hexlike();
					break;
				case 2:
					mstream->textlike();
					break;
				default:
					mstream->print_storage();
					break;
				};

				if(Network::g_trace_packet_use_logfile)
					DebugHelper::getSingleton().changeLogger(COMPONENT_NAME_EX(g_componentType));
			}

			ENTITY_MESSAGE_FORWARD_CLIENT_END(pSendBundle, msgHandler, viewEntityMessage);

			// 记录这个事件产生的数据量大小
			g_publicClientEventHistoryStats.trackEvent(pViewEntity->scriptName(),
				eventName,
				pSendBundle->currMsgLength() - snapshotLength,
				"::");

			pWitness->sendToClient(ClientInterface::onRemoteMethodCallOptimized, pSendBundle);
		}

	}
}

//-------------------------------------------------------------------------------------

}
