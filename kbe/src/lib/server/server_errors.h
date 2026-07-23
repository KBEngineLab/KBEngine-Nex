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

#ifndef KBE_SERVER_ERRORS_H
#define KBE_SERVER_ERRORS_H

#include "common/common.h"

namespace KBEngine { 

/**
	���������� ��Ҫ�Ƿ��������ظ��ͻ����õġ�
*/
	
typedef uint16 SERVER_ERROR_CODE;										// ���������


#define SERVER_SUCCESS										0			// �ɹ���
#define SERVER_ERR_SRV_NO_READY								1			// ������û��׼���á�
#define SERVER_ERR_SRV_OVERLOAD								2			// ���������ع��ء�
#define SERVER_ERR_ILLEGAL_LOGIN							3			// �Ƿ���¼��
#define SERVER_ERR_NAME_PASSWORD							4			// �û����������벻��ȷ��
#define SERVER_ERR_NAME										5			// �û�������ȷ��
#define SERVER_ERR_PASSWORD									6			// ���벻��ȷ��
#define SERVER_ERR_ACCOUNT_CREATE_FAILED					7			// �����˺�ʧ�ܡ�
#define SERVER_ERR_BUSY										8			// �������ڷ�æ(���磺�ڷ�����ǰһ������δִ����ϵ����������N�δ����˺�)��
#define SERVER_ERR_ACCOUNT_LOGIN_ANOTHER					9			// ��ǰ�˺�����һ����¼�ˡ�
#define SERVER_ERR_ACCOUNT_IS_ONLINE						10			// ���Ѿ���¼�ˣ��������ܾ��ٴε�¼��
#define SERVER_ERR_PROXY_DESTROYED							11			// ��ͻ��˹�����proxy�ڷ��������Ѿ����١�
#define SERVER_ERR_ENTITYDEFS_NOT_MATCH						12			// entityDefs��ƥ�䡣
#define SERVER_ERR_IN_SHUTTINGDOWN							13			// ���������ڹر���
#define SERVER_ERR_NAME_MAIL								14			// email��ַ����
#define SERVER_ERR_ACCOUNT_LOCK								15			// �˺ű����ᡣ
#define SERVER_ERR_ACCOUNT_DEADLINE							16			// �˺��ѹ��ڡ�
#define SERVER_ERR_ACCOUNT_NOT_ACTIVATED					17			// �˺�δ���
#define SERVER_ERR_VERSION_NOT_MATCH						18			// �����˵İ汾��ƥ�䡣
#define SERVER_ERR_OP_FAILED								19			// ����ʧ�ܡ�
#define SERVER_ERR_SRV_STARTING								20			// ���������������С�
#define SERVER_ERR_ACCOUNT_REGISTER_NOT_AVAILABLE			21			// δ�����˺�ע�Ṧ�ܡ�
#define SERVER_ERR_CANNOT_USE_MAIL							22			// ����ʹ��email��ַ��
#define SERVER_ERR_NOT_FOUND_ACCOUNT						23			// �Ҳ������˺š�
#define SERVER_ERR_DB										24			// ���ݿ����(����dbmgr��־��DB)��
#define SERVER_ERR_USER1									25			// �û��Զ��������1
#define SERVER_ERR_USER2									26			// �û��Զ��������2
#define SERVER_ERR_USER3									27			// �û��Զ��������3
#define SERVER_ERR_USER4									28			// �û��Զ��������4
#define SERVER_ERR_USER5									29			// �û��Զ��������5
#define SERVER_ERR_USER6									30			// �û��Զ��������6
#define SERVER_ERR_USER7									31			// �û��Զ��������7
#define SERVER_ERR_USER8									32			// �û��Զ��������8
#define SERVER_ERR_USER9									33			// �û��Զ��������9
#define SERVER_ERR_USER10									34			// �û��Զ��������10
#define SERVER_ERR_LOCAL_PROCESSING							35			// ���ش�����ͨ��Ϊĳ�����鲻�ɵ���������������KBE����������
#define SERVER_ERR_ACCOUNT_RESET_PASSWORD_NOT_AVAILABLE		36			// δ�����˺��������빦�ܡ�
#define SERVER_ERR_ACCOUNT_LOGIN_ANOTHER_SERVER				37			// ��ǰ�˺���������������½��
#define SERVER_ERR_MAX										38          // ��������������д��������棬�Ȿ������һ�������ʶ������ʾһ���ж�����������

const char SERVER_ERR_STR[][256] = {
	"SERVER_SUCCESS",
	"SERVER_ERR_SRV_NO_READY",
	"SERVER_ERR_SRV_OVERLOAD",
	"SERVER_ERR_ILLEGAL_LOGIN",
	"SERVER_ERR_NAME_PASSWORD",
	"SERVER_ERR_NAME",
	"SERVER_ERR_PASSWORD",
	"SERVER_ERR_ACCOUNT_CREATE_FAILED",
	"SERVER_ERR_BUSY",
	"SERVER_ERR_ACCOUNT_LOGIN_ANOTHER",
	"SERVER_ERR_ACCOUNT_IS_ONLINE",
	"SERVER_ERR_PROXY_DESTROYED",
	"SERVER_ERR_ENTITYDEFS_NOT_MATCH",
	"SERVER_ERR_IN_SHUTTINGDOWN",
	"SERVER_ERR_NAME_MAIL",
	"SERVER_ERR_ACCOUNT_LOCK",
	"SERVER_ERR_ACCOUNT_DEADLINE",
	"SERVER_ERR_ACCOUNT_NOT_ACTIVATED",
	"SERVER_ERR_VERSION_NOT_MATCH",
	"SERVER_ERR_OP_FAILED",
	"SERVER_ERR_SRV_STARTING",
	"SERVER_ERR_ACCOUNT_REGISTER_NOT_AVAILABLE",
	"SERVER_ERR_CANNOT_USE_MAIL",
	"SERVER_ERR_NOT_FOUND_ACCOUNT",
	"SERVER_ERR_DB",
	"SERVER_ERR_USER1",
	"SERVER_ERR_USER2",
	"SERVER_ERR_USER3",
	"SERVER_ERR_USER4",
	"SERVER_ERR_USER5",
	"SERVER_ERR_USER6",
	"SERVER_ERR_USER7",
	"SERVER_ERR_USER8",
	"SERVER_ERR_USER9",
	"SERVER_ERR_USER10",
	"SERVER_ERR_LOCAL_PROCESSING",
	"SERVER_ERR_ACCOUNT_RESET_PASSWORD_NOT_AVAILABLE",
	"SERVER_ERR_ACCOUNT_LOGIN_ANOTHER_SERVER"
};

}

#endif // KBE_SERVER_ERRORS_H
