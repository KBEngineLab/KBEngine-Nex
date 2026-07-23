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
	#undef KBE_BASEAPP_INTERFACE_H
#endif


#ifndef KBE_BASEAPP_INTERFACE_H
#define KBE_BASEAPP_INTERFACE_H

// common include	
#if defined(BASEAPP)
#include "baseapp.h"
#endif
#include "baseapp_interface_macros.h"
#include "entity_interface_macros.h"
#include "proxy_interface_macros.h"
#include "network/interface_defs.h"
#include "server/server_errors.h"
//#define NDEBUG
// windows include	
#if KBE_PLATFORM == PLATFORM_WIN32
#else
// linux include
#endif
	
namespace KBEngine{

/**
	BASEAPP������Ϣ�ӿ��ڴ˶���
*/
NETWORK_INTERFACE_DECLARE_BEGIN(BaseappInterface)
	// �ͻ���������Э�顣
	BASEAPP_MESSAGE_EXPOSED(importClientMessages)
	BASEAPP_MESSAGE_DECLARE_ARGS0(importClientMessages,								NETWORK_FIXED_MESSAGE)

	// �ͻ���entitydef������
	BASEAPP_MESSAGE_EXPOSED(importClientEntityDef)
	BASEAPP_MESSAGE_DECLARE_ARGS0(importClientEntityDef,							NETWORK_FIXED_MESSAGE)

	// ĳapp����������ߡ�
	BASEAPP_MESSAGE_DECLARE_ARGS0(reqClose,											NETWORK_FIXED_MESSAGE)

	// ĳapp��������look��
	BASEAPP_MESSAGE_DECLARE_ARGS0(lookApp,											NETWORK_FIXED_MESSAGE)

	// ĳ��app����鿴��app����״̬��
	BASEAPP_MESSAGE_DECLARE_ARGS0(queryLoad,										NETWORK_FIXED_MESSAGE)

	// consoleԶ��ִ��python��䡣
	BASEAPP_MESSAGE_DECLARE_STREAM(onExecScriptCommand,								NETWORK_VARIABLE_MESSAGE)

	// ĳappע���Լ��Ľӿڵ�ַ����app
	BASEAPP_MESSAGE_DECLARE_ARGS11(onRegisterNewApp,								NETWORK_VARIABLE_MESSAGE,
									int32,											uid, 
									std::string,									username,
									COMPONENT_TYPE,									componentType, 
									COMPONENT_ID,									componentID, 
									COMPONENT_ORDER,								globalorderID,
									COMPONENT_ORDER,								grouporderID,
									uint32,											intaddr, 
									uint16,											intport,
									uint32,											extaddr, 
									uint16,											extport,
									std::string,									extaddrEx)

	// dbmgr��֪�Ѿ�����������baseapp����cellapp�ĵ�ַ
	// ��ǰapp��Ҫ������ȥ�����ǽ�������
	BASEAPP_MESSAGE_DECLARE_ARGS11(onGetEntityAppFromDbmgr,							NETWORK_VARIABLE_MESSAGE,
									int32,											uid, 
									std::string,									username,
									COMPONENT_TYPE,									componentType, 
									COMPONENT_ID,									componentID, 
									COMPONENT_ORDER,								globalorderID,
									COMPONENT_ORDER,								grouporderID,
									uint32,											intaddr, 
									uint16,											intport,
									uint32,											extaddr, 
									uint16,											extport,
									std::string,									extaddrEx)

	// ĳapp�����ȡһ��entityID�εĻص�
	BASEAPP_MESSAGE_DECLARE_ARGS2(onReqAllocEntityID,								NETWORK_FIXED_MESSAGE,
									ENTITY_ID,										startID,
									ENTITY_ID,										endID)


	// ĳapp�����ȡһ��entityID�εĻص�
	BASEAPP_MESSAGE_DECLARE_ARGS6(onDbmgrInitCompleted,								NETWORK_VARIABLE_MESSAGE,
									GAME_TIME,										gametime, 
									ENTITY_ID,										startID,
									ENTITY_ID,										endID,
									COMPONENT_ORDER,								startGlobalOrder,
									COMPONENT_ORDER,								startGroupOrder,
									std::string,									digest)

