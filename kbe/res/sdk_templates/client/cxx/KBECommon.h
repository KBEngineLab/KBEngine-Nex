// Fill out your copyright notice in the Description page of Project Settings.


#pragma once

#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <string>


#if defined(__UNREAL__) || defined(UE_BUILD_DEBUG) || defined(UE_SERVER) || defined(UE_GAME) || defined(UE_CLIENT) || defined(UE_BUILD_DEVELOPMENT) || defined(UE_BUILD_SHIPPING)
    #define KBE_PLATFORM_UE 1
#elif defined(CC_TARGET_PLATFORM)
    #define KBE_PLATFORM_COCOS 1
#else
    #define KBE_PLATFORM_CPP 1
#endif


// ---------------------------
// UE Version
// ---------------------------
#if defined(KBE_PLATFORM_UE)
	#include "Misc/AssertionMacros.h"
	#define KBE_ASSERT(expr) check(expr)

// ---------------------------
// Cocos2d-x Version
// ---------------------------
#elif defined(KBE_PLATFORM_COCOS)
	#include "base/CCPlatformMacros.h" // cocos2d-x 宏
	#define KBE_ASSERT(expr) CCASSERT(expr, "KBE_ASSERT failed")

// ---------------------------
// Native C++ Version
// ---------------------------
#else
	#include <iostream>

	#define KBE_ASSERT(expr)                                                     \
	do {                                                                     \
	if (!(expr)) {                                                       \
	std::cerr << "Assertion failed: " #expr                          \
	<< " (" << __FILE__ << ":" << __LINE__ << ")\n";       \
	std::abort();                                                    \
	}                                                                     \
	} while (0)

#endif




#include "KBEBasicTypes.h"
#include "KBVariant.h"

typedef uint16 MessageID;
typedef uint16 MessageLength;
typedef uint32 MessageLengthEx;
typedef int32 ENTITY_ID;
typedef uint32 SPACE_ID;
typedef uint64 DBID;
typedef KBArray<uint8> ByteArray;
typedef KBMap<KBString, KBEngine::KBVariant> KB_FIXED_DICT;
typedef KBArray<KBEngine::KBVariant> KB_ARRAY;

#define KBE_FLT_MAX FLT_MAX

/** 安全的释放一个指针内存 */
#define KBE_SAFE_RELEASE(i)									\
	if (i)													\
		{													\
			delete i;										\
			i = NULL;										\
		}

/** 安全的释放一个指针数组内存 */
#define KBE_SAFE_RELEASE_ARRAY(i)							\
	if (i)													\
		{													\
			delete[] i;										\
			i = NULL;										\
		}

struct FKServerErr
{

	FKServerErr()
	: name()
	, descr()
	, id(0)
	{
	}

	std::string name;

	std::string descr;

	int32 id;
};

// 客户端的类别
// http://www.kbengine.org/docs/programming/clientsdkprogramming.html
// http://www.kbengine.org/cn/docs/programming/clientsdkprogramming.html
enum class EKCLIENT_TYPE : uint8
{
	CLIENT_TYPE_UNKNOWN = 0,
	CLIENT_TYPE_MOBILE,
	CLIENT_TYPE_WIN,
	CLIENT_TYPE_LINUX,
	CLIENT_TYPE_MAC,
	CLIENT_TYPE_BROWSER,
	CLIENT_TYPE_BOTS,
	CLIENT_TYPE_MINI,
};

//加密通讯的类别
enum class NETWORK_ENCRYPT_TYPE : uint8
{
	ENCRYPT_TYPE_NONE = 0,
	ENCRYPT_TYPE_BLOWFISH,
};

// 网络消息类别
enum NETWORK_MESSAGE_TYPE
{
	NETWORK_MESSAGE_TYPE_COMPONENT = 0,		// 组件消息
	NETWORK_MESSAGE_TYPE_ENTITY = 1,		// entity消息
};

enum ProtocolType
{
	PROTOCOL_TCP = 0,
	PROTOCOL_UDP = 1,
};

