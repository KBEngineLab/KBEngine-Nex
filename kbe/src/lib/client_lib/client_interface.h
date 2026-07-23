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
	#undef KBE_CLIENT_INTERFACE_H
#endif


#ifndef KBE_CLIENT_INTERFACE_H
#define KBE_CLIENT_INTERFACE_H

// common include	
#if defined(CLIENT)
#include "clientapp.h"
#endif
#include "client_interface_macros.h"
#include "network/interface_defs.h"
#include "server/server_errors.h"
#include "entitydef/common.h"
#include "common.h"
	
namespace KBEngine{

/**
	CLIENT������Ϣ�ӿ��ڴ˶���
*/
NETWORK_INTERFACE_DECLARE_BEGIN(ClientInterface)
	// �����hello���ء�
	CLIENT_MESSAGE_DECLARE_STREAM(onHelloCB,								NETWORK_VARIABLE_MESSAGE)

	// �ͷ���˵İ汾��ƥ��
	CLIENT_MESSAGE_DECLARE_STREAM(onVersionNotMatch,						NETWORK_VARIABLE_MESSAGE)

	// �ͷ���˵Ľű���汾��ƥ��
	CLIENT_MESSAGE_DECLARE_STREAM(onScriptVersionNotMatch,					NETWORK_VARIABLE_MESSAGE)

	// �����˺�ʧ�ܡ�
	CLIENT_MESSAGE_DECLARE_STREAM(onCreateAccountResult,					NETWORK_VARIABLE_MESSAGE)

	// ��¼�ɹ���
	CLIENT_MESSAGE_DECLARE_STREAM(onLoginSuccessfully,						NETWORK_VARIABLE_MESSAGE)

	// ��¼ʧ�ܡ�
	CLIENT_MESSAGE_DECLARE_STREAM(onLoginFailed,							NETWORK_VARIABLE_MESSAGE)

	// ���������Ѿ�������һ����ͻ��˹����Ĵ���Entity || ��¼���سɹ���
	CLIENT_MESSAGE_DECLARE_ARGS3(onCreatedProxies,							NETWORK_VARIABLE_MESSAGE,
								uint64,										rndUUID,
								ENTITY_ID,									eid,
								std::string,								entityType)

	// ��¼����ʧ�ܡ�
	CLIENT_MESSAGE_DECLARE_ARGS1(onLoginBaseappFailed,						NETWORK_FIXED_MESSAGE,
								SERVER_ERROR_CODE,							failedcode)

	// ��¼����ʧ�ܡ�
	CLIENT_MESSAGE_DECLARE_ARGS1(onReloginBaseappFailed,					NETWORK_FIXED_MESSAGE,
								SERVER_ERROR_CODE,							failedcode)

	// �������ϵ�entity�Ѿ�������Ϸ�����ˡ�
	CLIENT_MESSAGE_DECLARE_STREAM(onEntityEnterWorld,						NETWORK_VARIABLE_MESSAGE)

	// �������ϵ�entity�Ѿ��뿪��Ϸ�����ˡ�
	CLIENT_MESSAGE_DECLARE_ARGS1(onEntityLeaveWorld,						NETWORK_FIXED_MESSAGE,
								ENTITY_ID,									eid)

	// �������ϵ�entity�Ѿ��뿪��Ϸ�����ˡ�
	CLIENT_MESSAGE_DECLARE_STREAM(onEntityLeaveWorldOptimized,				NETWORK_VARIABLE_MESSAGE)

	// ���߿ͻ���ĳ��entity�����ˣ� ����entityͨ���ǻ�δonEntityEnterWorld��
	CLIENT_MESSAGE_DECLARE_ARGS1(onEntityDestroyed,							NETWORK_FIXED_MESSAGE,
								ENTITY_ID,									eid)

	// �������ϵ�entity�Ѿ�����space�ˡ�
	CLIENT_MESSAGE_DECLARE_STREAM(onEntityEnterSpace,						NETWORK_VARIABLE_MESSAGE)

	// �������ϵ�entity�Ѿ��뿪space�ˡ�
	CLIENT_MESSAGE_DECLARE_ARGS1(onEntityLeaveSpace,						NETWORK_FIXED_MESSAGE,
								ENTITY_ID,									eid)