	// hello���֡�
	BASEAPP_MESSAGE_EXPOSED(hello)
	BASEAPP_MESSAGE_DECLARE_STREAM(hello,											NETWORK_VARIABLE_MESSAGE)

	// global���ݸı�
	BASEAPP_MESSAGE_DECLARE_STREAM(onBroadcastGlobalDataChanged,					NETWORK_VARIABLE_MESSAGE)
	BASEAPP_MESSAGE_DECLARE_STREAM(onBroadcastBaseAppDataChanged,					NETWORK_VARIABLE_MESSAGE)

	// ĳ��app��app��֪���ڻ״̬��
	BASEAPP_MESSAGE_DECLARE_ARGS2(onAppActiveTick,									NETWORK_FIXED_MESSAGE,
									COMPONENT_TYPE,									componentType, 
									COMPONENT_ID,									componentID)

	// ĳ��app��app��֪���ڻ״̬��
	BASEAPP_MESSAGE_EXPOSED(onClientActiveTick)
	BASEAPP_MESSAGE_DECLARE_ARGS0(onClientActiveTick,								NETWORK_FIXED_MESSAGE)

	// �յ�baseappmgr������ĳ��baseappҪ��createEntityAnywhere�������ڱ�baseapp��ִ�� 
	BASEAPP_MESSAGE_DECLARE_STREAM(onCreateEntityAnywhere,							NETWORK_VARIABLE_MESSAGE)

	// createEntityAnywhere�ɹ�֮��ص���Ϣ�������createEntityAnywhere��baseapp��entity��
	BASEAPP_MESSAGE_DECLARE_STREAM(onCreateEntityAnywhereCallback,					NETWORK_FIXED_MESSAGE)

	// createEntityRemotely�ɹ�֮��ص���Ϣ�������createEntityRemotely��baseapp��entity��
	BASEAPP_MESSAGE_DECLARE_STREAM(onCreateEntityRemotely,							NETWORK_FIXED_MESSAGE)

	// createEntityRemotely�ɹ�֮��ص���Ϣ�������createEntityRemotely��baseapp��entity��
	BASEAPP_MESSAGE_DECLARE_STREAM(onCreateEntityRemotelyCallback,					 NETWORK_FIXED_MESSAGE)

	// createCellEntity��cellʵ�崴���ɹ��ص���
	BASEAPP_MESSAGE_DECLARE_ARGS3(onEntityGetCell,									NETWORK_FIXED_MESSAGE,
									ENTITY_ID,										id,
									COMPONENT_ID,									componentID,
									SPACE_ID,										spaceID)

	// createCellEntity��cellʵ�崴���ɹ��ص���
	BASEAPP_MESSAGE_DECLARE_ARGS1(onCreateCellFailure,								NETWORK_FIXED_MESSAGE,
									ENTITY_ID,										entityID)

	// loginapp���Լ�ע��һ����Ҫ��¼���˺�, ��baseappmgrת����
	BASEAPP_MESSAGE_DECLARE_STREAM(registerPendingLogin,							NETWORK_VARIABLE_MESSAGE)

	// ���ݿ��в�ѯ���Զ�entity������Ϣ���� 
	BASEAPP_MESSAGE_DECLARE_STREAM(onEntityAutoLoadCBFromDBMgr,						NETWORK_VARIABLE_MESSAGE)

	// ǰ�������¼�������ϡ�
	BASEAPP_MESSAGE_EXPOSED(loginBaseapp)
	BASEAPP_MESSAGE_DECLARE_ARGS2(loginBaseapp,										NETWORK_VARIABLE_MESSAGE,
									std::string,									accountName,
									std::string,									password)

