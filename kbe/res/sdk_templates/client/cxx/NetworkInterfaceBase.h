// Copyright 1998-2016 Epic Games, Inc. All Rights Reserved.

#pragma once

#include "KBECommon.h"

#include "EncryptionFilter.h"

/*
	网络模块
	处理连接、收发数据
*/
namespace KBEngine
{

class PacketSenderBase;
class PacketReceiverBase;
class MemoryStream;
class InterfaceConnect;
class NetworkInterfaceBase
{
public:
    NetworkInterfaceBase();
	virtual ~NetworkInterfaceBase();

	const KBString UDP_HELLO = KBTEXT("62a559f3fa7748bc22f8e0766019d498");
	const KBString UDP_HELLO_ACK = KBTEXT("1432ad7c829170a76dd31982c3501eca");

public:
	virtual EncryptionFilter* filter() {
		return pFilter_;
	}

	virtual void setFilter(EncryptionFilter* filter) {
		pFilter_ = filter;
	}

	// virtual void process();

	virtual void reset();
	virtual void close();
	virtual bool valid();
	virtual void process();

	virtual bool connectTo(const KBString& addr, uint16 port, InterfaceConnect* callback, int userdata);
	virtual bool send(MemoryStream* pMemoryStream);
	virtual bool sendTo(MemoryStream* pMemoryStream);

	virtual void destroy() {
		// close();
		reset();
	}

	// virtual FSocket* createSocket(const KBString& socketDescript = KBTEXT("default"));

protected:
	// virtual void tickConnecting();
	// virtual bool _connect(const FInternetAddr& addr);

	// virtual PacketSenderBase* createPacketSender() = 0;
	// virtual PacketReceiverBase* createPacketReceiver() = 0;

protected:
	// FSocket* socket_;
	// PacketSenderBase* pPacketSender_;
	// PacketReceiverBase* pPacketReceiver_;


	MessageReader* pMessageReader_;
	MemoryStream* pBuffer_;

	InterfaceConnect* connectCB_;
	KBString connectIP_;
	uint16 connectPort_;
	int connectUserdata_;
	double startTime_;

	bool isDestroyed_;

	EncryptionFilter *pFilter_;


};

}
