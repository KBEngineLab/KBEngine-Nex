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


#include "endpoint.h"
#ifndef CODE_INLINE
#include "endpoint.inl"
#endif

#include "resmgr/resmgr.h"
#include <openssl/bio.h>
#include <openssl/err.h>
#include <new>

#include "network/bundle.h"
#include "network/tcp_packet_receiver.h"
#include "network/tcp_packet_sender.h"
#include "network/udp_packet_receiver.h"

#if KBE_PLATFORM == PLATFORM_WIN32
#include <Iphlpapi.h>
#pragma comment (lib,"iphlpapi.lib") 
#else
#include <net/if.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#endif

namespace KBEngine { 
namespace Network
{
#if KBE_PLATFORM == PLATFORM_UNIX
#else	// not unix
	// Need to implement if_nameindex functions on Windows
	/** @internal */
	struct if_nameindex
	{

		unsigned int if_index;	/* 1, 2, ... */

		// Windows 回退表持有静态接口名，调用链只读取名称，不应暴露可写指针。
		// The Windows fallback table owns static interface names; the call chain only reads them and must not expose writable pointers.
		const char *if_name;

	};

	/** @internal */
	struct if_nameindex *if_nameindex(void)
	{
		static struct if_nameindex staticIfList[3] =
		{ { 1, "eth0" }, { 2, "lo" }, { 0, 0 } };

		return staticIfList;
	}