enum EntityDataFlags
{
	ED_FLAG_UNKOWN = 0x00000000,			// 未定义
	ED_FLAG_CELL_PUBLIC = 0x00000001,		// 相关所有cell广播
	ED_FLAG_CELL_PRIVATE = 0x00000002,		// 当前cell
	ED_FLAG_ALL_CLIENTS = 0x00000004,		// cell广播与所有客户端
	ED_FLAG_CELL_PUBLIC_AND_OWN = 0x00000008, // cell广播与自己的客户端
	ED_FLAG_OWN_CLIENT = 0x00000010,		// 当前cell和客户端
	ED_FLAG_BASE_AND_CLIENT = 0x00000020,	// base和客户端
	ED_FLAG_BASE = 0x00000040,				// 当前base
	ED_FLAG_OTHER_CLIENTS = 0x00000080,		// cell广播和其他客户端
};

// 加密额外存储的信息占用字节(长度+填充)
#define ENCRYPTTION_WASTAGE_SIZE			(1 + 7)

#define PACKET_MAX_SIZE						1500
#ifndef PACKET_MAX_SIZE_TCP
#define PACKET_MAX_SIZE_TCP					1460
#endif
#define PACKET_MAX_SIZE_UDP					1472

typedef uint16								PacketLength;				// 最大65535
#define PACKET_LENGTH_SIZE					sizeof(PacketLength)

#define NETWORK_MESSAGE_ID_SIZE				sizeof(MessageID)
#define NETWORK_MESSAGE_LENGTH_SIZE			sizeof(MessageLength)
#define NETWORK_MESSAGE_LENGTH1_SIZE		sizeof(MessageLengthEx)
#define NETWORK_MESSAGE_MAX_SIZE			65535
#define NETWORK_MESSAGE_MAX_SIZE1			4294967295

// 游戏内容可用包大小
#define GAME_PACKET_MAX_SIZE_TCP			PACKET_MAX_SIZE_TCP - NETWORK_MESSAGE_ID_SIZE - \
											NETWORK_MESSAGE_LENGTH_SIZE - ENCRYPTTION_WASTAGE_SIZE

/*
	网络消息类型， 定长或者变长。
	如果需要自定义长度则在NETWORK_INTERFACE_DECLARE_BEGIN中声明时填入长度即可。
*/
#ifndef NETWORK_FIXED_MESSAGE
#define NETWORK_FIXED_MESSAGE 0
#endif

#ifndef NETWORK_VARIABLE_MESSAGE
#define NETWORK_VARIABLE_MESSAGE -1
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/*
	网络MTU最大值
*/

#define TCP_PACKET_MAX 1460

double getTimeSeconds();

inline float int82angle(int8 angle, bool half)
{
	float halfv = 128.f;
	if (half == true)
		halfv = 254.f;

	halfv = ((float)angle) * ((float)M_PI / halfv);
	return halfv;
}

inline bool almostEqual(float f1, float f2, float epsilon)
{
	return std::fabs(f1 - f2) < epsilon;
}

inline bool isNumeric(KBEngine::KBVariant& v)
{
	return v.type() == KBEngine::KBVariant::Type::Bool ||
		v.type() == KBEngine::KBVariant::Type::Double ||
		v.type() == KBEngine::KBVariant::Type::Float ||
		v.type() == KBEngine::KBVariant::Type::Int8 ||
		v.type() == KBEngine::KBVariant::Type::Int16 ||
		v.type() == KBEngine::KBVariant::Type::Int32 ||
		v.type() == KBEngine::KBVariant::Type::Int64 ||
		v.type() == KBEngine::KBVariant::Type::UInt8 ||
		v.type() == KBEngine::KBVariant::Type::UInt16 ||
		v.type() == KBEngine::KBVariant::Type::UInt32 ||
		v.type() == KBEngine::KBVariant::Type::UInt64;
}



// =======================================
//  cross-platform safe_stoll for KBString
// =======================================
inline int64 safe_stoll(const KBCHAR* s)
{
	if (s == nullptr || *s == '\0')
		return 0;

	return std::stoll(s);   // 直接解析 UTF-8 char*
}



