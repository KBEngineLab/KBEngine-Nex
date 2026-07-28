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

#ifndef KBE_ENDPOINT_H
#define KBE_ENDPOINT_H

#include "common/common.h"
#include "common/objectpool.h"
#include "helper/debug_helper.h"
#include "network/address.h"
#include "network/common.h"
#include "openssl/ssl.h"

namespace KBEngine { 
namespace Network
{

class Bundle;
class EndPoint : public PoolObject
{
public:
	typedef KBEShared_ptr< SmartPoolObject< EndPoint > > SmartPoolObjectPtr;
	static SmartPoolObjectPtr createSmartPoolObj(const std::string& logPoint);
	static ObjectPool<EndPoint>& ObjPool();
	static EndPoint* createPoolObject(const std::string& logPoint);
	static void reclaimPoolObject(EndPoint* obj);
	static void destroyObjPool();
	void onReclaimObject();

	virtual size_t getPoolObjectBytes()
	{
		size_t bytes = sizeof(KBESOCKET)
		 + address_.getPoolObjectBytes();

		return bytes;
	}

	EndPoint(Address address);
	EndPoint(u_int32_t networkAddr = 0, u_int16_t networkPort = 0);
	virtual ~EndPoint();

	INLINE operator KBESOCKET() const;
	
	static void initNetwork();
	INLINE bool good() const;
		
	void socket(int type);
	INLINE KBESOCKET socket() const;
	
	INLINE void setFileDescriptor(KBESOCKET fd);

	INLINE int joinMulticastGroup(u_int32_t networkAddr);
	INLINE int quitMulticastGroup(u_int32_t networkAddr);
	
	INLINE int close();
	
	INLINE int setnonblocking(bool nonblocking);
	INLINE int setbroadcast(bool broadcast);
	INLINE int setreuseaddr(bool reuseaddr);
	INLINE int setkeepalive(bool keepalive);
	INLINE int setnodelay(bool nodelay = true);
	INLINE int setlinger(uint16 onoff, uint16 linger);

	INLINE int bind(u_int16_t networkPort = 0, u_int32_t networkAddr = INADDR_ANY);

	INLINE int listen(int backlog = 5);

	INLINE int connect(u_int16_t networkPort, u_int32_t networkAddr = INADDR_BROADCAST, bool autosetflags = true);
	INLINE int connect(bool autosetflags = true);

	INLINE EndPoint* accept(u_int16_t * networkPort = NULL, u_int32_t * networkAddr = NULL, bool autosetflags = true);
	
	INLINE int send(const void * gramData, int gramSize);
	void send(Bundle * pBundle);
	void sendto(Bundle * pBundle, u_int16_t networkPort, u_int32_t networkAddr = BROADCAST);

	INLINE int recv(void * gramData, int gramSize);
	bool recvAll(void * gramData, int gramSize);
	
	INLINE uint32 getRTT();

	INLINE int getInterfaceFlags(const char * name, int & flags);
	INLINE int getInterfaceAddress(const char * name, u_int32_t & address);
	INLINE int getInterfaceNetmask(const char * name, u_int32_t & netmask);
	bool getInterfaces(std::map< u_int32_t, std::string > &interfaces);

	int findIndicatedInterface(const char * spec, u_int32_t & address);
	int findDefaultInterface(char * name, int buffsize);

	int getInterfaceAddressByName(const char * name, u_int32_t & address);
	int getInterfaceAddressByMAC(const char * mac, u_int32_t & address);
	int getDefaultInterfaceAddress(u_int32_t & address);

	int getBufferSize(int optname) const;
	bool setBufferSize(int optname, int size);
	
	INLINE int getlocaladdress(u_int16_t * networkPort, u_int32_t * networkAddr) const;
	INLINE int getremoteaddress(u_int16_t * networkPort, u_int32_t * networkAddr) const;
	
	Network::Address getLocalAddress() const;
	Network::Address getRemoteAddress() const;

