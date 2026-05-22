// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com


#include "endpoint.h"
#ifndef CODE_INLINE
#include "endpoint.inl"
#endif

#include "resmgr/resmgr.h"
#include <openssl/err.h>

#include "network/bundle.h"
#include "network/tcp_packet_receiver.h"
#include "network/tcp_packet_sender.h"
#include "network/udp_packet_receiver.h"
#include <cstring>

#if KBE_PLATFORM == PLATFORM_WIN32
#include <Iphlpapi.h>
#pragma comment (lib,"iphlpapi.lib") 
#else
#include <net/if.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#endif

namespace KBEngine { 
namespace Network
{
#if KBE_PLATFORM_UNIX_FAMILY
#else	// not unix
	// Need to implement if_nameindex functions on Windows
	/** @internal */
	struct if_nameindex
	{

		unsigned int if_index;	/* 1, 2, ... */

		char *if_name;			/* null terminated name: "eth0", ... */

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

#if KBE_PLATFORM_UNIX_FAMILY
static bool isVirtualLikeInterfaceName(const char* ifname)
{
	if (ifname == nullptr)
		return false;

	static const char* kPrefixes[] = {
		"utun", "vmnet", "vboxnet", "bridge", "awdl", "llw", "anpi"
	};

	for (const char* prefix : kPrefixes)
	{
		if (std::strncmp(ifname, prefix, std::strlen(prefix)) == 0)
			return true;
	}

	return false;
}

static int scoreInterface(unsigned int flags, const char* ifname)
{
	int score = 0;

	if (flags & IFF_UP) score += 8;
	if (flags & IFF_RUNNING) score += 4;
	if (flags & IFF_BROADCAST) score += 4;
	if (flags & IFF_LOOPBACK) score -= 8;
	if (flags & IFF_POINTOPOINT) score -= 6;
	if (isVirtualLikeInterfaceName(ifname)) score -= 5;

	return score;
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

#if defined(__linux__)
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
#endif // __linux__

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
#elif KBE_PLATFORM_UNIX_FAMILY
	struct ifaddrs* ifaddr = nullptr;
	if (getifaddrs(&ifaddr) != 0 || ifaddr == nullptr)
	{
		ERROR_MSG(fmt::format("EndPoint::getInterfaces: getifaddrs failed: {}\n", kbe_strerror()));
		return false;
	}

	for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next)
	{
		if (ifa->ifa_name == nullptr || ifa->ifa_addr == nullptr)
			continue;

		if (ifa->ifa_addr->sa_family != AF_INET)
			continue;

		unsigned int flags = ifa->ifa_flags;
		if (!(flags & IFF_UP) || (flags & IFF_LOOPBACK))
			continue;

		u_int32_t ip = ((struct sockaddr_in*)ifa->ifa_addr)->sin_addr.s_addr;
		interfaces[ip] = ifa->ifa_name;
	}

	freeifaddrs(ifaddr);
	return !interfaces.empty();

#elif defined(__linux__)
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

	return false;
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

#elif KBE_PLATFORM_UNIX_FAMILY
		
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

#else
	ret = -1;

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

#elif defined(__linux__)

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

#else
	ret = -1;

#endif

