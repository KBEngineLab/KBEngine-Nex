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

#ifndef KBE_SERVER_COMMON_H
#define KBE_SERVER_COMMON_H

#include "common/timer.h"
#include "common/common.h"
#include "server/server_errors.h"


namespace KBEngine { 

// ��Ϣת����ĳ�����
#define NETWORK_MESSAGE_FORWARD(SEND_INTERFACE, SENDBUNDLE, FORWARDBUNDLE, MYCOMPONENT_ID, FORWARD_COMPONENT_ID)						\
	SENDBUNDLE.newMessage(SEND_INTERFACE::forwardMessage);																				\
	SENDBUNDLE << MYCOMPONENT_ID << FORWARD_COMPONENT_ID;																				\
	FORWARDBUNDLE.finiMessage(true);																									\
	SENDBUNDLE.append(FORWARDBUNDLE);																									\

// cellappת����Ϣ���ͻ���
#define NETWORK_ENTITY_MESSAGE_FORWARD_CLIENT(ENTITYID, SENDBUNDLE, FORWARDBUNDLE)														\
	SENDBUNDLE.newMessage(BaseappInterface::forwardMessageToClientFromCellapp);															\
	SENDBUNDLE << ENTITYID;																												\
	FORWARDBUNDLE.finiMessage(true);																									\
	SENDBUNDLE.append(FORWARDBUNDLE);																									\

// cellappת����Ϣ��cellapp
#define NETWORK_ENTITY_MESSAGE_FORWARD_CELLAPP(ENTITYID, SENDBUNDLE, FORWARDBUNDLE)														\
	SENDBUNDLE.newMessage(BaseappInterface::forwardMessageToCellappFromCellapp);														\
	SENDBUNDLE << ENTITYID;																												\
	FORWARDBUNDLE.finiMessage(true);																									\
	SENDBUNDLE.append(FORWARDBUNDLE);	

// cellappת����Ϣ���ͻ��˿�ʼ
#define NETWORK_ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(ENTITYID, SENDBUNDLE)																\
	SENDBUNDLE.newMessage(BaseappInterface::forwardMessageToClientFromCellapp);															\
	SENDBUNDLE << ENTITYID;																												\

// cellappת����Ϣ���ͻ�����Ϣ��׷����Ϣ
#define NETWORK_ENTITY_MESSAGE_FORWARD_CLIENT_APPEND(SENDBUNDLE, FORWARDBUNDLE)															\
	FORWARDBUNDLE.finiMessage(true);																									\
	SENDBUNDLE.append(FORWARDBUNDLE);																									\

#define ENTITY_MESSAGE_FORWARD_CLIENT_END(SENDBUNDLE, MESSAGEHANDLE, ACTIONNAME)														\
{																																		\
	size_t messageLength = SENDBUNDLE->currMsgLength() - messageLength_last_##ACTIONNAME;												\
	Network::Packet* pCurrPacket = SENDBUNDLE->pCurrPacket();																			\
																																		\
	if(MESSAGEHANDLE.msgLen == NETWORK_VARIABLE_MESSAGE)																				\
	{																																	\
		if(messageLength >= NETWORK_MESSAGE_MAX_SIZE)																					\
		{																																\
			Network::MessageLength1 ex_msg_length = messageLength;																		\
			KBEngine::EndianConvert(ex_msg_length);																						\
																																		\
			Network::MessageLength msgLen = NETWORK_MESSAGE_MAX_SIZE;																	\
			KBEngine::EndianConvert(msgLen);																							\
																																		\
			memcpy(&pCurrPacket_##ACTIONNAME->data()[currMsgLengthPos_##ACTIONNAME], 													\
				(uint8*)&msgLen, NETWORK_MESSAGE_LENGTH_SIZE);																			\
																																		\
			pCurrPacket_##ACTIONNAME->insert(currMsgLengthPos_##ACTIONNAME + NETWORK_MESSAGE_LENGTH_SIZE, 								\
											(uint8*)&ex_msg_length, NETWORK_MESSAGE_LENGTH1_SIZE);										\
			SENDBUNDLE->currMsgLength(SENDBUNDLE->currMsgLength() + NETWORK_MESSAGE_LENGTH1_SIZE);										\
		}																																\
		else																															\
		{																																\
			Network::MessageLength msgLen = messageLength;																				\
			KBEngine::EndianConvert(msgLen);																							\
																																		\
			memcpy(&pCurrPacket_##ACTIONNAME->data()[currMsgLengthPos_##ACTIONNAME], 													\
				(uint8*)&msgLen, NETWORK_MESSAGE_LENGTH_SIZE);																			\
		}																																\
	}																																	\
																																		\
	Network::NetworkStats::getSingleton().trackMessage(Network::NetworkStats::SEND, MESSAGEHANDLE, messageLength);						\
																																		\
	if (Network::g_trace_packet > 0)																									\
		Network::Bundle::debugCurrentMessages(MESSAGEHANDLE.msgID, &MESSAGEHANDLE, 														\
				pCurrPacket, SENDBUNDLE->packets(), messageLength, SENDBUNDLE->pChannel());												\
}																																		\