	bool getClosedPort(Network::Address & closedPort);

	INLINE const char * c_str() const;
	INLINE int getremotehostname(std::string * name) const;
	
	INLINE int sendto(void * gramData, int gramSize, u_int16_t networkPort, u_int32_t networkAddr = BROADCAST);
	// peer EndPoint 已保存目标地址，sender 通过该重载避免重复拆装端口和 IP。
	// A peer EndPoint already stores its destination, so this overload avoids repeatedly unpacking the port and IP in senders.
	INLINE int sendto(void* gramData, int gramSize);
	INLINE int sendto(void * gramData, int gramSize, struct sockaddr_in & sin);
	INLINE int recvfrom(void * gramData, int gramSize, u_int16_t * networkPort, u_int32_t * networkAddr);
	INLINE int recvfrom(void * gramData, int gramSize, struct sockaddr_in & sin);
	
	INLINE const Address& addr() const;
	INLINE void addr(const Address& newAddress);
	INLINE void addr(u_int16_t newNetworkPort, u_int32_t newNetworkAddress);

	bool waitSend();

	// UDP peer EndPoint 仅引用 listener socket；销毁 peer Channel 时绝不能关闭所有连接共享的原生句柄。
	// A UDP peer EndPoint only references the listener socket; destroying one peer Channel must never close the native handle shared by all peers.
	void setSocketRef(KBESOCKET socket)
	{
		socket_ = socket;
		isRefSocket_ = true;
	}

	// completion 后端使用内存 BIO 接管 TLS 密文，readiness 后端继续使用原生 socket BIO。
	// Completion backends use memory BIOs for TLS ciphertext while readiness backends retain the native socket BIO.
	bool setupSSL(int sslVersion, Packet* pPacket, bool useMemoryBIO = false);
	// 启动或继续 TLS 双向关闭；内存 BIO 生成的 close_notify 仍由 Channel 交给 completion poller。
	// Start or continue bidirectional TLS shutdown; Channel still hands memory-BIO close_notify records to the completion poller.
	bool shutdownSSL();
	bool destroySSL();
	// 将 completion 收到的密文喂给 OpenSSL，并返回当前可用的应用层明文。
	// Feed ciphertext delivered by a completion backend into OpenSSL and return currently available application plaintext.
	bool consumeSSLNetworkData(const void* data, int length, std::vector<char>& plaintext, bool& peerClosed);
	// 把应用层数据编码为 TLS record；调用成功后密文由 takeSSLNetworkOutput 统一取走。
	// Encode application data into TLS records; takeSSLNetworkOutput transfers the resulting ciphertext after success.
	bool encryptSSLNetworkData(const void* data, int length);
	// 转移 OpenSSL 已生成但尚未交给 poller 的全部 TLS 密文。
	// Transfer all TLS ciphertext generated by OpenSSL but not yet handed to the poller.
	bool takeSSLNetworkOutput(std::vector<char>& output);

	bool isSSL() const {
		return sslHandle_ != NULL;
	}

	bool usesSSLMemoryBIO() const {
		return sslUsesMemoryBIO_;
	}

protected:
	// 驱动一次非阻塞服务端握手，并把输出 BIO 中的数据收集到待发送队列。
	// Advance the nonblocking server handshake once and collect output BIO bytes for transmission.
	bool driveSSLHandshake();
	// 排空 OpenSSL 输出 BIO，避免握手或应用数据密文滞留在 TLS 层。
	// Drain the OpenSSL output BIO so handshake or application ciphertext cannot remain stranded in TLS state.
	bool drainSSLNetworkOutput();

	KBESOCKET socket_;
	Address address_;
	SSL* sslHandle_;
	SSL_CTX* sslContext_;
	bool isRefSocket_;
	bool sslUsesMemoryBIO_;
	std::vector<char> sslNetworkOutput_;
};

}
}

#ifdef CODE_INLINE
#include "endpoint.inl"
#endif
#endif // KBE_ENDPOINT_H
