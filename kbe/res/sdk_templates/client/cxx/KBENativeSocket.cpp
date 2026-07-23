// Copyright 1998-2016 Epic Games, Inc. All Rights Reserved.

#include "KBENativeSocket.h"

#include <cstring>
#include <mutex>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace KBEngine
{
namespace NativeSocket
{

namespace
{
#if defined(_WIN32)
using NativeSocketHandle = SOCKET;
constexpr NativeSocketHandle NativeInvalidSocket = INVALID_SOCKET;
constexpr int NativeSocketError = SOCKET_ERROR;
#else
using NativeSocketHandle = int;
constexpr NativeSocketHandle NativeInvalidSocket = -1;
constexpr int NativeSocketError = -1;
#endif

NativeSocketHandle toNative(Socket socket)
{
	return static_cast<NativeSocketHandle>(socket);
}

Socket fromNative(NativeSocketHandle socket)
{
	return static_cast<Socket>(socket);
}

int lastErrorCode()
{
#if defined(_WIN32)
	return WSAGetLastError();
#else
	return errno;
#endif
}

bool isWouldBlock(int errorCode)
{
#if defined(_WIN32)
	return errorCode == WSAEWOULDBLOCK || errorCode == WSAEINPROGRESS || errorCode == WSAEALREADY;
#else
	return errorCode == EINPROGRESS || errorCode == EWOULDBLOCK || errorCode == EAGAIN || errorCode == EALREADY;
#endif
}

bool setNonBlocking(NativeSocketHandle socket, bool enabled)
{
#if defined(_WIN32)
	u_long mode = enabled ? 1UL : 0UL;
	return ioctlsocket(socket, FIONBIO, &mode) == 0;
#else
	const int flags = fcntl(socket, F_GETFL, 0);
	if (flags < 0)
	{
		return false;
	}

	const int newFlags = enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
	return fcntl(socket, F_SETFL, newFlags) == 0;
#endif
}

void closeNative(NativeSocketHandle socket)
{
	if (socket == NativeInvalidSocket)
	{
		return;
	}

#if defined(_WIN32)
	closesocket(socket);
#else
	close(socket);
#endif
}

void shutdownNative(NativeSocketHandle socket)
{
	if (socket == NativeInvalidSocket)
	{
		return;
	}

#if defined(_WIN32)
	shutdown(socket, SD_BOTH);
#else
	shutdown(socket, SHUT_RDWR);
#endif
}

void configureCommonOptions(NativeSocketHandle socket)
{
#if defined(__APPLE__)
	int noSigPipe = 1;
	setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, reinterpret_cast<const char*>(&noSigPipe), sizeof(noSigPipe));
#endif
}

void configureTcpOptions(NativeSocketHandle socket)
{
	configureCommonOptions(socket);

	// KBE 的小包较多，禁用 Nagle 能减少交互延迟。
	int noDelay = 1;
	setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
}

bool waitTcpConnected(NativeSocketHandle socket, int timeoutMs)
{
	fd_set writeSet;
	FD_ZERO(&writeSet);
	FD_SET(socket, &writeSet);

	fd_set errorSet;
	FD_ZERO(&errorSet);
	FD_SET(socket, &errorSet);

	timeval tv;
	tv.tv_sec = timeoutMs / 1000;
	tv.tv_usec = (timeoutMs % 1000) * 1000;

	const int result = select(static_cast<int>(socket) + 1, nullptr, &writeSet, &errorSet, &tv);
	if (result <= 0)
	{
		return false;
	}

	int socketError = 0;
	socklen_t socketErrorLen = static_cast<socklen_t>(sizeof(socketError));
	if (getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&socketError), &socketErrorLen) != 0)
	{
		return false;
	}

	return socketError == 0;
}

bool connectWithTimeout(NativeSocketHandle socket, const sockaddr* addr, socklen_t addrLen, int timeoutMs)
{
	if (!setNonBlocking(socket, true))
	{
		return false;
	}

	const int result = connect(socket, addr, addrLen);
	if (result == 0)
	{
		setNonBlocking(socket, false);
		return true;
	}

	const int errorCode = lastErrorCode();
	if (!isWouldBlock(errorCode))
	{
		return false;
	}

	const bool connected = waitTcpConnected(socket, timeoutMs);
	if (connected)
	{
		setNonBlocking(socket, false);
	}

	return connected;
}

