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


#if defined(DEFINE_IN_INTERFACE)
	#undef KBE_DBMGR_INTERFACE_H
#endif


#ifndef KBE_DBMGR_INTERFACE_H
#define KBE_DBMGR_INTERFACE_H

// common include	
#if defined(DBMGR)
#include "dbmgr.h"
#endif
#include "dbmgr_interface_macros.h"
#include "network/interface_defs.h"

//#define NDEBUG
// windows include	
#if KBE_PLATFORM == PLATFORM_WIN32
#else
// linux include
#endif
	
namespace KBEngine{

/**
	Dbmgr��Ϣ�꣬  ����Ϊ���� ��Ҫ�Լ��⿪
*/

/**
	DBMGR������Ϣ�ӿ��ڴ˶���
*/
NETWORK_INTERFACE_DECLARE_BEGIN(DbmgrInterface)
	// ĳappע���Լ��Ľӿڵ�ַ����app
	DBMGR_MESSAGE_DECLARE_ARGS11(onRegisterNewApp,					NETWORK_VARIABLE_MESSAGE,
									int32,							uid, 
									std::string,					username,
									COMPONENT_TYPE,					componentType, 
									COMPONENT_ID,					componentID, 
									COMPONENT_ORDER,				globalorderID,
									COMPONENT_ORDER,				grouporderID,
									uint32,							intaddr, 
									uint16,							intport,
									uint32,							extaddr, 
									uint16,							extport,
									std::string,					extaddrEx)

	// ĳapp��������look��
	DBMGR_MESSAGE_DECLARE_ARGS0(lookApp,							NETWORK_FIXED_MESSAGE)

	// ĳ��app����鿴��app����״̬��
	DBMGR_MESSAGE_DECLARE_ARGS0(queryLoad,							NETWORK_FIXED_MESSAGE)

	// ĳapp�����ȡһ��entityID�� 
	DBMGR_MESSAGE_DECLARE_ARGS2(onReqAllocEntityID,					NETWORK_FIXED_MESSAGE,
								COMPONENT_TYPE,						componentType,
								COMPONENT_ID,						componentID)

	// global���ݸı�
	DBMGR_MESSAGE_DECLARE_STREAM(onBroadcastGlobalDataChanged,		NETWORK_VARIABLE_MESSAGE)

	// ĳ��app��app��֪���ڻ״̬��
	DBMGR_MESSAGE_DECLARE_ARGS2(onAppActiveTick,					NETWORK_FIXED_MESSAGE,
									COMPONENT_TYPE,					componentType, 
									COMPONENT_ID,					componentID)

	// loginapp���󴴽��˺š�
	DBMGR_MESSAGE_DECLARE_STREAM(reqCreateAccount,					NETWORK_VARIABLE_MESSAGE)
	DBMGR_MESSAGE_DECLARE_STREAM(onCreateAccountCBFromInterfaces,	NETWORK_VARIABLE_MESSAGE)

	// ��½�˺š�
	DBMGR_MESSAGE_DECLARE_STREAM(onAccountLogin,					NETWORK_VARIABLE_MESSAGE)
	DBMGR_MESSAGE_DECLARE_STREAM(onLoginAccountCBBFromInterfaces,	NETWORK_VARIABLE_MESSAGE)

	// baseapp��ѯ�˺���Ϣ��
	DBMGR_MESSAGE_DECLARE_ARGS8(queryAccount,						NETWORK_VARIABLE_MESSAGE,
									std::string,					accountName,
									std::string,					password,
									bool,							needCheckPassword,
									COMPONENT_ID,					componentID,
									ENTITY_ID,						entityID,
									DBID,							entityDBID,
									uint32,							ip,
									uint16,							port)

	// baseapp���˺����ߡ�
	DBMGR_MESSAGE_DECLARE_ARGS3(onAccountOnline,					NETWORK_VARIABLE_MESSAGE,
									std::string,					accountName,
									COMPONENT_ID,					componentID,
									ENTITY_ID,						entityID)
		
	// baseapp��entity���ߡ�
	DBMGR_MESSAGE_DECLARE_ARGS3(onEntityOffline,					NETWORK_FIXED_MESSAGE,
									DBID,							dbid,
									uint16,							sid,
									uint16,							dbInterfaceIndex)

	// ��������ͻ�����������
	DBMGR_MESSAGE_DECLARE_ARGS1(eraseClientReq,						NETWORK_VARIABLE_MESSAGE,
									std::string,					logkey)