	/** @internal */
	inline void if_freenameindex(struct if_nameindex *)
	{}
#endif	// not unix

static bool g_networkInitted = false;

#if KBE_PLATFORM == PLATFORM_WIN32
// OpenSSL 的标准 socket BIO 使用 int 保存描述符，会在 Win64 上截断 UINT_PTR 类型的 SOCKET。
// OpenSSL's standard socket BIO stores descriptors as int, which truncates Win64 UINT_PTR SOCKET values.
static KBESOCKET windowsSocketBIODescriptor(BIO* bio)
{
	KBESOCKET* pSocket = static_cast<KBESOCKET*>(BIO_get_data(bio));
	return pSocket ? *pSocket : INVALID_SOCKET;
}

// BIO 不拥有 socket；EndPoint 统一管理句柄生命周期，避免 SSL_free 与 EndPoint::close 重复关闭。
// The BIO does not own the socket; EndPoint retains lifecycle ownership to prevent SSL_free and EndPoint::close from closing it twice.
static int windowsSocketBIOCreate(BIO* bio)
{
	BIO_set_init(bio, 1);
	BIO_set_shutdown(bio, 0);
	BIO_set_data(bio, NULL);
	return 1;
}

static int windowsSocketBIODestroy(BIO* bio)
{
	if (!bio)
		return 0;

	delete static_cast<KBESOCKET*>(BIO_get_data(bio));
	BIO_set_data(bio, NULL);
	BIO_set_init(bio, 0);
	return 1;
}

static int windowsSocketBIORead(BIO* bio, char* buffer, int length)
{
	BIO_clear_retry_flags(bio);
	int result = ::recv(windowsSocketBIODescriptor(bio), buffer, length, 0);
	if (result == SOCKET_ERROR)
	{
		int error = WSAGetLastError();
		if (error == WSAEWOULDBLOCK || error == WSAEINTR)
			BIO_set_retry_read(bio);
	}

	return result;
}

static int windowsSocketBIOWrite(BIO* bio, const char* buffer, int length)
{
	BIO_clear_retry_flags(bio);
	int result = ::send(windowsSocketBIODescriptor(bio), buffer, length, 0);
	if (result == SOCKET_ERROR)
	{
		int error = WSAGetLastError();
		if (error == WSAEWOULDBLOCK || error == WSAEINTR)
			BIO_set_retry_write(bio);
	}

	return result;
}

static int windowsSocketBIOPuts(BIO* bio, const char* value)
{
	return windowsSocketBIOWrite(bio, value, static_cast<int>(strlen(value)));
}

static long windowsSocketBIOControl(BIO* bio, int command, long argument, void*)
{
	switch (command)
	{
	case BIO_CTRL_FLUSH:
		return 1;
	case BIO_CTRL_GET_CLOSE:
		return BIO_get_shutdown(bio);
	case BIO_CTRL_SET_CLOSE:
		BIO_set_shutdown(bio, static_cast<int>(argument));
		return 1;
	default:
		return 0;
	}
}

static BIO_METHOD* windowsSocketBIOMethod()
{
	static BIO_METHOD* method = []() -> BIO_METHOD*
	{
		BIO_METHOD* value = BIO_meth_new(BIO_TYPE_SOCKET, "KBEngine Win64 socket");
		if (!value ||
			BIO_meth_set_create(value, windowsSocketBIOCreate) != 1 ||
			BIO_meth_set_destroy(value, windowsSocketBIODestroy) != 1 ||
			BIO_meth_set_read(value, windowsSocketBIORead) != 1 ||
			BIO_meth_set_write(value, windowsSocketBIOWrite) != 1 ||
			BIO_meth_set_puts(value, windowsSocketBIOPuts) != 1 ||
			BIO_meth_set_ctrl(value, windowsSocketBIOControl) != 1)
		{
			if (value)
				BIO_meth_free(value);
			return NULL;
		}

		return value;
	}();

	return method;
}

static BIO* createWindowsSocketBIO(KBESOCKET socket)
{
	BIO_METHOD* method = windowsSocketBIOMethod();
	if (!method)
		return NULL;

	BIO* bio = BIO_new(method);
	if (!bio)
		return NULL;

	KBESOCKET* pSocket = new (std::nothrow) KBESOCKET(socket);
	if (!pSocket)
	{
		BIO_free(bio);
		return NULL;
	}

	BIO_set_data(bio, pSocket);
	return bio;
}
#endif

//-------------------------------------------------------------------------------------
static ObjectPool<EndPoint> _g_objPool("EndPoint");
ObjectPool<EndPoint>& EndPoint::ObjPool()
{
	return _g_objPool;
}

//-------------------------------------------------------------------------------------
EndPoint* EndPoint::createPoolObject(const std::string& logPoint)
{
	return _g_objPool.createObject(logPoint);
}

//-------------------------------------------------------------------------------------
void EndPoint::reclaimPoolObject(EndPoint* obj)
{
	_g_objPool.reclaimObject(obj);
}

//-------------------------------------------------------------------------------------
void EndPoint::destroyObjPool()
{
	DEBUG_MSG(fmt::format("EndPoint::destroyObjPool(): size {}.\n", 
		_g_objPool.size()));

	_g_objPool.destroy();
}

//-------------------------------------------------------------------------------------
EndPoint::SmartPoolObjectPtr EndPoint::createSmartPoolObj(const std::string& logPoint)
{
	return SmartPoolObjectPtr(new SmartPoolObject<EndPoint>(ObjPool().createObject(logPoint), _g_objPool));
}

//-------------------------------------------------------------------------------------
void EndPoint::onReclaimObject()
{
	close();
}

//-------------------------------------------------------------------------------------
bool EndPoint::getClosedPort(Network::Address & closedPort)
{
	bool isResultSet = false;

#if KBE_PLATFORM == PLATFORM_UNIX
//	KBE_ASSERT(errno == ECONNREFUSED);

	struct sockaddr_in	offender;
	offender.sin_family = 0;
	offender.sin_port = 0;
	offender.sin_addr.s_addr = 0;

	struct msghdr	errHeader;
	struct iovec	errPacket;

	char data[ 256 ];
	char control[ 256 ];

	errHeader.msg_name = &offender;
	errHeader.msg_namelen = sizeof(offender);
	errHeader.msg_iov = &errPacket;
	errHeader.msg_iovlen = 1;
	errHeader.msg_control = control;
	errHeader.msg_controllen = sizeof(control);
	errHeader.msg_flags = 0;	// result only

	errPacket.iov_base = data;
	errPacket.iov_len = sizeof(data);

	int errMsgErr = recvmsg(*this, &errHeader, MSG_ERRQUEUE);
	if (errMsgErr < 0)
	{
		return false;
	}

	struct cmsghdr * ctlHeader;

	for (ctlHeader = CMSG_FIRSTHDR(&errHeader);
		ctlHeader != NULL;
		ctlHeader = CMSG_NXTHDR(&errHeader,ctlHeader))
	{
		if (ctlHeader->cmsg_level == SOL_IP &&
			ctlHeader->cmsg_type == IP_RECVERR) break;
	}

	// Was there an IP_RECVERR error.

	if (ctlHeader != NULL)
	{
		struct sock_extended_err * extError =
			(struct sock_extended_err*)CMSG_DATA(ctlHeader);

		// Only use this address if the kernel has the bug where it does not
		// report the packet details.

		if (errHeader.msg_namelen == 0)
		{
			// Finally we figure out whose fault it is except that this is the
			// generator of the error (possibly a machine on the path to the
			// destination), and we are interested in the actual destination.
			offender = *(sockaddr_in*)SO_EE_OFFENDER(extError);
			offender.sin_port = 0;

			ERROR_MSG("EndPoint::getClosedPort: "
				"Kernel has a bug: recv_msg did not set msg_name.\n");
		}

		closedPort.ip = offender.sin_addr.s_addr;
		closedPort.port = offender.sin_port;

		isResultSet = true;
	}
#endif // unix

	return isResultSet;
}

//-------------------------------------------------------------------------------------
int EndPoint::getBufferSize(int optname) const
{
	KBE_ASSERT(optname == SO_SNDBUF || optname == SO_RCVBUF);

	int recvbuf = -1;
	socklen_t rbargsize = sizeof(int);

	int rberr = getsockopt(socket_, SOL_SOCKET, optname,
		(char*)&recvbuf, &rbargsize);

	if (rberr == 0 && rbargsize == sizeof(int))
		return recvbuf;

	ERROR_MSG(fmt::format("EndPoint::getBufferSize: "
		"Failed to read option {}: {}\n",
		(optname == SO_SNDBUF ? "SO_SNDBUF" : "SO_RCVBUF"),
		kbe_strerror()));

	return -1;
}

//-------------------------------------------------------------------------------------
bool EndPoint::getInterfaces(std::map< u_int32_t, std::string > &interfaces)
{
#if KBE_PLATFORM == PLATFORM_WIN32
	int count = 0;
	char hostname[1024];
	struct hostent* inaddrs;

	if(gethostname(hostname, 1024) == 0)
	{
		inaddrs = gethostbyname(hostname);
		if(inaddrs)
		{
			while(inaddrs->h_addr_list[count])
			{
				unsigned long addrs = *(unsigned long*)inaddrs->h_addr_list[count];
				interfaces[addrs] = "eth0";
				char *ip = inet_ntoa (*(struct in_addr *)inaddrs->h_addr_list[count]);
				DEBUG_MSG(fmt::format("EndPoint::getInterfaces: found eth0 {}\n", ip));
				++count;
			}
		}
	}

	return count > 0;
#else
	struct ifconf ifc;
	char          buf[1024];

	ifc.ifc_len = sizeof(buf);
	ifc.ifc_buf = buf;

	if(ioctl(socket_, SIOCGIFCONF, &ifc) < 0)
	{
		ERROR_MSG("EndPoint::getInterfaces: ioctl(SIOCGIFCONF) failed.\n");
		return false;
	}

	struct ifreq * ifr         = ifc.ifc_req;
	int nInterfaces = ifc.ifc_len / sizeof(struct ifreq);
	for (int i = 0; i < nInterfaces; ++i)
	{
		struct ifreq *item = &ifr[i];

		interfaces[ ((struct sockaddr_in *)&item->ifr_addr)->sin_addr.s_addr ] =
			item->ifr_name;
	}

	return true;
#endif
}

//-------------------------------------------------------------------------------------
int EndPoint::findIndicatedInterface(const char * spec, u_int32_t & address)
{
	address = 0;

	if (spec == NULL || spec[0] == 0)
	{
		return -1;
	}

	// 是否指定地址
	if (0 == Address::string2ip(spec, address))
	{
		return 0;
	}
	else if (0 == this->getInterfaceAddressByMAC(spec, address))
	{
		return 0;
	}
	else if (0 == this->getInterfaceAddressByName(spec, address))
	{
		return 0;
	}

	return -1;
}

//-------------------------------------------------------------------------------------
int EndPoint::getInterfaceAddressByName(const char * name, u_int32_t & address)
{
	int ret = -1;

#if KBE_PLATFORM == PLATFORM_WIN32

    PIP_ADAPTER_INFO pIpAdapterInfo = new IP_ADAPTER_INFO();
    unsigned long size = sizeof(IP_ADAPTER_INFO);

    int ret_info = ::GetAdaptersInfo(pIpAdapterInfo, &size);

    if (ERROR_BUFFER_OVERFLOW == ret_info)
    {
        delete pIpAdapterInfo;
        pIpAdapterInfo = (PIP_ADAPTER_INFO)new unsigned char[size];
        ret_info = ::GetAdaptersInfo(pIpAdapterInfo, &size);    
    }

    if (ERROR_SUCCESS == ret_info)
    {
		PIP_ADAPTER_INFO _pIpAdapterInfo = pIpAdapterInfo;
		while (_pIpAdapterInfo)
		{
			if(!strcmp(_pIpAdapterInfo->AdapterName, name))
			{
				IP_ADDR_STRING* pIpAddrString = &(_pIpAdapterInfo->IpAddressList);
				ret = Address::string2ip(pIpAddrString->IpAddress.String, address);
				break;
			}

			_pIpAdapterInfo = _pIpAdapterInfo->Next;
		}
    }

    if (pIpAdapterInfo)
    {
        delete pIpAdapterInfo;
    }

#else
	
	int fd;
	int interfaceNum = 0;
	struct ifreq buf[16];
	struct ifconf ifc;

	if((fd = ::socket(AF_INET, SOCK_DGRAM, 0)) < 0)
	{
		::close(fd);
		return -1;
	}

	ifc.ifc_len = sizeof(buf);
	ifc.ifc_buf = (caddr_t)buf;

	if(!ioctl(fd, SIOCGIFCONF, (char *)&ifc))
	{
		interfaceNum = ifc.ifc_len / sizeof(struct ifreq);
		while(interfaceNum-- > 0)
		{
			if(!strcmp((char*)buf[interfaceNum].ifr_name, (char*)name))
			{
				if(!ioctl(fd, SIOCGIFADDR, (char *)&buf[interfaceNum]))
				{
					ret = Address::string2ip((const char *)inet_ntoa(((struct sockaddr_in *)&(buf[interfaceNum].ifr_addr))->sin_addr), address);
				}

				break;
			}
		}
	}

	::close(fd);

#endif

	return ret;
}

//-------------------------------------------------------------------------------------
int EndPoint::getInterfaceAddressByMAC(const char * mac, u_int32_t & address)
{
	int ret = -1;

	if(!mac)
	{
		return ret;
	}

	// mac地址转换
	unsigned char macAddress[16] = {0};
	unsigned char macAddressIdx = 0;
	char szTemp[2] = {0};
	char szTempIdx = 0;
	char* pMac = (char*)mac;
	while(*pMac && macAddressIdx < sizeof(macAddress))
	{
		if(('a' <= *pMac && *pMac <= 'f') || ('A' <= *pMac && *pMac <= 'F') || ('0' <= *pMac && *pMac <= '9'))
		{
			szTemp[szTempIdx++] = *pMac;
			if(szTempIdx > 1)
			{
				macAddress[macAddressIdx++] = (unsigned char)::strtol(szTemp, NULL, 16);
				szTempIdx = 0;
			}
		}

		++pMac;
	}

#if KBE_PLATFORM == PLATFORM_WIN32

	PIP_ADAPTER_INFO pIpAdapterInfo = new IP_ADAPTER_INFO();
	unsigned long size = sizeof(IP_ADAPTER_INFO);

	int ret_info = ::GetAdaptersInfo(pIpAdapterInfo, &size);

	if (ERROR_BUFFER_OVERFLOW == ret_info)
	{
		delete pIpAdapterInfo;
		pIpAdapterInfo = (PIP_ADAPTER_INFO)new unsigned char[size];
		ret_info = ::GetAdaptersInfo(pIpAdapterInfo, &size);    
	}

	if (ERROR_SUCCESS == ret_info)
	{
		PIP_ADAPTER_INFO _pIpAdapterInfo = pIpAdapterInfo;
		while (_pIpAdapterInfo)
		{
			if(!strcmp((char*)_pIpAdapterInfo->Address, (char*)macAddress))
			{
				IP_ADDR_STRING* pIpAddrString = &(_pIpAdapterInfo->IpAddressList);
				ret = Address::string2ip(pIpAddrString->IpAddress.String, address);
				break;
			}

			_pIpAdapterInfo = _pIpAdapterInfo->Next;
		}
	}

	if (pIpAdapterInfo)
	{
		delete pIpAdapterInfo;
	}

#else

	int fd;
	int interfaceNum = 0;
	struct ifreq buf[16];
	struct ifconf ifc;

	if((fd = ::socket(AF_INET, SOCK_DGRAM, 0)) < 0)
	{
		::close(fd);
		return -1;
	}

	ifc.ifc_len = sizeof(buf);
	ifc.ifc_buf = (caddr_t)buf;

	if(!ioctl(fd, SIOCGIFCONF, (char *)&ifc))
	{
		interfaceNum = ifc.ifc_len / sizeof(struct ifreq);
		while(interfaceNum-- > 0)
		{
			if(!ioctl(fd, SIOCGIFHWADDR, (char *)(&buf[interfaceNum])))
			{
				if(!strcmp((char*)buf[interfaceNum].ifr_hwaddr.sa_data, (char*)macAddress))
				{
					if(!ioctl(fd, SIOCGIFADDR, (char *)&buf[interfaceNum]))
					{
						ret = Address::string2ip((const char *)inet_ntoa(((struct sockaddr_in *)&(buf[interfaceNum].ifr_addr))->sin_addr), address);
					}

					break;
				}
			}
			else
			{
				break;
			}
		}
	}

	::close(fd);

#endif

	return ret;
}

//-------------------------------------------------------------------------------------
int EndPoint::findDefaultInterface(char * name, int buffsize)
{
#if KBE_PLATFORM != PLATFORM_UNIX
	strcpy(name, "eth0");
	return 0;
#else
	int		ret = -1;

	struct if_nameindex* pIfInfo = if_nameindex();
	if (pIfInfo)
	{
		int		flags = 0;
		struct if_nameindex* pIfInfoCur = pIfInfo;
		while (pIfInfoCur->if_name)
		{
			flags = 0;
			this->getInterfaceFlags(pIfInfoCur->if_name, flags);

			if ((flags & IFF_UP) && (flags & IFF_RUNNING))
			{
				u_int32_t	addr;
				if (this->getInterfaceAddress(pIfInfoCur->if_name, addr) == 0)
				{
					strncpy(name, pIfInfoCur->if_name, MAX_BUF);
					ret = 0;

					// we only stop if it's not a loopback address,
					// otherwise we continue, hoping to find a better one
					if (!(flags & IFF_LOOPBACK)) break;
				}
			}
			++pIfInfoCur;
		}
		if_freenameindex(pIfInfo);
	}
	else
	{
		ERROR_MSG(fmt::format("EndPoint::findDefaultInterface: "
							"if_nameindex returned NULL ({})\n",
						kbe_strerror()));
	}

	return ret;
#endif // unix
}

//-------------------------------------------------------------------------------------
int EndPoint::getDefaultInterfaceAddress(u_int32_t & address)
{
	int ret = -1;

	char interfaceName[MAX_BUF] = {0};
	ret = findDefaultInterface(interfaceName, MAX_BUF);
	if(0 == ret)
	{
		ret = getInterfaceAddressByName(interfaceName, address);
	}

	if(0 != ret)
	{
		char hostname[256] = {0};
		::gethostname(hostname, sizeof(hostname));
		struct hostent * host = gethostbyname(hostname);
		if(host)
		{
			if(host->h_addr_list[0] < host->h_name)
			{
				address = ((struct in_addr*)(host->h_addr_list[0]))->s_addr;
				ret = 0;
			}
		}
	}

	return ret;
}

//-------------------------------------------------------------------------------------
bool EndPoint::setBufferSize(int optname, int size)
{
	setsockopt(socket_, SOL_SOCKET, optname, (const char*)&size, sizeof(size));

	return this->getBufferSize(optname) >= size;
}

//-------------------------------------------------------------------------------------
bool EndPoint::recvAll(void * gramData, int gramSize)
{
	while (gramSize > 0)
	{
		int len = this->recv(gramData, gramSize);

		if (len <= 0)
		{
			if (len == 0)
			{
				WARNING_MSG("EndPoint::recvAll: Connection lost\n");
			}
			else
			{
				WARNING_MSG(fmt::format("EndPoint::recvAll: Got error '{}'\n",
					kbe_strerror()));
			}

			return false;
		}
		gramSize -= len;
		gramData = ((char *)gramData) + len;
	}

	return true;
}

//-------------------------------------------------------------------------------------
Network::Address EndPoint::getLocalAddress() const
{
	Network::Address addr(0, 0);

	if (this->getlocaladdress((u_int16_t*)&addr.port,
				(u_int32_t*)&addr.ip) == -1)
	{
		ERROR_MSG("EndPoint::getLocalAddress: Failed\n");
	}

	return addr;
}

//-------------------------------------------------------------------------------------
Network::Address EndPoint::getRemoteAddress() const
{
	Network::Address addr(0, 0);

	if (this->getremoteaddress((u_int16_t*)&addr.port,
				(u_int32_t*)&addr.ip) == -1)
	{
		ERROR_MSG("EndPoint::getRemoteAddress: Failed\n");
	}

	return addr;
}

//-------------------------------------------------------------------------------------
void EndPoint::initNetwork()
{
	if (g_networkInitted) 
		return;
	
	g_networkInitted = true;

#if KBE_PLATFORM == PLATFORM_WIN32
	WSAData wsdata;
	WSAStartup(0x202, &wsdata);
#endif
}

//-------------------------------------------------------------------------------------
bool EndPoint::waitSend()
{
	fd_set	fds;
	struct timeval tv = { 0, 10000 };
	FD_ZERO( &fds );
	FD_SET(socket_, &fds);

#if KBE_PLATFORM == PLATFORM_WIN32
	return select(0, NULL, &fds, NULL, &tv) > 0;
#else
	return select(socket_ + 1, NULL, &fds, NULL, &tv) > 0;
#endif
}

//-------------------------------------------------------------------------------------
void EndPoint::send(Bundle * pBundle)
{
	//AUTO_SCOPED_PROFILE("sendBundle");
	SEND_BUNDLE((*this), (*pBundle));
}

//-------------------------------------------------------------------------------------
void EndPoint::sendto(Bundle * pBundle, u_int16_t networkPort, u_int32_t networkAddr)
{
	//AUTO_SCOPED_PROFILE("sendBundle");
	SENDTO_BUNDLE((*this), networkAddr, networkPort, (*pBundle));
}

//-------------------------------------------------------------------------------------
static long ssl_bio_callback(BIO *bio, int cmd, const char *argp, int argi, long argl, long ret)
{
	if ((cmd & ~BIO_CB_RETURN) != BIO_CB_READ)
		return ret;

	Packet* pPacket = (Packet*)BIO_get_callback_arg(bio);

	// 类似recv， argi是buffer，argl是buffer长度，这里判断pPacket大于长度返回指定长度，小于长度则返回读取到的长度
	if ((int)pPacket->length() < argi)
		argi = (int)pPacket->length();

	// 将我们的buffer填充进去
	if ((cmd & BIO_CB_RETURN) > 0)
	{
		memcpy((void*)argp, pPacket->data() + pPacket->rpos(), argi);
		pPacket->read_skip(argi);
#if (OPENSSL_VERSION_NUMBER <  0x10100000)
		bio->num_read += argi;
#endif
	}
	else
	{
		return ret;
	}

	if (pPacket->length() == 0)
	{
		BIO_set_callback(bio, NULL);
		BIO_set_callback_arg(bio, (char*)NULL);
	}

	return argi;
}

bool EndPoint::setupSSL(int sslVersion, Packet* pPacket, bool useMemoryBIO)
{
	switch (sslVersion)
	{
#if (OPENSSL_VERSION_NUMBER <  0x1000207fL)
#ifndef OPENSSL_NO_SSL2
	case SSL2_VERSION:
		sslContext_ = SSL_CTX_new(SSLv2_server_method());
		break;
#endif
#ifndef OPENSSL_NO_SSL3_METHOD
	case SSL3_VERSION:
		sslContext_ = SSL_CTX_new(SSLv3_server_method());
		break;
#endif
	case TLS1_VERSION:
		sslContext_ = SSL_CTX_new(TLSv1_server_method());
		break;
	case TLS1_1_VERSION:
		sslContext_ = SSL_CTX_new(TLSv1_1_server_method());
		break;
	case TLS1_2_VERSION:
		sslContext_ = SSL_CTX_new(TLSv1_2_server_method());
		break;
#endif
	default:
		sslContext_ = SSL_CTX_new(SSLv23_server_method());
		break;
	};

	if (!sslContext_)
	{
		ERROR_MSG(fmt::format("EndPoint::setupSSL: SSL_CTX_new(SSLv23_client_method()): {}!\n", ERR_error_string(ERR_get_error(), NULL)));
		return false;
	}

	SSL_CTX_set_options(sslContext_, SSL_OP_SINGLE_DH_USE | SSL_OP_SINGLE_ECDH_USE);

	std::string pem = Resmgr::getSingleton().matchRes(g_sslCertificate.c_str());
	int use_cert = SSL_CTX_use_certificate_file(sslContext_, pem.c_str(), SSL_FILETYPE_PEM);
	if (0 >= use_cert)
	{
		ERROR_MSG(fmt::format("EndPoint::setupSSL: load SSL_CTX_use_certificate_file({}): {}! check kbengine[_defs].xml->channelCommon->sslCertificate\n",
			pem, ERR_error_string(ERR_get_error(), NULL)));

		destroySSL();
		return false;
	}

	pem = Resmgr::getSingleton().matchRes(g_sslPrivateKey.c_str());
	int use_prv = SSL_CTX_use_PrivateKey_file(sslContext_, pem.c_str(), SSL_FILETYPE_PEM);
	if (0 >= use_prv)
	{
		ERROR_MSG(fmt::format("EndPoint::setupSSL: load SSL_CTX_use_PrivateKey_file({}): {}! check kbengine[_defs].xml->channelCommon->sslPrivateKey\n",
			pem, ERR_error_string(ERR_get_error(), NULL)));

		destroySSL();
		return false;
	}

	if (!SSL_CTX_check_private_key(sslContext_)) {
		ERROR_MSG(fmt::format("EndPoint::setupSSL: SSL_CTX_check_private_key(): {}!\n", pem, ERR_error_string(ERR_get_error(), NULL)));
		destroySSL();
		return false;
	}

	sslHandle_ = SSL_new(sslContext_);

	if (!sslHandle_)
	{
		ERROR_MSG(fmt::format("EndPoint::setupSSL: SSL_new: {}!\n", ERR_error_string(ERR_get_error(), NULL)));
		destroySSL();
		return false;
	}

	sslUsesMemoryBIO_ = useMemoryBIO;
	if (sslUsesMemoryBIO_)
	{
		// IOCP/io_uring/kqueue 已经从 socket 取得密文，TLS 层只能消费显式喂入的内存 BIO。
		// IOCP/io_uring/kqueue have already consumed socket ciphertext, so TLS may only read explicitly supplied memory BIO data.
		BIO* readBIO = BIO_new(BIO_s_mem());
		BIO* writeBIO = BIO_new(BIO_s_mem());
		if (!readBIO || !writeBIO)
		{
			if (readBIO)
				BIO_free(readBIO);
			if (writeBIO)
				BIO_free(writeBIO);

			ERROR_MSG(fmt::format("EndPoint::setupSSL: BIO_new(BIO_s_mem): {}!\n", ERR_error_string(ERR_get_error(), NULL)));
			destroySSL();
			return false;
		}

		// 空输入必须表现为暂时不可读，而不是 EOF，否则分片 ClientHello 会被误判为连接关闭。
		// Empty input must mean temporarily unavailable rather than EOF, otherwise fragmented ClientHello records look disconnected.
		BIO_set_mem_eof_return(readBIO, -1);
		SSL_set_bio(sslHandle_, readBIO, writeBIO);
		SSL_set_accept_state(sslHandle_);

		const int inputLength = static_cast<int>(pPacket->length());
		if (inputLength > 0)
		{
			const int written = BIO_write(readBIO, pPacket->data() + pPacket->rpos(), inputLength);
			if (written != inputLength)
			{
				ERROR_MSG(fmt::format("EndPoint::setupSSL: BIO_write accepted {}/{} ClientHello bytes.\n", written, inputLength));
				destroySSL();
				return false;
			}

			pPacket->read_skip(inputLength);
		}

		return driveSSLHandshake();
	}

#if KBE_PLATFORM == PLATFORM_WIN32
	BIO* socketBIO = createWindowsSocketBIO(*this);
	if (!socketBIO)
	{
		ERROR_MSG(fmt::format("EndPoint::setupSSL: createWindowsSocketBIO(): {}!\n", ERR_error_string(ERR_get_error(), NULL)));
		destroySSL();
		return false;
	}

	SSL_set_bio(sslHandle_, socketBIO, socketBIO);
#else
	SSL_set_fd(sslHandle_, *this);
#endif

	BIO_set_callback(SSL_get_rbio(sslHandle_), ssl_bio_callback);
	BIO_set_callback_arg(SSL_get_rbio(sslHandle_), (char*)pPacket);

	while (SSL_accept(sslHandle_) == -1)
	{
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(*this, &fds);

		struct timeval tv = { 0, 100000 }; // 100ms

		switch (SSL_get_error(sslHandle_, -1))
		{
		case SSL_ERROR_WANT_READ:
		{
#if KBE_PLATFORM == PLATFORM_WIN32
			int selgot = select(0, &fds, NULL, NULL, &tv);
#else
			int selgot = select((*this) + 1, &fds, NULL, NULL, &tv);
#endif
			if (selgot <= 0)
			{
				ERROR_MSG(fmt::format("EndPoint::setupSSL: SSL_accept(SSL_ERROR_WANT_READ): {}!\n", ERR_error_string(SSL_get_error(sslHandle_, -1), NULL)));
				destroySSL();
				return true;
			}

			break;
		}
		case SSL_ERROR_WANT_WRITE:
		{
#if KBE_PLATFORM == PLATFORM_WIN32
			int selgot = select(0, NULL, &fds, NULL, &tv);
#else
			int selgot = select((*this) + 1, NULL, &fds, NULL, &tv);
#endif
			if (selgot <= 0)
			{
				ERROR_MSG(fmt::format("EndPoint::setupSSL: SSL_accept(SSL_ERROR_WANT_WRITE): {}!\n", ERR_error_string(SSL_get_error(sslHandle_, -1), NULL)));
				destroySSL();
				return true;
			}

			break;
		}
		default:
		{
			ERROR_MSG(fmt::format("EndPoint::setupSSL: SSL_accept: {}!\n", ERR_error_string(SSL_get_error(sslHandle_, -1), NULL)));
			destroySSL();
			return false;
		}
		}
	}

	BIO_set_callback(SSL_get_rbio(sslHandle_), NULL);
	BIO_set_callback_arg(SSL_get_rbio(sslHandle_), (char*)NULL);
	return true;
}

//-------------------------------------------------------------------------------------
bool EndPoint::driveSSLHandshake()
{
	KBE_ASSERT(sslHandle_ != NULL && sslUsesMemoryBIO_);
	if (SSL_is_init_finished(sslHandle_))
		return drainSSLNetworkOutput();

	ERR_clear_error();
	const int result = SSL_do_handshake(sslHandle_);
	const int sslError = result == 1 ? SSL_ERROR_NONE : SSL_get_error(sslHandle_, result);
	if (!drainSSLNetworkOutput())
		return false;

	if (result == 1 || sslError == SSL_ERROR_WANT_READ || sslError == SSL_ERROR_WANT_WRITE)
		return true;

	const unsigned long errorCode = ERR_get_error();
	ERROR_MSG(fmt::format("EndPoint::driveSSLHandshake: SSL_do_handshake failed, sslError={}, error={}.\n",
		sslError, errorCode ? ERR_error_string(errorCode, NULL) : "none"));
	return false;
}

//-------------------------------------------------------------------------------------
bool EndPoint::drainSSLNetworkOutput()
{
	if (!sslUsesMemoryBIO_ || !sslHandle_)
		return true;

	BIO* writeBIO = SSL_get_wbio(sslHandle_);
	char buffer[16 * 1024];
	while (BIO_ctrl_pending(writeBIO) > 0)
	{
		const int length = BIO_read(writeBIO, buffer, sizeof(buffer));
		if (length <= 0)
		{
			ERROR_MSG(fmt::format("EndPoint::drainSSLNetworkOutput: BIO_read failed: {}.\n",
				ERR_error_string(ERR_get_error(), NULL)));
			return false;
		}

		sslNetworkOutput_.insert(sslNetworkOutput_.end(), buffer, buffer + length);
	}

	return true;
}

//-------------------------------------------------------------------------------------
bool EndPoint::consumeSSLNetworkData(const void* data, int length, std::vector<char>& plaintext, bool& peerClosed)
{
	plaintext.clear();
	peerClosed = false;
	if (!sslHandle_ || !sslUsesMemoryBIO_ || length < 0)
		return false;

	BIO* readBIO = SSL_get_rbio(sslHandle_);
	if (length > 0)
	{
		const int written = BIO_write(readBIO, data, length);
		if (written != length)
		{
			ERROR_MSG(fmt::format("EndPoint::consumeSSLNetworkData: BIO_write accepted {}/{} bytes.\n", written, length));
			return false;
		}
	}

	if (!driveSSLHandshake())
		return false;

	if (!SSL_is_init_finished(sslHandle_))
		return true;

	char buffer[16 * 1024];
	for (;;)
	{
		ERR_clear_error();
		const int result = SSL_read(sslHandle_, buffer, sizeof(buffer));
		if (result > 0)
		{
			plaintext.insert(plaintext.end(), buffer, buffer + result);
			continue;
		}

		const int sslError = SSL_get_error(sslHandle_, result);
		if (sslError == SSL_ERROR_WANT_READ || sslError == SSL_ERROR_WANT_WRITE)
			break;

		if (sslError == SSL_ERROR_ZERO_RETURN)
		{
			peerClosed = true;
			break;
		}

		const unsigned long errorCode = ERR_get_error();
		ERROR_MSG(fmt::format("EndPoint::consumeSSLNetworkData: SSL_read failed, sslError={}, error={}.\n",
			sslError, errorCode ? ERR_error_string(errorCode, NULL) : "none"));
		return false;
	}

	return drainSSLNetworkOutput();
}

//-------------------------------------------------------------------------------------
bool EndPoint::encryptSSLNetworkData(const void* data, int length)
{
	if (!sslHandle_ || !sslUsesMemoryBIO_ || !SSL_is_init_finished(sslHandle_) || length < 0)
		return false;

	const char* bytes = static_cast<const char*>(data);
	int offset = 0;
	while (offset < length)
	{
		ERR_clear_error();
		const int result = SSL_write(sslHandle_, bytes + offset, length - offset);
		if (result <= 0)
		{
			const int sslError = SSL_get_error(sslHandle_, result);
			const unsigned long errorCode = ERR_get_error();
			ERROR_MSG(fmt::format("EndPoint::encryptSSLNetworkData: SSL_write failed, sslError={}, error={}.\n",
				sslError, errorCode ? ERR_error_string(errorCode, NULL) : "none"));
			return false;
		}

		offset += result;
		if (!drainSSLNetworkOutput())
			return false;
	}

	return true;
}

//-------------------------------------------------------------------------------------
bool EndPoint::takeSSLNetworkOutput(std::vector<char>& output)
{
	output.clear();
	if (sslNetworkOutput_.empty())
		return false;

	output.swap(sslNetworkOutput_);
	return true;
}

//-------------------------------------------------------------------------------------
bool EndPoint::shutdownSSL()
{
	if (!sslHandle_)
		return true;

	// SSL_shutdown 可在非阻塞 socket 上返回 WANT_READ/WANT_WRITE；状态必须保留，后续 tick 可以继续推进。
	// SSL_shutdown may return WANT_READ/WANT_WRITE on a nonblocking socket; preserve state so a later tick can continue it.
	ERR_clear_error();
	const int result = SSL_shutdown(sslHandle_);
	if (sslUsesMemoryBIO_ && !drainSSLNetworkOutput())
		return false;

	if (result >= 0)
		return true;

	const int sslError = SSL_get_error(sslHandle_, result);
	if (sslError == SSL_ERROR_WANT_READ || sslError == SSL_ERROR_WANT_WRITE)
		return true;

	const unsigned long errorCode = ERR_get_error();
	ERROR_MSG(fmt::format("EndPoint::shutdownSSL: SSL_shutdown failed, sslError={}, error={}.\n",
		sslError, errorCode ? ERR_error_string(errorCode, NULL) : "none"));
	return false;
}

//-------------------------------------------------------------------------------------
bool EndPoint::destroySSL()
{
	if (sslHandle_)
	{
		SSL_free(sslHandle_);
		sslHandle_ = NULL;
	}

	if (sslContext_)
	{
		SSL_CTX_free(sslContext_);
		sslContext_ = NULL;
	}

	sslUsesMemoryBIO_ = false;
	sslNetworkOutput_.clear();

	return true;
}

//-------------------------------------------------------------------------------------
}
}
