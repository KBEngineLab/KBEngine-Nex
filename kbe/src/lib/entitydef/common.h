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


#ifndef KBENGINE_DEF_COMMON_H
#define KBENGINE_DEF_COMMON_H

#include "common/common.h"
#if KBE_PLATFORM == PLATFORM_WIN32
#pragma warning (disable : 4910)
#pragma warning (disable : 4251)
#endif


namespace KBEngine{

/** entity�����ݴ������Ա�� */
enum EntityDataFlags
{
	ED_FLAG_UNKOWN													= 0x00000000, // δ����
	ED_FLAG_CELL_PUBLIC												= 0x00000001, // �������cell�㲥
	ED_FLAG_CELL_PRIVATE											= 0x00000002, // ��ǰcell
	ED_FLAG_ALL_CLIENTS												= 0x00000004, // cell�㲥�����пͻ���
	ED_FLAG_CELL_PUBLIC_AND_OWN										= 0x00000008, // cell�㲥���Լ��Ŀͻ���
	ED_FLAG_OWN_CLIENT												= 0x00000010, // ��ǰcell�Ϳͻ���
	ED_FLAG_BASE_AND_CLIENT											= 0x00000020, // base�Ϳͻ���
	ED_FLAG_BASE													= 0x00000040, // ��ǰbase
	ED_FLAG_OTHER_CLIENTS											= 0x00000080, // cell�㲥�������ͻ���
};

std::string entityDataFlagsToString(uint32 flags);
EntityDataFlags stringToEntityDataFlags(const std::string& strFlags);

#define ED_FLAG_ALL  ED_FLAG_CELL_PUBLIC | ED_FLAG_CELL_PRIVATE | ED_FLAG_ALL_CLIENTS \
	| ED_FLAG_CELL_PUBLIC_AND_OWN | ED_FLAG_OWN_CLIENT |	\
	ED_FLAG_BASE_AND_CLIENT | ED_FLAG_BASE | ED_FLAG_OTHER_CLIENTS

/** �൱�ڶ�entity���ݴ�������һ������Ķ��� */
enum EntityDataFlagRelation
{
	// ������baseapp�й�ϵ�ı�־
	ENTITY_BASE_DATA_FLAGS											= ED_FLAG_BASE | ED_FLAG_BASE_AND_CLIENT,
	// ������cellapp�й�ϵ�ı�־
	ENTITY_CELL_DATA_FLAGS											= ED_FLAG_CELL_PUBLIC | ED_FLAG_CELL_PRIVATE | ED_FLAG_ALL_CLIENTS | ED_FLAG_CELL_PUBLIC_AND_OWN | ED_FLAG_OTHER_CLIENTS | ED_FLAG_OWN_CLIENT,
	// ������client�й�ϵ�ı�־
	ENTITY_CLIENT_DATA_FLAGS										= ED_FLAG_BASE_AND_CLIENT | ED_FLAG_ALL_CLIENTS | ED_FLAG_CELL_PUBLIC_AND_OWN | ED_FLAG_OTHER_CLIENTS | ED_FLAG_OWN_CLIENT,
	// ������Ҫ�㲥������cellapp�ı�־
	ENTITY_BROADCAST_CELL_FLAGS										= ED_FLAG_CELL_PUBLIC | ED_FLAG_ALL_CLIENTS | ED_FLAG_CELL_PUBLIC_AND_OWN | ED_FLAG_OTHER_CLIENTS,
	// ������Ҫ�㲥�������ͻ���(�������Լ���)�ı�־
	ENTITY_BROADCAST_OTHER_CLIENT_FLAGS								= ED_FLAG_OTHER_CLIENTS | ED_FLAG_ALL_CLIENTS,
	// ������Ҫ�㲥���Լ��Ŀͻ��˵ı�־
	ENTITY_BROADCAST_OWN_CLIENT_FLAGS								= ED_FLAG_ALL_CLIENTS | ED_FLAG_CELL_PUBLIC_AND_OWN | ED_FLAG_OWN_CLIENT | ED_FLAG_BASE_AND_CLIENT,
	// ����baseapp��ͻ����йصı�־
	ENTITY_BASEAPP_ANDA_CLIENT_DATA_FLAGS							= ED_FLAG_BASE_AND_CLIENT,
	// ����cellapp��ͻ����йصı�־
	ENTITY_CELLAPP_ANDA_CLIENT_DATA_FLAGS							= ED_FLAG_ALL_CLIENTS | ED_FLAG_CELL_PUBLIC_AND_OWN | ED_FLAG_OTHER_CLIENTS | ED_FLAG_OWN_CLIENT,
};

/** entityCall�������Ӧ��������ӳ�䣬  ��������������ϸ�ƥ��ENTITYCALL_TYPE��ֵ */
const COMPONENT_TYPE ENTITYCALL_COMPONENT_TYPE_MAPPING[] = 
{
	CELLAPP_TYPE,
	BASEAPP_TYPE,
	CLIENT_TYPE,
	BASEAPP_TYPE,
	CELLAPP_TYPE,
	CELLAPP_TYPE,
	BASEAPP_TYPE,
};

/** ���Ե�lod�㲥����Χ�Ķ��� */
typedef uint8 DETAIL_TYPE;
#define DETAIL_LEVEL_NEAR													0	// lod���𣺽�						
#define DETAIL_LEVEL_MEDIUM													1	// lod������
#define DETAIL_LEVEL_FAR													2	// lod����Զ	

typedef std::map<std::string, EntityDataFlags> ENTITYFLAGMAP;
extern ENTITYFLAGMAP g_entityFlagMapping;										// entity ��flag�ַ���ӳ���

// ���Ժͷ�����UID���
typedef uint16 ENTITY_PROPERTY_UID;
typedef uint16 ENTITY_METHOD_UID;
typedef uint16 ENTITY_SCRIPT_UID;
typedef uint16 DATATYPE_UID;
typedef uint8  DATATYPE;
typedef uint8  ENTITY_DEF_ALIASID;

#define DATA_TYPE_UNKONWN		0
#define DATA_TYPE_FIXEDARRAY	1
#define DATA_TYPE_FIXEDDICT		2
#define DATA_TYPE_STRING		3
#define DATA_TYPE_DIGIT			4
#define DATA_TYPE_BLOB			5
#define DATA_TYPE_PYTHON		6
#define DATA_TYPE_VECTOR2		7
#define DATA_TYPE_VECTOR3		8
#define DATA_TYPE_VECTOR4		9
#define DATA_TYPE_UNICODE		10
#define DATA_TYPE_ENTITYCALL	11
#define DATA_TYPE_PYDICT		12
#define DATA_TYPE_PYTUPLE		13
#define DATA_TYPE_PYLIST		14

// ��entity��һЩϵͳ����Ŀɱ����Խ��б���Ա����紫��ʱ���б��
enum ENTITY_BASE_PROPERTY_UTYPE
{
	ENTITY_BASE_PROPERTY_UTYPE_POSITION_XYZ					= 1,
	ENTITY_BASE_PROPERTY_UTYPE_DIRECTION_ROLL_PITCH_YAW		= 2,
	ENTITY_BASE_PROPERTY_UTYPE_SPACEID						= 3,
};

// ��entity��һЩϵͳ����Ŀɱ����Խ��б���Ա����紫��ʱ���б��
enum ENTITY_BASE_PROPERTY_ALIASID
{
	ENTITY_BASE_PROPERTY_ALIASID_POSITION_XYZ				= 0,
	ENTITY_BASE_PROPERTY_ALIASID_DIRECTION_ROLL_PITCH_YAW	= 1,
	ENTITY_BASE_PROPERTY_ALIASID_SPACEID					= 2,
	ENTITY_BASE_PROPERTY_ALIASID_MAX						= 3,
};

// �����Ƶ�ϵͳ���ԣ�def�в���������
const char ENTITY_LIMITED_PROPERTYS[][34] =
{
	"id",
	"position",
	"direction",
	"spaceID",
	"autoLoad",
	"cell",
	"base",
	"client",
	"cellData",
	"className",
	"component"
	"databaseID",
	"isDestroyed",
	"shouldAutoArchive",
	"shouldAutoBackup",
	"__ACCOUNT_NAME__",
	"__ACCOUNT_PASSWORD__",
	"clientAddr",
	"clientEnabled",
	"hasClient",
	"roundTripTime",
	"timeSinceHeardFromClient",
	"allClients",
	"hasWitness",
	"isWitnessed",
	"otherClients",
	"topSpeed",
	"topSpeedY",
	"interface"
	"",
};

// ��ý��̵�python����Ŀ¼
std::pair<std::wstring, std::wstring> getComponentPythonPaths(COMPONENT_TYPE componentType);

}
#endif // KBENGINE_DEF_COMMON_H