	// Զ�̺���entity����
	CLIENT_MESSAGE_DECLARE_STREAM(onRemoteMethodCall,						NETWORK_VARIABLE_MESSAGE)
	CLIENT_MESSAGE_DECLARE_STREAM(onRemoteMethodCallOptimized,				NETWORK_VARIABLE_MESSAGE)

	// ���߳�������
	CLIENT_MESSAGE_DECLARE_ARGS1(onKicked,									NETWORK_FIXED_MESSAGE,
								SERVER_ERROR_CODE,							failedcode)

	// ����������entity����
	CLIENT_MESSAGE_DECLARE_STREAM(onUpdatePropertys,						NETWORK_VARIABLE_MESSAGE)
	CLIENT_MESSAGE_DECLARE_STREAM(onUpdatePropertysOptimized,				NETWORK_VARIABLE_MESSAGE)

	// ������ǿ������entity��λ���볯��
	CLIENT_MESSAGE_DECLARE_STREAM(onSetEntityPosAndDir,						NETWORK_VARIABLE_MESSAGE)

	// ���������°�
	CLIENT_MESSAGE_DECLARE_ARGS3(onUpdateBasePos,							NETWORK_FIXED_MESSAGE,
								float,										x,
								float,										y,
								float,										z)
	CLIENT_MESSAGE_DECLARE_STREAM(onUpdateBaseDir,							NETWORK_VARIABLE_MESSAGE)

	CLIENT_MESSAGE_DECLARE_ARGS2(onUpdateBasePosXZ,							NETWORK_FIXED_MESSAGE,
								float,										x,
								float,										z)

	CLIENT_MESSAGE_DECLARE_STREAM(onUpdateData,								NETWORK_VARIABLE_MESSAGE)

	CLIENT_MESSAGE_DECLARE_STREAM(onUpdateData_ypr,							NETWORK_VARIABLE_MESSAGE)
	CLIENT_MESSAGE_DECLARE_STREAM(onUpdateData_yp,							NETWORK_VARIABLE_MESSAGE)
	CLIENT_MESSAGE_DECLARE_STREAM(onUpdateData_yr,							NETWORK_VARIABLE_MESSAGE)
	CLIENT_MESSAGE_DECLARE_STREAM(onUpdateData_pr,							NETWORK_VARIABLE_MESSAGE)
	CLIENT_MESSAGE_DECLARE_STREAM(onUpdateData_y,							NETWORK_VARIABLE_MESSAGE)
	CLIENT_MESSAGE_DECLARE_STREAM(onUpdateData_p,							NETWORK_VARIABLE_MESSAGE)
	CLIENT_MESSAGE_DECLARE_STREAM(onUpdateData_r,							NETWORK_VARIABLE_MESSAGE)

	CLIENT_MESSAGE_DECLARE_STREAM(onUpdateData_xz,							NETWORK_VARIABLE_MESSAGE)
	CLIENT_MESSAGE_DECLARE_STREAM(onUpdateData_xz_ypr,						NETWORK_VARIABLE_MESSAGE)
	CLIENT_MESSAGE_DECLARE_STREAM(onUpdateData_xz_yp,						NETWORK_VARIABLE_MESSAGE)
	CLIENT_MESSAGE_DECLARE_STREAM(onUpdateData_xz_yr,						NETWORK_VARIABLE_MESSAGE)
	CLIENT_MESSAGE_DECLARE_STREAM(onUpdateData_xz_pr,						NETWORK_VARIABLE_MESSAGE)
	CLIENT_MESSAGE_DECLARE_STREAM(onUpdateData_xz_y,						NETWORK_VARIABLE_MESSAGE)
	CLIENT_MESSAGE_DECLARE_STREAM(onUpdateData_xz_p,						NETWORK_VARIABLE_MESSAGE)
	CLIENT_MESSAGE_DECLARE_STREAM(onUpdateData_xz_r,						NETWORK_VARIABLE_MESSAGE)