// 支持 KBString
inline int64 safe_stoll(const KBString& s)
{
	return safe_stoll(s.c_str());
}



inline int64 safe_atoi64(const KBCHAR* s)
{
	if (s == nullptr || *s == '\0')
		return 0;

	errno = 0;
	char* end = nullptr;

	int64 v = strtoll(s, &end, 10);

	// 无法解析 or 溢出 → 返回 0（与 UE 行为一致）
	if (end == s || errno == ERANGE)
		return 0;

	return v;
}

inline int64 safe_atoi64(const KBString& s)
{
	return safe_atoi64(s.c_str());
}




inline double safe_atod(const KBCHAR* s)
{
	if (s == nullptr || *s == '\0')
		return 0.0;

	errno = 0;
	char* end = nullptr;

	double v = strtod(s, &end);

	if (end == s || errno == ERANGE)
		return 0.0;

	return v;
}

inline double safe_atod(const KBString& s)
{
	return safe_atod(s.c_str());
}


inline float safe_atof(const KBCHAR* s)
{
	if (s == nullptr || *s == '\0')
		return 0.0f;

	errno = 0;
	char* end = nullptr;

	float v = strtof(s, &end);

	if (end == s || errno == ERANGE)
		return 0.0f;

	return v;
}

inline float safe_atof(const KBString& s)
{
	return safe_atof(s.c_str());
}


//
// // UE4的尺度单位转化为米
// #define UE4_SCALE_UNIT_TO_METER 100.f
//
// // 将KBE坐标系的position(Vector3)转换为UE4坐标系的位置
// inline void KBPos2UE4Pos(KBVector3f& UE4_POSITION, const KBVector3f& KBE_POSITION)
// {
// 	// UE4坐标单位为厘米， KBE单位为米， 因此转化需要常量
// 	UE4_POSITION.y = KBE_POSITION.x * UE4_SCALE_UNIT_TO_METER;
// 	UE4_POSITION.z = KBE_POSITION.y * UE4_SCALE_UNIT_TO_METER;
// 	UE4_POSITION.x = KBE_POSITION.z * UE4_SCALE_UNIT_TO_METER;
// }
//
// // 将UE4坐标系的position(Vector3)转换为KBE坐标系的位置
// inline void UE4Pos2KBPos(KBVector3f& KBE_POSITION, const KBVector3f& UE4_POSITION)
// {
// 	// UE4坐标单位为厘米， KBE单位为米， 因此转化需要常量
// 	KBE_POSITION.x = UE4_POSITION.y / UE4_SCALE_UNIT_TO_METER;
// 	KBE_POSITION.y = UE4_POSITION.z / UE4_SCALE_UNIT_TO_METER;
// 	KBE_POSITION.z = UE4_POSITION.x / UE4_SCALE_UNIT_TO_METER;
// }
//
// // 将KBE方向转换为UE4方向
// inline void KBDir2UE4Dir(FRotator& UE4_DIRECTION, const KBVector3f& KBE_DIRECTION)
// {
// 	UE4_DIRECTION.Pitch = FMath::RadiansToDegrees<float>(KBE_DIRECTION.Y);
// 	UE4_DIRECTION.Yaw = FMath::RadiansToDegrees<float>(KBE_DIRECTION.Z);
// 	UE4_DIRECTION.Roll = FMath::RadiansToDegrees<float>(KBE_DIRECTION.X);
// }

// 将UE4方向转换为KBE方向
// inline void UE4Dir2KBDir(KBVector3f& KBE_DIRECTION, const FRotator& UE4_DIRECTION)
// {
// 	KBE_DIRECTION.Y = FMath::DegreesToRadians<float>(UE4_DIRECTION.Pitch);
// 	KBE_DIRECTION.Z = FMath::DegreesToRadians<float>(UE4_DIRECTION.Yaw);
// 	KBE_DIRECTION.X = FMath::DegreesToRadians<float>(UE4_DIRECTION.Roll);
// }