// cellappת����Ϣ���ͻ�����Ϣ��׷����Ϣ(ֱ����SENDBUNDLE׷��)
#define ENTITY_MESSAGE_FORWARD_CLIENT_BEGIN(SENDBUNDLE, MESSAGEHANDLE, ACTIONNAME)														\
	(*SENDBUNDLE) << MESSAGEHANDLE.msgID;																								\
	size_t currMsgLengthPos_##ACTIONNAME = 0;																							\
	Network::Packet* pCurrPacket_##ACTIONNAME = SENDBUNDLE->pCurrPacket();																\
	if(MESSAGEHANDLE.msgLen == NETWORK_VARIABLE_MESSAGE)																				\
	{																																	\
		if(SENDBUNDLE->packetMaxSize() - pCurrPacket_##ACTIONNAME->wpos() - 1 < NETWORK_MESSAGE_LENGTH_SIZE)							\
		{																																\
			SENDBUNDLE->finiCurrPacket();																								\
			SENDBUNDLE->newPacket();																									\
			pCurrPacket_##ACTIONNAME = SENDBUNDLE->pCurrPacket();																		\
		}																																\
																																		\
		Network::MessageLength msglen = 0;																								\
		currMsgLengthPos_##ACTIONNAME = pCurrPacket_##ACTIONNAME->wpos();																\
		(*SENDBUNDLE) << msglen;																										\
	}																																	\
																																		\
	size_t messageLength_last_##ACTIONNAME = SENDBUNDLE->currMsgLength();																\


// ������Ϣ
#define COMMON_NETWORK_MESSAGE(COMPONENTTYPE, BUNDLE, MESSAGENAME)											\
		switch(COMPONENTTYPE)																				\
		{																									\
		case CELLAPPMGR_TYPE:																				\
			{																								\
				BUNDLE.newMessage(CellappmgrInterface::MESSAGENAME);										\
			}																								\
			break;																							\
		case BASEAPPMGR_TYPE:																				\
			{																								\
				BUNDLE.newMessage(BaseappmgrInterface::MESSAGENAME);										\
			}																								\
			break;																							\
		case DBMGR_TYPE:																					\
			{																								\
				BUNDLE.newMessage(DbmgrInterface::MESSAGENAME);												\
			}																								\
			break;																							\
		case CELLAPP_TYPE:																					\
			{																								\
				BUNDLE.newMessage(CellappInterface::MESSAGENAME);											\
			}																								\
			break;																							\
		case BASEAPP_TYPE:																					\
			{																								\
				BUNDLE.newMessage(BaseappInterface::MESSAGENAME);											\
			}																								\
			break;																							\
		case LOGINAPP_TYPE:																					\
			{																								\
				BUNDLE.newMessage(LoginappInterface::MESSAGENAME);											\
			}																								\
			break;																							\
		case LOGGER_TYPE:																					\
			{																								\
				BUNDLE.newMessage(LoggerInterface::MESSAGENAME);											\
			}																								\
			break;																							\
		case INTERFACES_TYPE:																				\
			{																								\
				BUNDLE.newMessage(InterfacesInterface::MESSAGENAME);										\
			}																								\
			break;																							\
		default:																							\
			ERROR_MSG(fmt::format("not support componentType={}({})\n",										\
					COMPONENTTYPE, COMPONENT_NAME_EX(COMPONENTTYPE)));										\
			KBE_ASSERT(false && "not support componentType!\n");											\
			break;																							\
		};																									\


#define ENTITTAPP_COMMON_NETWORK_MESSAGE(COMPONENTTYPE, BUNDLE, MESSAGENAME)								\
		switch(COMPONENTTYPE)																				\
		{																									\
		case CELLAPP_TYPE:																					\
			{																								\
				BUNDLE.newMessage(CellappInterface::MESSAGENAME);											\
			}																								\
			break;																							\
		case BASEAPP_TYPE:																					\
			{																								\
				BUNDLE.newMessage(BaseappInterface::MESSAGENAME);											\
			}																								\
			break;																							\
		default:																							\
			ERROR_MSG(fmt::format("not support componentType={}({})\n",										\
					COMPONENTTYPE, COMPONENT_NAME_EX(COMPONENTTYPE)));										\
			KBE_ASSERT(false && "not support componentType!\n");											\
			break;																							\
		};																									\

/**
����ת��Ϊtick
@lowerBound: ���ٲ�����Ntick
*/
int32 secondsToTicks(float seconds, int lowerBound);

/**
	����Ϊ��λ��ʱ��ת��Ϊÿ�����ĵ�stamps
*/
inline uint64 secondsToStamps(float seconds)
{
	return (uint64)(seconds * stampsPerSecondD());
}

void autoFixUserDigestUID();

/*
 �˺ź�������󳤶�
*/
#define ACCOUNT_NAME_MAX_LENGTH						128
#define ACCOUNT_PASSWD_MAX_LENGTH					255

// ��¼ע��ʱ��������Ϣ��󳤶�
#define ACCOUNT_DATA_MAX_LENGTH						1024

// �����������κ�ֻ��һ�κ��Զ�����Ϊ������ѡ��
#define KBE_NEXT_ONLY								2

/** c/c++�������ת����KBEDataTypeID */
#define KBE_DATATYPE2ID_MAX							21
uint16 datatype2id(std::string datatype);

/** c/c++�������ת����ԭ�����UINT16 ... */
std::string datatype2nativetype(std::string datatype);
std::string datatype2nativetype(uint16 datatype);

int getMacMD5();
int getMD5(std::string data);

}

#endif // KBE_SERVER_COMMON_H
