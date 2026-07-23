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

#ifndef KBE_EVENT_POLLER_H
#define KBE_EVENT_POLLER_H

#include "common/common.h"
#include "common/timestamp.h"
#include "network/interfaces.h"
#include "thread/concurrency.h"
#include "network/common.h"
#include "network/address.h"
#include <map>
#include <vector>

namespace KBEngine { 
namespace Network
{
	
class InputNotificationHandler;
class Address;
typedef std::map<int, InputNotificationHandler *> FDReadHandlers;
typedef std::map<int, OutputNotificationHandler *> FDWriteHandlers;

struct TcpCompletionData
{
	std::vector<char> payload;
	bool disconnected;
	int errorCode;

	// Store one TCP receive completion so upper layers consume data without probing socket readiness again.
	// ����һ�� TCP ������ɽ�������ϲ���������ʱ�����ظ�̽�� socket readiness��
};

struct UdpCompletionData
{
	std::vector<char> payload;
	Address srcAddr;
	int errorCode;

	// Store one UDP datagram completion together with its source address.
	// ����һ�� UDP ���ݱ���ɽ��������Դ��ַ��
};

class EventPoller
{
public:
	EventPoller();
	virtual ~EventPoller();

	bool registerForRead(int fd, InputNotificationHandler * handler);
	bool registerForWrite(int fd, OutputNotificationHandler * handler);

	bool deregisterForRead(int fd);
	bool deregisterForWrite(int fd);


	virtual int processPendingEvents(double maxWait) = 0;
	virtual int getFileDescriptor() const;

	// ���ص�ǰ�¼���˵��ȶ����ƣ���������־����ϺͲ���ʶ��ʵ�� IO ģ�͡�
	// Return a stable name for the active event backend so startup logs, diagnostics, and tests can identify the actual IO model.
	virtual const char* ioModelName() const;

	// readiness ��˷��� false�����ģ�ͽ�����ɾ����˷��� true��
	// Readiness backends return false; a completion backend returns true after it is integrated.
	virtual bool supportsCompletion() const;

	// ����ɶ���ȡ��һ���Ѿ���ɵ� accept��readiness ��˱��� false������ı�ɵ�������
	// Take one completed accept from the completion queue; readiness backends return false to preserve the old call path.
	virtual bool takeAcceptedSocket(int fd, KBESOCKET& acceptedSocket);

	// ����ɶ���ȡ��һ�� TCP ���ݻ���ֹ״̬����������Ȩ�ڵ��÷����պ�ת�ơ�
	// Take TCP data or a terminal state from the completion queue; ownership transfers to the caller on success.
	virtual bool takeTcpReceivedData(int fd, std::vector<char>& data, bool& disconnected, int& errorCode);

	// ����ɶ���ȡ��һ�� UDP ���ݱ�������Դ��ַ��
	// Take one UDP datagram and its source address from the completion queue.
	virtual bool takeUdpReceivedData(int fd, std::vector<char>& data, Address& srcAddr, int& errorCode);

	// �� TCP ���ݽ�����ɺ���Ŷӣ��ɺ�˷��� false������ʹ��ԭ�� PacketSender ����·����
	// Queue TCP data in a completion backend; legacy backends return false and keep the original PacketSender path.
	virtual bool queueTcpSend(int fd, const void* data, int len);

	// �� UDP ���ݱ�������ɺ���Ŷӣ��ɺ�˷��� false������ԭ�� UDP �������塣
	// Queue a UDP datagram in a completion backend; legacy backends return false and preserve existing UDP send semantics.
	virtual bool queueUdpSend(int fd, const void* data, int len, const Address& dstAddr);

	// ��ѯָ�� socket �Ƿ�������ɺ�˴��������ݣ����ڱ����ظ�ע��д�¼���
	// Report whether a socket still has pending completion-backend sends to avoid duplicate write registration.
	virtual bool hasPendingSend(int fd) const;

	void clearSpareTime()		{spareTime_ = 0;}
	uint64 spareTime() const	{return spareTime_;}

	static EventPoller * create();

	// ���ݵ�ǰ����ƽ̨���� 1.x ����Ĭ�Ϻ�����ƣ����ı���ѡ���߼���
	// Return the 1.x baseline backend name for the build platform without changing backend selection logic.
	static const char* defaultIOModelName();

	InputNotificationHandler* findForRead(int fd);
	OutputNotificationHandler* findForWrite(int fd);

protected:
	virtual bool doRegisterForRead(int fd) = 0;
	virtual bool doRegisterForWrite(int fd) = 0;

	virtual bool doDeregisterForRead(int fd) = 0;
	virtual bool doDeregisterForWrite(int fd) = 0;

	bool triggerRead(int fd);
	bool triggerWrite(int fd);
	bool triggerError(int fd);
	
	bool isRegistered(int fd, bool isForRead) const;

	int maxFD() const;

private:
	FDReadHandlers fdReadHandlers_;
	FDWriteHandlers fdWriteHandlers_;

protected:
	uint64 spareTime_;
};

}
}
#endif // KBE_EVENT_POLLER_H