	CLIENT_MESSAGE_DECLARE_STREAM(onUpdateData_xyz,							NETWORK_VARIABLE_MESSAGE)
	CLIENT_MESSAGE_DECLARE_STREAM(onUpdateData_xyz_ypr,						NETWORK_VARIABLE_MESSAGE)
	CLIENT_MESSAGE_DECLARE_STREAM(onUpdateData_xyz_yp,						NETWORK_VARIABLE_MESSAGE)
	CLIENT_MESSAGE_DECLARE_STREAM(onUpdateData_xyz_yr,						NETWORK_VARIABLE_MESSAGE)
	CLIENT_MESSAGE_DECLARE_STREAM(onUpdateData_xyz_pr,						NETWORK_VARIABLE_MESSAGE)
	CLIENT_MESSAGE_DECLARE_STREAM(onUpdateData_xyz_y,						NETWORK_VARIABLE_MESSAGE)
	CLIENT_MESSAGE_DECLARE_STREAM(onUpdateData_xyz_p,						NETWORK_VARIABLE_MESSAGE)
	CLIENT_MESSAGE_DECLARE_STREAM(onUpdateData_xyz_r,						NETWORK_VARIABLE_MESSAGE)

	CLIENT_MESSAGE_DECLARE_STREAM(onUpdateData_ypr_optimized,				NETWORK_VARIABLE_MESSAGE)
	CLIENT_MESSAGE_DECLARE_STREAM(onUpdateData_yp_optimized,				NETWORK_VARIABLE_MESSAGE)
	CLIENT_MESSAGE_DECLARE_STREAM(onUpdateData_yr_optimized,				NETWORK_VARIABLE_MESSAGE)
	CLIENT_MESSAGE_DECLARE_STREAM(onUpdateData_pr_optimized,				NETWORK_VARIABLE_MESSAGE)
	CLIENT_MESSAGE_DECLARE_STREAM(onUpdateData_y_optimized,					NETWORK_VARIABLE_MESSAGE)
	CLIENT_MESSAGE_DECLARE_STREAM(onUpdateData_p_optimized,					NETWORK_VARIABLE_MESSAGE)
	CLIENT_MESSAGE_DECLARE_STREAM(onUpdateData_r_optimized,					NETWORK_VARIABLE_MESSAGE)

	CLIENT_MESSAGE_DECLARE_STREAM(onUpdateData_xz_optimized,				NETWORK_VARIABLE_MESSAGE)
	CLIENT_MESSAGE_DECLARE_STREAM(onUpdateData_xz_ypr_optimized,			NETWORK_VARIABLE_MESSAGE)
	CLIENT_MESSAGE_DECLARE_STREAM(onUpdateData_xz_yp_optimized,				NETWORK_VARIABLE_MESSAGE)
	CLIENT_MESSAGE_DECLARE_STREAM(onUpdateData_xz_yr_optimized,				NETWORK_VARIABLE_MESSAGE)
	CLIENT_MESSAGE_DECLARE_STREAM(onUpdateData_xz_pr_optimized,				NETWORK_VARIABLE_MESSAGE)
	CLIENT_MESSAGE_DECLARE_STREAM(onUpdateData_xz_y_optimized,				NETWORK_VARIABLE_MESSAGE)
	CLIENT_MESSAGE_DECLARE_STREAM(onUpdateData_xz_p_optimized,				NETWORK_VARIABLE_MESSAGE)
	CLIENT_MESSAGE_DECLARE_STREAM(onUpdateData_xz_r_optimized,				NETWORK_VARIABLE_MESSAGE)

	CLIENT_MESSAGE_DECLARE_STREAM(onUpdateData_xyz_optimized,				NETWORK_VARIABLE_MESSAGE)
	CLIENT_MESSAGE_DECLARE_STREAM(onUpdateData_xyz_ypr_optimized,			NETWORK_VARIABLE_MESSAGE)
	CLIENT_MESSAGE_DECLARE_STREAM(onUpdateData_xyz_yp_optimized,			NETWORK_VARIABLE_MESSAGE)
	CLIENT_MESSAGE_DECLARE_STREAM(onUpdateData_xyz_yr_optimized,			NETWORK_VARIABLE_MESSAGE)
	CLIENT_MESSAGE_DECLARE_STREAM(onUpdateData_xyz_pr_optimized,			NETWORK_VARIABLE_MESSAGE)
	CLIENT_MESSAGE_DECLARE_STREAM(onUpdateData_xyz_y_optimized,				NETWORK_VARIABLE_MESSAGE)
	CLIENT_MESSAGE_DECLARE_STREAM(onUpdateData_xyz_p_optimized,				NETWORK_VARIABLE_MESSAGE)
	CLIENT_MESSAGE_DECLARE_STREAM(onUpdateData_xyz_r_optimized,				NETWORK_VARIABLE_MESSAGE)

