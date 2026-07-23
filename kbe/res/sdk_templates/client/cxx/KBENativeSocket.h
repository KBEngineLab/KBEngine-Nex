// Copyright 1998-2016 Epic Games, Inc. All Rights Reserved.

#pragma once

#include "KBECommon.h"

#include <string>

namespace KBEngine
{

/*
	跨平台原生 socket 小工具。
	这里只封装平台差异，不保存业务状态，TCP 与 UDP/KCP 都通过它调用系统 socket API。
*/
namespace NativeSocket
{

#if defined(_WIN32)
using Socket = uintptr_t;
#else
using Socket = int;
#endif

constexpr Socket InvalidSocket = static_cast<Socket>(-1);
constexpr int WouldBlock = -2;

// Windows 需要初始化 Winsock；Linux/macOS 这里是空操作。
bool initialize();

// 创建并连接 TCP socket，timeoutMs 为连接超时时间。
bool connectTcp(const KBString& host, uint16 port, int timeoutMs, Socket& outSocket, std::string& outError);

// 创建并 connect UDP socket。UDP 的 connect 只是绑定默认远端，之后可直接 send/recv。
bool connectUdp(const KBString& host, uint16 port, Socket& outSocket, std::string& outError);

// 关闭 socket。内部会先 shutdown，确保阻塞中的 recv 能尽快返回。
void closeSocket(Socket& socket);

// 完整发送一段 TCP 字节流，处理短写。
bool sendAll(Socket socket, const uint8* data, int32 length, std::string& outError);

// 发送一个 UDP 数据报。UDP 保持报文边界，不能像 TCP 那样拆成多次 send。
int sendDatagram(Socket socket, const uint8* data, int32 length, std::string& outError);

// 接收数据。TCP 返回字节流片段，UDP 返回一个完整数据报；暂时无数据返回 WouldBlock。
int recvSome(Socket socket, uint8* buffer, int32 capacity, std::string& outError);

bool isValid(Socket socket);
std::string lastError();

}

}