	// ǰ����������صǳ���
	BASEAPP_MESSAGE_EXPOSED(logoutBaseapp)
	BASEAPP_MESSAGE_DECLARE_ARGS2(logoutBaseapp,									NETWORK_FIXED_MESSAGE,
									uint64,											key,
									ENTITY_ID,										entityID)

	// ǰ���������µ�¼�������ϡ�
	BASEAPP_MESSAGE_EXPOSED(reloginBaseapp)
	BASEAPP_MESSAGE_DECLARE_ARGS4(reloginBaseapp,									NETWORK_VARIABLE_MESSAGE,
									std::string,									accountName,
									std::string,									password,
									uint64,											key,
									ENTITY_ID,										entityID)

	// ��dbmgr��ȡ���˺�Entity��Ϣ
	BASEAPP_MESSAGE_DECLARE_STREAM(onQueryAccountCBFromDbmgr,						NETWORK_VARIABLE_MESSAGE)

	// entity�յ�Զ��call����, ��ĳ��app�ϵ�entitycall����
	BASEAPP_MESSAGE_DECLARE_STREAM(onEntityCall,									NETWORK_VARIABLE_MESSAGE)
	
	// client����entity��cell����
	BASEAPP_MESSAGE_EXPOSED(onRemoteCallCellMethodFromClient)
	BASEAPP_MESSAGE_DECLARE_STREAM(onRemoteCallCellMethodFromClient,				NETWORK_VARIABLE_MESSAGE)

	// client��������
	BASEAPP_MESSAGE_EXPOSED(onUpdateDataFromClient)
	BASEAPP_MESSAGE_DECLARE_STREAM(onUpdateDataFromClient,							NETWORK_VARIABLE_MESSAGE)
	BASEAPP_MESSAGE_EXPOSED(onUpdateDataFromClientForControlledEntity)
	BASEAPP_MESSAGE_DECLARE_STREAM(onUpdateDataFromClientForControlledEntity,		NETWORK_VARIABLE_MESSAGE)

	// executeRawDatabaseCommand��dbmgr�Ļص�
	BASEAPP_MESSAGE_DECLARE_STREAM(onExecuteRawDatabaseCommandCB,					NETWORK_VARIABLE_MESSAGE)

	// cellapp����entity��cell����
	BASEAPP_MESSAGE_DECLARE_STREAM(onBackupEntityCellData,							NETWORK_VARIABLE_MESSAGE)

	// cellapp writeToDB���
	BASEAPP_MESSAGE_DECLARE_STREAM(onCellWriteToDBCompleted,						NETWORK_VARIABLE_MESSAGE)

	// cellappת��entity��Ϣ��client
	BASEAPP_MESSAGE_DECLARE_STREAM(forwardMessageToClientFromCellapp,				NETWORK_VARIABLE_MESSAGE)

	// cellappת��entity��Ϣ��ĳ��baseEntity��cellEntity
	BASEAPP_MESSAGE_DECLARE_STREAM(forwardMessageToCellappFromCellapp,				NETWORK_VARIABLE_MESSAGE)

	// ����رշ�����
	BASEAPP_MESSAGE_DECLARE_STREAM(reqCloseServer,									NETWORK_VARIABLE_MESSAGE)

	// дentity��db�ص���
	BASEAPP_MESSAGE_DECLARE_ARGS5(onWriteToDBCallback,								NETWORK_FIXED_MESSAGE,
									ENTITY_ID,										eid,
									DBID,											entityDBID,
									uint16,											dbInterfaceIndex,
									CALLBACK_ID,									callbackID,
									bool,											success)

	// createEntityFromDBID�Ļص�
	BASEAPP_MESSAGE_DECLARE_STREAM(onCreateEntityFromDBIDCallback,					NETWORK_FIXED_MESSAGE)

	// createEntityAnywhereFromDBID�Ļص�
	BASEAPP_MESSAGE_DECLARE_STREAM(onGetCreateEntityAnywhereFromDBIDBestBaseappID,	NETWORK_FIXED_MESSAGE)

