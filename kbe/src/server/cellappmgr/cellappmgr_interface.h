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
	#undef KBE_CELLAPPMGR_INTERFACE_H
#endif


#ifndef KBE_CELLAPPMGR_INTERFACE_H
#define KBE_CELLAPPMGR_INTERFACE_H

// common include	
#if defined(CELLAPPMGR)
#include "cellappmgr.h"
#endif
#include "cellappmgr_interface_macros.h"
#include "network/interface_defs.h"
//#define NDEBUG
// windows include	
#if KBE_PLATFORM == PLATFORM_WIN32
#else
// linux include
#endif
	
namespace KBEngine{

/**
	BASEAPPMGR������Ϣ�ӿ��ڴ˶���
*/
NETWORK_INTERFACE_DECLARE_BEGIN(CellappmgrInterface)
	// ĳappע���Լ��Ľӿڵ�ַ����app
	CELLAPPMGR_MESSAGE_DECLARE_ARGS11(onRegisterNewApp,						NETWORK_VARIABLE_MESSAGE,
									int32,									uid, 
									std::string,							username,
									COMPONENT_TYPE,							componentType,
									COMPONENT_ID,							componentID, 
									COMPONENT_ORDER,						globalorderID,
									COMPONENT_ORDER,						grouporderID,
									uint32,									intaddr, 
									uint16,									intport,
									uint32,									extaddr, 
									uint16,									extport,
									std::string,							extaddrEx)

	// ĳapp��������look��
	CELLAPPMGR_MESSAGE_DECLARE_ARGS0(lookApp,								NETWORK_FIXED_MESSAGE)

	// ĳ��app����鿴��app����״̬��
	CELLAPPMGR_MESSAGE_DECLARE_ARGS0(queryLoad,								NETWORK_FIXED_MESSAGE)

	// ĳ��app��app��֪���ڻ״̬��
	CELLAPPMGR_MESSAGE_DECLARE_ARGS2(onAppActiveTick,						NETWORK_FIXED_MESSAGE,
									COMPONENT_TYPE,							componentType, 
									COMPONENT_ID,							componentID)

	// baseEntity���󴴽���һ���µ�space�С�
	CELLAPPMGR_MESSAGE_DECLARE_STREAM(reqCreateCellEntityInNewSpace,		NETWORK_VARIABLE_MESSAGE)

	// baseEntity����ָ���һ���µ�space�С�
	CELLAPPMGR_MESSAGE_DECLARE_STREAM(reqRestoreSpaceInCell,				NETWORK_VARIABLE_MESSAGE)

	// ��Ϣת���� ��ĳ��app��ͨ����app����Ϣת����ĳ��app��
	CELLAPPMGR_MESSAGE_DECLARE_STREAM(forwardMessage,						NETWORK_VARIABLE_MESSAGE)

	// ����رշ�����
	CELLAPPMGR_MESSAGE_DECLARE_STREAM(reqCloseServer,						NETWORK_VARIABLE_MESSAGE)

	// �����ѯwatcher����
	CELLAPPMGR_MESSAGE_DECLARE_STREAM(queryWatcher,							NETWORK_VARIABLE_MESSAGE)

	// ����cellapp��Ϣ��
	CELLAPPMGR_MESSAGE_DECLARE_ARGS4(updateCellapp,							NETWORK_FIXED_MESSAGE,
									COMPONENT_ID,							componentID,
									ENTITY_ID,								numEntities,
									float,									load,
									uint32,									flags)

	// ��ʼprofile
	CELLAPPMGR_MESSAGE_DECLARE_STREAM(startProfile,							NETWORK_VARIABLE_MESSAGE)

	// ����ǿ��ɱ����ǰapp
	CELLAPPMGR_MESSAGE_DECLARE_STREAM(reqKillServer,						NETWORK_VARIABLE_MESSAGE)

	// cellappͬ���Լ��ĳ�ʼ����Ϣ
	CELLAPPMGR_MESSAGE_DECLARE_ARGS4(onCellappInitProgress,					NETWORK_FIXED_MESSAGE,
									COMPONENT_ID,							cid,
									float,									progress,
									COMPONENT_ORDER,						componentGlobalOrder,
									COMPONENT_ORDER,						componentGroupOrder)

	// ��ѯ������ؽ��̸�����Ϣ
	CELLAPPMGR_MESSAGE_DECLARE_STREAM(queryAppsLoads,						NETWORK_VARIABLE_MESSAGE)

	// ��ѯ������ؽ���space��Ϣ
	CELLAPPMGR_MESSAGE_DECLARE_STREAM(querySpaces,							NETWORK_VARIABLE_MESSAGE)

	// ������ؽ���space��Ϣ��ע�⣺��spaceData����API�ĵ���������spaceData
	// ��ָspace��һЩ��Ϣ
	CELLAPPMGR_MESSAGE_DECLARE_STREAM(updateSpaceData,						NETWORK_VARIABLE_MESSAGE)

	// ��������ı�space�鿴���������Ӻ�ɾ�����ܣ�
	CELLAPPMGR_MESSAGE_DECLARE_STREAM(setSpaceViewer,						NETWORK_VARIABLE_MESSAGE)

NETWORK_INTERFACE_DECLARE_END()

#ifdef DEFINE_IN_INTERFACE
	#undef DEFINE_IN_INTERFACE
#endif

}
#endif
