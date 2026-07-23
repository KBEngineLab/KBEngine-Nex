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


#ifndef KBE_DEBUG_OPTION_H
#define KBE_DEBUG_OPTION_H
#include "common/common.h"

namespace KBEngine{

namespace Network
{

/** 
	��������������ݰ��Ƿ�����Я��������Ϣ�� ������ĳЩǰ�˽������ʱ�ṩһЩ����
	 ���Ϊfalse��һЩ�̶����ȵ����ݰ���Я��������Ϣ�� �ɶԶ����н���
*/
extern bool g_packetAlwaysContainLength;

/**
�Ƿ���Ҫ���κν��պͷ��͵İ����ı������log���ṩ����
		g_trace_packet:
			0: �����
			1: 16�������
			2: �ַ������
			3: 10�������
		use_logfile:
			�Ƿ����һ��log�ļ�����¼�����ݣ��ļ���ͨ��Ϊ
			appname_packetlogs.log
		g_trace_packet_disables:
			�ر�ĳЩ�������
*/
extern uint8 g_trace_packet;
extern bool g_trace_encrypted_packet;
extern std::vector<std::string> g_trace_packet_disables;
extern bool g_trace_packet_use_logfile;

}

/**
	�Ƿ����entity�Ĵ����� �ű���ȡ���ԣ� ��ʼ�����Եȵ�����Ϣ��
*/
extern bool g_debugEntity;

/**
	apps����״̬, ���ڽű��л�ȡ��ֵ
		0 : debug
		1 : release
*/
extern int8 g_appPublish;

}

#endif // KBE_DEBUG_OPTION_H