bool resolveAndConnect(
	const KBString& host,
	uint16 port,
	int socketType,
	int protocol,
	int timeoutMs,
	Socket& outSocket,
	std::string& outError)
{
	outSocket = InvalidSocket;
	outError.clear();

	if (!initialize())
	{
		outError = lastError();
		return false;
	}

	addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = socketType;
	hints.ai_protocol = protocol;

	addrinfo* results = nullptr;
	const std::string hostAnsi = host;
	const std::string portString = std::to_string(port);
	const int resolveResult = getaddrinfo(hostAnsi.c_str(), portString.c_str(), &hints, &results);
	if (resolveResult != 0)
	{
#if defined(_WIN32)
		outError = std::to_string(resolveResult);
#else
		outError = gai_strerror(resolveResult);
#endif
		return false;
	}

	NativeSocketHandle connectedSocket = NativeInvalidSocket;
	for (addrinfo* current = results; current != nullptr; current = current->ai_next)
	{
		NativeSocketHandle socket = ::socket(current->ai_family, current->ai_socktype, current->ai_protocol);
		if (socket == NativeInvalidSocket)
		{
			continue;
		}

		if (socketType == SOCK_STREAM)
		{
			configureTcpOptions(socket);
			if (connectWithTimeout(socket, current->ai_addr, static_cast<socklen_t>(current->ai_addrlen), timeoutMs))
			{
				connectedSocket = socket;
				break;
			}
		}
		else
		{
			configureCommonOptions(socket);
			if (::connect(socket, current->ai_addr, static_cast<socklen_t>(current->ai_addrlen)) == 0)
			{
				// UDP/KCP 需要周期性重发 hello 和驱动超时，不能让 recv 永久阻塞。
				setNonBlocking(socket, true);
				connectedSocket = socket;
				break;
			}
		}

		closeNative(socket);
	}

	freeaddrinfo(results);

	if (connectedSocket == NativeInvalidSocket)
	{
		outError = lastError();
		return false;
	}

	outSocket = fromNative(connectedSocket);
	return true;
}

int sendNoSignalFlag()
{
#if defined(__linux__)
	return MSG_NOSIGNAL;
#else
	return 0;
#endif
}
}

bool initialize()
{
#if defined(_WIN32)
	static std::once_flag once;
	static bool succeeded = false;
	std::call_once(once, []()
	{
		WSADATA data;
		succeeded = WSAStartup(MAKEWORD(2, 2), &data) == 0;
	});
	return succeeded;
#else
	return true;
#endif
}

bool connectTcp(const KBString& host, uint16 port, int timeoutMs, Socket& outSocket, std::string& outError)
{
	return resolveAndConnect(host, port, SOCK_STREAM, IPPROTO_TCP, timeoutMs, outSocket, outError);
}

bool connectUdp(const KBString& host, uint16 port, Socket& outSocket, std::string& outError)
{
	return resolveAndConnect(host, port, SOCK_DGRAM, IPPROTO_UDP, 0, outSocket, outError);
}

void closeSocket(Socket& socket)
{
	if (!isValid(socket))
	{
		return;
	}

	const NativeSocketHandle nativeSocket = toNative(socket);
	socket = InvalidSocket;

	shutdownNative(nativeSocket);
	closeNative(nativeSocket);
}

bool sendAll(Socket socket, const uint8* data, int32 length, std::string& outError)
{
	outError.clear();
	if (!isValid(socket))
	{
		outError = "invalid socket";
		return false;
	}

	int32 sentTotal = 0;
	while (sentTotal < length)
	{
		const int sent = ::send(
			toNative(socket),
			reinterpret_cast<const char*>(data + sentTotal),
			length - sentTotal,
			sendNoSignalFlag());

		if (sent == NativeSocketError || sent <= 0)
		{
			outError = lastError();
			return false;
		}

		sentTotal += sent;
	}

	return true;
}

int sendDatagram(Socket socket, const uint8* data, int32 length, std::string& outError)
{
	outError.clear();
	if (!isValid(socket))
	{
		outError = "invalid socket";
		return NativeSocketError;
	}

	const int sent = ::send(
		toNative(socket),
		reinterpret_cast<const char*>(data),
		length,
		sendNoSignalFlag());

	if (sent == NativeSocketError)
	{
		outError = lastError();
	}

	return sent;
}

int recvSome(Socket socket, uint8* buffer, int32 capacity, std::string& outError)
{
	outError.clear();
	if (!isValid(socket))
	{
		outError = "invalid socket";
		return NativeSocketError;
	}

	const int received = ::recv(toNative(socket), reinterpret_cast<char*>(buffer), capacity, 0);
	if (received == NativeSocketError)
	{
		if (isWouldBlock(lastErrorCode()))
		{
			return WouldBlock;
		}

		outError = lastError();
	}

	return received;
}

bool isValid(Socket socket)
{
	return socket != InvalidSocket;
}

std::string lastError()
{
#if defined(_WIN32)
	return std::to_string(WSAGetLastError());
#else
	return strerror(errno);
#endif
}

}
}