	// download stream��ʼ�� 
	CLIENT_MESSAGE_DECLARE_ARGS3(onStreamDataStarted,						NETWORK_VARIABLE_MESSAGE,
								int16,										id,
								uint32,										datasize,
								std::string,								descr)

	// ���յ�streamData
	CLIENT_MESSAGE_DECLARE_STREAM(onStreamDataRecv,							NETWORK_VARIABLE_MESSAGE)

	// download stream����� 
	CLIENT_MESSAGE_DECLARE_ARGS1(onStreamDataCompleted,						NETWORK_FIXED_MESSAGE,
								int16,										id)

	// ����Э��
	CLIENT_MESSAGE_DECLARE_STREAM(onImportClientMessages,					NETWORK_VARIABLE_MESSAGE)
	
	// ����entitydef
	CLIENT_MESSAGE_DECLARE_STREAM(onImportClientEntityDef,					NETWORK_VARIABLE_MESSAGE)

	// ��������������
	CLIENT_MESSAGE_DECLARE_STREAM(onImportServerErrorsDescr,				NETWORK_VARIABLE_MESSAGE)

	// ���յ���sdk��Ϣ
	CLIENT_MESSAGE_DECLARE_STREAM(onImportClientSDK,						NETWORK_VARIABLE_MESSAGE)

	// ����˳�ʼ��spacedata
	CLIENT_MESSAGE_DECLARE_STREAM(initSpaceData,							NETWORK_VARIABLE_MESSAGE)

	// �����������spacedata
	CLIENT_MESSAGE_DECLARE_ARGS3(setSpaceData,								NETWORK_VARIABLE_MESSAGE,
								SPACE_ID,									spaceID,
								std::string,								key,
								std::string,								val)

	// �����ɾ����spacedata
	CLIENT_MESSAGE_DECLARE_ARGS2(delSpaceData,								NETWORK_VARIABLE_MESSAGE,
								SPACE_ID,									spaceID,
								std::string,								key)

	// �����˺��������󷵻�
	CLIENT_MESSAGE_DECLARE_ARGS1(onReqAccountResetPasswordCB,				NETWORK_FIXED_MESSAGE,
								SERVER_ERROR_CODE,							failedcode)

	// �����˺��������󷵻�
	CLIENT_MESSAGE_DECLARE_ARGS1(onReqAccountBindEmailCB,					NETWORK_FIXED_MESSAGE,
								SERVER_ERROR_CODE,							failedcode)

	// �����˺��������󷵻�
	CLIENT_MESSAGE_DECLARE_ARGS1(onReqAccountNewPasswordCB,					NETWORK_FIXED_MESSAGE,
								SERVER_ERROR_CODE,							failedcode)

	// �ص�½���سɹ� 
	CLIENT_MESSAGE_DECLARE_STREAM(onReloginBaseappSuccessfully,				NETWORK_VARIABLE_MESSAGE)
									
	// ���߿ͻ��ˣ��㵱ǰ���𣨻�ȡ��������˭��λ��ͬ��
	CLIENT_MESSAGE_DECLARE_ARGS2(onControlEntity,							NETWORK_FIXED_MESSAGE,
									ENTITY_ID,								eid,
									int8,									isControlled)

	// �����������ص�
	CLIENT_MESSAGE_DECLARE_ARGS0(onAppActiveTickCB,							NETWORK_FIXED_MESSAGE)

	NETWORK_INTERFACE_DECLARE_END()

#ifdef DEFINE_IN_INTERFACE
	#undef DEFINE_IN_INTERFACE
#endif

}

#endif // KBE_CLIENT_INTERFACE_H