	// ���ݿ��ѯ
	DBMGR_MESSAGE_DECLARE_STREAM(executeRawDatabaseCommand,			NETWORK_VARIABLE_MESSAGE)

	// ĳ��entity�浵
	DBMGR_MESSAGE_DECLARE_STREAM(writeEntity,						NETWORK_VARIABLE_MESSAGE)

	// ɾ��ĳ��entity�Ĵ浵
	DBMGR_MESSAGE_DECLARE_STREAM(removeEntity,						NETWORK_VARIABLE_MESSAGE)

	// ��������ݿ�ɾ��ʵ��
	DBMGR_MESSAGE_DECLARE_STREAM(deleteEntityByDBID,				NETWORK_VARIABLE_MESSAGE)

	// ͨ��dbid��ѯһ��ʵ���Ƿ�����ݿ���
	DBMGR_MESSAGE_DECLARE_STREAM(lookUpEntityByDBID,				NETWORK_VARIABLE_MESSAGE)

	// ����رշ�����
	DBMGR_MESSAGE_DECLARE_STREAM(reqCloseServer,					NETWORK_VARIABLE_MESSAGE)

	// ĳ��app��app��֪���ڻ״̬��
	DBMGR_MESSAGE_DECLARE_ARGS7(queryEntity,						NETWORK_VARIABLE_MESSAGE, 
									uint16,							dbInterfaceIndex,
									COMPONENT_ID,					componentID,
									int8,							queryMode,
									DBID,							dbid, 
									std::string,					entityType,
									CALLBACK_ID,					callbackID,
									ENTITY_ID,						entityID)

	// ʵ���Զ����ع���
	DBMGR_MESSAGE_DECLARE_STREAM(entityAutoLoad,					NETWORK_VARIABLE_MESSAGE)

	// ͬ��entity��ģ��
	DBMGR_MESSAGE_DECLARE_STREAM(syncEntityStreamTemplate,			NETWORK_VARIABLE_MESSAGE)

	// �����ѯwatcher����
	DBMGR_MESSAGE_DECLARE_STREAM(queryWatcher,						NETWORK_VARIABLE_MESSAGE)

	// ��ֵ����
	DBMGR_MESSAGE_DECLARE_STREAM(charge,							NETWORK_VARIABLE_MESSAGE)

	// ��ֵ�ص�
	DBMGR_MESSAGE_DECLARE_STREAM(onChargeCB,						NETWORK_VARIABLE_MESSAGE)

	// ����ص���
	DBMGR_MESSAGE_DECLARE_ARGS1(accountActivate,					NETWORK_VARIABLE_MESSAGE,
									std::string,					scode)

	// �˺������������롣
	DBMGR_MESSAGE_DECLARE_ARGS1(accountReqResetPassword,			NETWORK_VARIABLE_MESSAGE,
									std::string,					accountName)

	// �˺�����������롣
	DBMGR_MESSAGE_DECLARE_ARGS3(accountResetPassword,				NETWORK_VARIABLE_MESSAGE,
									std::string,					accountName,
									std::string,					newpassword,
									std::string,					code)

	// �˺���������䡣
	DBMGR_MESSAGE_DECLARE_ARGS4(accountReqBindMail,					NETWORK_VARIABLE_MESSAGE,
									ENTITY_ID,						entityID,
									std::string,					accountName,
									std::string,					password,
									std::string,					email)

	// �˺���ɰ����䡣
	DBMGR_MESSAGE_DECLARE_ARGS2(accountBindMail,					NETWORK_VARIABLE_MESSAGE,
									std::string,					username,
									std::string,					code)

	// �˺��޸����롣
	DBMGR_MESSAGE_DECLARE_ARGS4(accountNewPassword,					NETWORK_VARIABLE_MESSAGE,
									ENTITY_ID,						entityID,
									std::string,					accountName,
									std::string,					password,
									std::string,					newpassword)

	// ��ʼprofile
	DBMGR_MESSAGE_DECLARE_STREAM(startProfile,						NETWORK_VARIABLE_MESSAGE)

	// ����ǿ��ɱ����ǰapp
	DBMGR_MESSAGE_DECLARE_STREAM(reqKillServer,						NETWORK_VARIABLE_MESSAGE)

NETWORK_INTERFACE_DECLARE_END()

#ifdef DEFINE_IN_INTERFACE
	#undef DEFINE_IN_INTERFACE
#endif

}
#endif