	// createEntityAnywhereFromDBID�Ļص�
	BASEAPP_MESSAGE_DECLARE_STREAM(onCreateEntityAnywhereFromDBIDCallback,			NETWORK_FIXED_MESSAGE)

	// createEntityAnywhereFromDBID�Ļص�
	BASEAPP_MESSAGE_DECLARE_STREAM(createEntityAnywhereFromDBIDOtherBaseapp,		NETWORK_FIXED_MESSAGE)

	// createEntityAnywhereFromDBID�Ļص�
	BASEAPP_MESSAGE_DECLARE_ARGS5(onCreateEntityAnywhereFromDBIDOtherBaseappCallback,NETWORK_VARIABLE_MESSAGE,
									COMPONENT_ID,									createByBaseappID,
									std::string,									entityType,
									ENTITY_ID,										createdEntityID,
									CALLBACK_ID,									callbackID,
									DBID,											dbid)

	// createEntityRemotelyFromDBID�Ļص�
	BASEAPP_MESSAGE_DECLARE_STREAM(onCreateEntityRemotelyFromDBIDCallback,			NETWORK_FIXED_MESSAGE)

	// createEntityRemotelyFromDBID�Ļص�
	BASEAPP_MESSAGE_DECLARE_STREAM(createEntityRemotelyFromDBIDOtherBaseapp,		NETWORK_FIXED_MESSAGE)

	// createEntityRemotelyFromDBID�Ļص�
	BASEAPP_MESSAGE_DECLARE_ARGS5(onCreateEntityRemotelyFromDBIDOtherBaseappCallback,NETWORK_VARIABLE_MESSAGE,
									COMPONENT_ID,									createByBaseappID,
									std::string,									entityType,
									ENTITY_ID,										createdEntityID,
									CALLBACK_ID,									callbackID,
									DBID,											dbid)

	// �����ѯwatcher����
	BASEAPP_MESSAGE_DECLARE_STREAM(queryWatcher,									NETWORK_VARIABLE_MESSAGE)

	// ��ֵ�ص�
	BASEAPP_MESSAGE_DECLARE_STREAM(onChargeCB,										NETWORK_VARIABLE_MESSAGE)

	// ��ʼprofile
	BASEAPP_MESSAGE_DECLARE_STREAM(startProfile,									NETWORK_VARIABLE_MESSAGE)

	// ��������ݿ�ɾ��ʵ��
	BASEAPP_MESSAGE_DECLARE_STREAM(deleteEntityByDBIDCB,							NETWORK_VARIABLE_MESSAGE)
	
	// lookUpEntityByDBID�Ļص�
	BASEAPP_MESSAGE_DECLARE_STREAM(lookUpEntityByDBIDCB,							NETWORK_VARIABLE_MESSAGE)

	// ĳ��baseapp�ϵ�space�ָ���cell�� �жϵ�ǰbaseapp�Ƿ������entity��Ҫ�ָ�cell
	BASEAPP_MESSAGE_DECLARE_STREAM(onRestoreSpaceCellFromOtherBaseapp,				NETWORK_VARIABLE_MESSAGE)

	// ����������APP���ѻָ����ؽ����
	BASEAPP_MESSAGE_DECLARE_STREAM(onRequestRestoreCB,								NETWORK_VARIABLE_MESSAGE)

	// �����email
	BASEAPP_MESSAGE_EXPOSED(reqAccountBindEmail)
	BASEAPP_MESSAGE_DECLARE_ARGS3(reqAccountBindEmail,								NETWORK_VARIABLE_MESSAGE,
									ENTITY_ID,										entityID,
									std::string,									password,
									std::string,									email)

	// �����email����Ļص�
	BASEAPP_MESSAGE_DECLARE_ARGS5(onReqAccountBindEmailCBFromDBMgr,					NETWORK_VARIABLE_MESSAGE,
									ENTITY_ID,										entityID,
									std::string,									accountName,
									std::string,									email,
									SERVER_ERROR_CODE,								failedcode,
									std::string,									code)

