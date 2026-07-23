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


#ifndef KBE_PROXY_H
#define KBE_PROXY_H
	
#include "entity.h"
#include "data_downloads.h"
#include "common/common.h"
#include "helper/debug_helper.h"
#include "network/address.h"
#include "network/message_handler.h"
	
namespace KBEngine{


namespace Network
{
class Channel;
}

class ProxyForwarder;

#define LOG_ON_REJECT  0
#define LOG_ON_ACCEPT  1
#define LOG_ON_WAIT_FOR_DESTROY 2

class Proxy : public Entity
{
	/** ���໯��һЩpy�������������� */
	BASE_SCRIPT_HREADER(Proxy, Entity)

public:
	Proxy(ENTITY_ID id, const ScriptDefModule* pScriptModule);
	~Proxy();
	
	INLINE void addr(const Network::Address& address);
	INLINE const Network::Address& addr() const;

	typedef std::vector<Network::Bundle*> Bundles;
	bool pushBundle(Network::Bundle* pBundle);

	/**
		��witness�ͻ�������һ����Ϣ
	*/
	bool sendToClient(const Network::MessageHandler& msgHandler, Network::Bundle* pBundle);
	bool sendToClient(Network::Bundle* pBundle, bool immediately = false);
	bool sendToClient(bool expectData = true);

	/** 
		�ű������ȡ���ӵ�rttֵ
	*/
	double getRoundTripTime() const;
	DECLARE_PY_GET_MOTHOD(pyGetRoundTripTime);

	/** 
		This is the number of seconds since a packet from the client was last received. 
	*/
	double getTimeSinceHeardFromClient() const;
	DECLARE_PY_GET_MOTHOD(pyGetTimeSinceHeardFromClient);

	/** 
		�ű������ȡ�Ƿ���client�󶨵�proxy��
	*/
	bool hasClient() const;
	DECLARE_PY_GET_MOTHOD(pyHasClient);

	/** 
		�ű������ȡclient��ַ
	*/
	DECLARE_PY_GET_MOTHOD(pyClientAddr);

	/** 
		ʵ���Ƿ����
	*/
	INLINE bool clientEnabled() const;
	DECLARE_PY_GET_MOTHOD(pyGetClientEnabled);

	/**
		���entity��������, �ڿͻ��˳�ʼ���ö�Ӧ��entity�� �������������
	*/
	void onClientEnabled(void);
	
	/**
		һ�����������������
	*/
	void onStreamComplete(int16 id, bool success);

	/**
		��½���ԣ� �������ĵ�½ʧ��֮�� ��������ӿ��ٽ��г��� 
	*/
	int32 onLogOnAttempt(const char* addr, uint32 port, const char* password);
	
	/**
		��ʼ���ͻ���proxy������
	*/
	void initClientBasePropertys();
	void initClientCellPropertys();

	/** 
		��������entity��Ӧ�Ŀͻ���socket�Ͽ�ʱ������ 
	*/
	void onClientDeath(void);
	
	/** ����ӿ�
		���ͻ��������������entity��cell������ʱ�������� 
	*/
	void onClientGetCell(Network::Channel* pChannel, COMPONENT_ID componentID);

	/**
		��ȡǰ�����
	*/
	INLINE COMPONENT_CLIENT_TYPE getClientType() const;
	INLINE void setClientType(COMPONENT_CLIENT_TYPE ctype);
	DECLARE_PY_MOTHOD_ARG0(pyGetClientType);

	/**
		�Ͽ��ͻ�������
	*/
	DECLARE_PY_MOTHOD_ARG0(pyDisconnect);

	/**
		��ȡǰ�˸�������
	*/
	INLINE const std::string& getLoginDatas();
	INLINE void setLoginDatas(const std::string& datas);
	
	INLINE const std::string& getCreateDatas();
	INLINE void setCreateDatas(const std::string& datas);

	DECLARE_PY_MOTHOD_ARG0(pyGetClientDatas);

	/**
		ÿ��proxy����֮�󶼻���ϵͳ����һ��uuid�� �ṩǰ���ص�½ʱ��������ʶ��
	*/
	INLINE uint64 rndUUID() const;
	INLINE void rndUUID(uint64 uid);

	/** 
		���������������Ŀͻ���ת����һ��proxyȥ���� 
	*/
	void giveClientTo(Proxy* proxy);
	void onGiveClientTo(Network::Channel* lpChannel);
	void onGiveClientToFailure();
	DECLARE_PY_MOTHOD_ARG1(pyGiveClientTo, PyObject_ptr);

	/**
		�ļ�����������
	*/
	static PyObject* __py_pyStreamFileToClient(PyObject* self, PyObject* args);
	int16 streamFileToClient(PyObjectPtr objptr, 
		const std::string& descr = "", int16 id = -1);

	/**
		�ַ�������������
	*/
	static PyObject* __py_pyStreamStringToClient(PyObject* self, PyObject* args);
	int16 streamStringToClient(PyObjectPtr objptr, 
		const std::string& descr = "", int16 id = -1);

	/**
		����witness
	*/
	void onGetWitness();

	/**
		���ͻ��˴ӷ������߳�
	*/
	void kick();

	/**
		������proxy�Ŀͻ������Ӷ���
	*/
	Network::Channel* pChannel();

protected:
	uint64 rndUUID_;
	Network::Address addr_;
	DataDownloads dataDownloads_;

	bool clientEnabled_;

	// ���ƿͻ���ÿ������ʹ�õĴ���
	int32 bandwidthPerSecond_;

	// ͨ�ż���key Ĭ��blowfish
	std::string encryptionKey;

	ProxyForwarder* pProxyForwarder_;

	COMPONENT_CLIENT_TYPE clientComponentType_;

	// ��½ʱ������datas���ݣ����浵��
	std::string loginDatas_;

	// ע��ʱ������datas���ݣ����ô浵��
	std::string createDatas_;
};

}


#ifdef CODE_INLINE
#include "proxy.inl"
#endif

#endif // KBE_PROXY_H