	return ret;
}

//-------------------------------------------------------------------------------------
int EndPoint::findDefaultInterface(char * name, int buffsize)
{
#if !KBE_PLATFORM_UNIX_FAMILY
	strcpy(name, "eth0");
	return 0;
#else
	int		ret = -1;

	// Prefer getifaddrs on Unix-family systems (macOS/Linux) and select
	// the most suitable IPv4 interface for LAN broadcast traffic.
	struct ifaddrs* ifaddr = nullptr;
	if (getifaddrs(&ifaddr) == 0 && ifaddr != nullptr)
	{
		int bestScore = -99999;
		std::string bestName;

		for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next)
		{
			if (ifa->ifa_name == nullptr || ifa->ifa_addr == nullptr)
				continue;

			if (ifa->ifa_addr->sa_family != AF_INET)
				continue;

			unsigned int flags = ifa->ifa_flags;
			if (!(flags & IFF_UP))
				continue;

			int score = scoreInterface(flags, ifa->ifa_name);
			if (score > bestScore)
			{
				bestScore = score;
				bestName = ifa->ifa_name;
			}
		}

		freeifaddrs(ifaddr);

		if (!bestName.empty())
		{
			kbe_snprintf(name, buffsize, "%s", bestName.c_str());
			return 0;
		}
	}

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
					kbe_snprintf(name, buffsize, "%s", pIfInfoCur->if_name);
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
	address = 0;

#if KBE_PLATFORM_UNIX_FAMILY
	// Prefer the best IPv4 interface for broadcast/local traffic.
	struct ifaddrs* ifaddr = nullptr;
	if (getifaddrs(&ifaddr) == 0 && ifaddr != nullptr)
	{
		int bestScore = -99999;
		u_int32_t bestAddress = 0;

		for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next)
		{
			if (ifa->ifa_addr == nullptr || ifa->ifa_name == nullptr)
				continue;

			if (ifa->ifa_addr->sa_family != AF_INET)
				continue;

			unsigned int flags = ifa->ifa_flags;
			if (!(flags & IFF_UP))
				continue;

			u_int32_t ip = ((struct sockaddr_in*)ifa->ifa_addr)->sin_addr.s_addr;
			int score = scoreInterface(flags, ifa->ifa_name);
			if (score > bestScore)
			{
				bestScore = score;
				bestAddress = ip;
			}
		}

		freeifaddrs(ifaddr);
		if (bestAddress != 0)
		{
			address = bestAddress;
			return 0;
		}
	}
#endif

	char hostname[256] = {0};
	::gethostname(hostname, sizeof(hostname));
	struct hostent* host = gethostbyname(hostname);
	if (host && host->h_addr_list && host->h_addr_list[0])
	{
		address = ((struct in_addr*)(host->h_addr_list[0]))->s_addr;
		return 0;
	}

	return -1;
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

	return select(static_cast<int>(socket_ + 1), NULL, &fds, NULL, &tv) > 0;
}

//-------------------------------------------------------------------------------------
void EndPoint::send(Bundle * pBundle)
{
#if KBE_PLATFORM == PLATFORM_WIN32
#pragma warning(push)
#pragma warning(disable:4267)
#endif
	//AUTO_SCOPED_PROFILE("sendBundle");
	SEND_BUNDLE((*this), (*pBundle));
#if KBE_PLATFORM == PLATFORM_WIN32
#pragma warning(pop)
#endif
}

//-------------------------------------------------------------------------------------
void EndPoint::sendto(Bundle * pBundle, u_int16_t networkPort, u_int32_t networkAddr)
{
#if KBE_PLATFORM == PLATFORM_WIN32
#pragma warning(push)
#pragma warning(disable:4267)
#endif
	//AUTO_SCOPED_PROFILE("sendBundle");
	SENDTO_BUNDLE((*this), networkAddr, networkPort, (*pBundle));
#if KBE_PLATFORM == PLATFORM_WIN32
#pragma warning(pop)
#endif
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

bool EndPoint::setupSSL(int sslVersion, Packet* pPacket)
{
#if (OPENSSL_VERSION_NUMBER <  0x1000207fL)
	switch (sslVersion)
	{
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
	default:
		sslContext_ = SSL_CTX_new(SSLv23_server_method());
		break;
	};
#else
	// 新版 OpenSSL 已经不再暴露旧协议 method，统一用协商入口创建 server context。
	sslContext_ = SSL_CTX_new(SSLv23_server_method());
#endif

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

	SSL_set_fd(sslHandle_, static_cast<int>(*this));

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
			int selgot = select(static_cast<int>((*this) + 1), &fds, NULL, NULL, &tv);
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
			int selgot = select(static_cast<int>((*this) + 1), NULL, &fds, NULL, &tv);
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

	return true;
}

//-------------------------------------------------------------------------------------
}
}