	// baseapp�����email������ʱ��Ҫ�ҵ�loginapp�ĵ�ַ��
	BASEAPP_MESSAGE_DECLARE_ARGS7(onReqAccountBindEmailCBFromBaseappmgr,			NETWORK_VARIABLE_MESSAGE,
									ENTITY_ID,										entityID,
									std::string,									accountName,
									std::string,									email,
									SERVER_ERROR_CODE,								failedcode,
									std::string,									code,
									std::string,									loginappCBHost,
									uint16,											loginappCBPort)

	// �����޸�����
	BASEAPP_MESSAGE_EXPOSED(reqAccountNewPassword)
	BASEAPP_MESSAGE_DECLARE_ARGS3(reqAccountNewPassword,							NETWORK_VARIABLE_MESSAGE,
									ENTITY_ID,										entityID,
									std::string,									oldpassword,
									std::string,									newpassword)

	// �����޸�����Ļص�
	BASEAPP_MESSAGE_DECLARE_ARGS3(onReqAccountNewPasswordCB,						NETWORK_VARIABLE_MESSAGE,
									ENTITY_ID,										entityID,
									std::string,									accountName,
									SERVER_ERROR_CODE,								failedcode)

	// ����ǿ��ɱ����ǰapp
	BASEAPP_MESSAGE_DECLARE_STREAM(reqKillServer,									NETWORK_VARIABLE_MESSAGE)

	//--------------------------------------------Entity----------------------------------------------------------
	// Զ�̺���entity����
	ENTITY_MESSAGE_EXPOSED(onRemoteMethodCall)
	ENTITY_MESSAGE_DECLARE_STREAM(onRemoteMethodCall,								NETWORK_VARIABLE_MESSAGE)

	// cellappͨ����entity��cell�������ٻ��߶�ʧ
	ENTITY_MESSAGE_DECLARE_STREAM(onLoseCell,										NETWORK_VARIABLE_MESSAGE)

	// �ͻ���ֱ�ӷ�����Ϣ��cellʵ��
	ENTITY_MESSAGE_EXPOSED(forwardEntityMessageToCellappFromClient)
	ENTITY_MESSAGE_DECLARE_STREAM(forwardEntityMessageToCellappFromClient,			NETWORK_VARIABLE_MESSAGE)

	// ĳ��entity����teleport��Ļص����
	ENTITY_MESSAGE_DECLARE_ARGS2(onTeleportCB,										NETWORK_FIXED_MESSAGE,
								SPACE_ID,											spaceID,
								bool,												fromCellTeleport)

	// ĳ��entity����teleport��Ļص����
	ENTITY_MESSAGE_DECLARE_ARGS1(onGetDBID,											NETWORK_FIXED_MESSAGE,
								DBID,												dbid)

	// entity����Ǩ�Ƶ���һ��cellapp�ϵ�space���̿�ʼ
	ENTITY_MESSAGE_DECLARE_ARGS2(onMigrationCellappStart,							NETWORK_FIXED_MESSAGE,
								COMPONENT_ID,										sourceCellAppID,
								COMPONENT_ID,										targetCellAppID)
		
	// entity����Ǩ�Ƶ���һ��cellapp�ϵ�space���̽���
	ENTITY_MESSAGE_DECLARE_ARGS2(onMigrationCellappEnd,								NETWORK_FIXED_MESSAGE,
								COMPONENT_ID,										sourceCellAppID,
								COMPONENT_ID,										targetCellAppID)

	//--------------------------------------------Proxy---------------------------------------------------------
	/**
		Զ�̺���entity����
	*/
	//PROXY_MESSAGE_EXPOSED(onClientGetCell)
	//PROXY_MESSAGE_DECLARE_ARGS0(onClientGetCell,									NETWORK_FIXED_MESSAGE)

NETWORK_INTERFACE_DECLARE_END()

#ifdef DEFINE_IN_INTERFACE
	#undef DEFINE_IN_INTERFACE
#endif

}
#endif
