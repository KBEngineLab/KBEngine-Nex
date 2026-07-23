// Copyright 1998-2016 Epic Games, Inc. All Rights Reserved.

#pragma once

#include <functional>
#include "KBECommon.h"
#include "KBEventTypes.h"


/*
事件数据基础类。

不同事件可以派生出不同的数据类，用于携带事件参数。
eventName 会在事件触发时由事件系统填充，回调中可以通过它判断事件类型。
*/
class UKBEventData
{
public:
	// 事件名称，可用于对事件类型进行判断，该名称由事件触发时事件系统进行填充
	KBString eventName;
};

/*
事件模块。

事件分为 out 和 in 两个方向：
out = KBE 插件层 -> 表现层/业务层。
in  = 表现层/业务层 -> KBE 插件层。

旧接口 registerEvent / deregister / fire 继续保留，内部按 out 事件处理。

暂停语义：
pause/resume 只控制 out 事件的即时执行和 out 队列消费。
fireIn/processInEvents 不受 pause 控制。

队列语义：
fire 时会按当前监听者生成 FiredEvent 快照，放入 fired 队列。
process 时先把 fired 搬到 doing，再逐个执行，避免处理过程中再次 fire 导致重入顺序混乱。
*/
class KBEvent
{
public:
	KBEvent();
	virtual ~KBEvent();
	
public:
		/**
		 * 旧接口：注册 out 事件监听。
		 *
		 * out = KBE 插件层 -> 表现层/业务层。
		 * 保留该接口是为了兼容 KBENGINE_REGISTER_EVENT 宏和历史用户代码。
		 *
		 * @param eventName 事件名。
		 * @param funcName 回调函数名，用于注销和日志定位。
		 * @param func 实际事件回调。
		 * @param objPtr 回调所属对象指针，用于按对象注销事件。
		 * @return 注册成功返回 true。
		 */
		static bool registerEvent(const KBString& eventName, const KBString& funcName, std::function<void(std::shared_ptr<UKBEventData>)> func, void* objPtr);

		/**
		 * 注册 out 事件监听。
		 *
		 * out = KBE 插件层 -> 表现层/业务层。
		 * 常用于网络层或实体层收到服务器消息后通知 UI / 业务逻辑。
		 *
		 * @param eventName 事件名。
		 * @param funcName 回调函数名，用于注销和日志定位。
		 * @param func 实际事件回调。
		 * @param objPtr 回调所属对象指针。
		 * @return 注册成功返回 true。
		 */
		static bool registerOut(const KBString& eventName, const KBString& funcName, std::function<void(std::shared_ptr<UKBEventData>)> func, void* objPtr);

		/**
		 * 注册 in 事件监听。
		 *
		 * in = 表现层/业务层 -> KBE 插件层。
		 * 常用于 UI 或业务层触发 login/createAccount/logout 等请求，由插件层处理。
		 *
		 * @param eventName 事件名。
		 * @param funcName 回调函数名，用于注销和日志定位。
		 * @param func 实际事件回调。
		 * @param objPtr 回调所属对象指针。
		 * @return 注册成功返回 true。
		 */
		static bool registerIn(const KBString& eventName, const KBString& funcName, std::function<void(std::shared_ptr<UKBEventData>)> func, void* objPtr);

		/**
		 * 旧接口：注销指定对象的指定事件监听。
		 *
		 * 为兼容历史用法，会同时尝试从 out 和 in 注册表中移除。
		 * 如果明确知道方向，建议使用 deregisterOut 或 deregisterIn。
		 *
		 * @param objPtr 回调所属对象指针。
		 * @param eventName 事件名。
		 * @param funcName 回调函数名；为空时移除该对象在该事件上的全部回调。
		 * @return 成功移除任意注册项返回 true。
		 */
		static bool deregister(void* objPtr, const KBString& eventName, const KBString& funcName);

		/**
		 * 旧接口：注销指定对象的所有事件监听。
		 *
		 * 会同时尝试清理 out 和 in 注册表，以及两边队列中已经排队的回调。
		 *
		 * @param objPtr 回调所属对象指针。
		 * @return 成功移除任意注册项返回 true。
		 */
		static bool deregister(void* objPtr);

		/**
		 * 注销 out 事件监听。
		 *
		 * 除了从 out 注册表移除，也会清理 out fired/doing 队列中已经排队的同一回调。
		 *
		 * @param objPtr 回调所属对象指针。
		 * @param eventName 事件名。
		 * @param funcName 回调函数名；为空时移除该对象在该事件上的全部 out 回调。
		 * @return 成功移除任意注册项返回 true。
		 */
		static bool deregisterOut(void* objPtr, const KBString& eventName, const KBString& funcName);

		/**
		 * 注销 in 事件监听。
		 *
		 * 除了从 in 注册表移除，也会清理 in fired/doing 队列中已经排队的同一回调。
		 *
		 * @param objPtr 回调所属对象指针。
		 * @param eventName 事件名。
		 * @param funcName 回调函数名；为空时移除该对象在该事件上的全部 in 回调。
		 * @return 成功移除任意注册项返回 true。
		 */
		static bool deregisterIn(void* objPtr, const KBString& eventName, const KBString& funcName);

		/**
		 * 注销指定对象的所有 out 事件监听。
		 *
		 * 会同时清理 out fired/doing 队列中属于该对象的待执行事件。
		 *
		 * @param objPtr 回调所属对象指针。
		 * @return 成功移除任意注册项返回 true。
		 */
		static bool deregisterOut(void* objPtr);

		/**
		 * 注销指定对象的所有 in 事件监听。
		 *
		 * 会同时清理 in fired/doing 队列中属于该对象的待执行事件。
		 *
		 * @param objPtr 回调所属对象指针。
		 * @return 成功移除任意注册项返回 true。
		 */
		static bool deregisterIn(void* objPtr);

		// static void fire(const KBString& eventName, UKBEventData* pEventData);
		/**
		 * 旧接口：触发 out 事件。
		 *
		 * 内部等价于 fireOut，保留该接口是为了兼容 KBENGINE_EVENT_FIRE 宏和历史用户代码。
		 *
		 * @param eventName 事件名。
		 * @param pEventData 事件数据；允许为空，内部会创建 UKBEventData 基础对象。
		 */
		static void fire(const KBString& eventName, std::shared_ptr<UKBEventData> pEventData);

		/**
		 * 触发 out 事件。
		 *
		 * outEventsImmediately 为 true 且当前未 pause 时会立即调用监听者；
		 * 否则事件进入 out 队列，等待 processOutEvents。
		 *
		 * @param eventName 事件名。
		 * @param pEventData 事件数据；允许为空，内部会创建 UKBEventData 基础对象。
		 */
		static void fireOut(const KBString& eventName, std::shared_ptr<UKBEventData> pEventData);

		/**
		 * 触发 in 事件。
		 *
		 * in 事件始终进入 in 队列，不会在 fireIn 调用点直接执行。
		 * 需要调用 processInEvents 才会消费队列。
		 *
		 * @param eventName 事件名。
		 * @param pEventData 事件数据；允许为空，内部会创建 UKBEventData 基础对象。
		 */
		static void fireIn(const KBString& eventName, std::shared_ptr<UKBEventData> pEventData);

		/**
		 * 同时触发 in 和 out 事件。
		 *
		 * in 部分进入 in 队列，等待 processInEvents。
		 * out 部分进入 out 队列，等待 processOutEvents。
		 * fireAll 不做即时调用。
		 *
		 * @param eventName 事件名。
		 * @param pEventData 事件数据；允许为空，内部会创建 UKBEventData 基础对象。
		 */
		static void fireAll(const KBString& eventName, std::shared_ptr<UKBEventData> pEventData);

		/**
		 * 清理所有事件注册和所有待处理队列。
		 *
		 * 会清空 out/in 注册表，并调用 clearFiredEvents 清理 fired/doing 队列。
		 */
		static void clear();

		/**
		 * 清理所有已触发但尚未处理完成的事件，并解除 pause 状态。
		 *
		 * 只清理 fired/doing 队列，不清理 out/in 注册表。
		 */
		static void clearFiredEvents();

		/**
		 * 处理 in 事件队列。
		 *
		 * processInEvents 不受 pause 控制。
		 * 会先把 firedEvents_in_ 搬到 doingEvents_in_，再逐个执行。
		 */
		static void processInEvents();

		/**
		 * 处理 out 事件队列。
		 *
		 * processOutEvents 受 pause 控制。
		 * 会先把 firedEvents_out_ 搬到 doingEvents_out_，再逐个执行。
		 */
		static void processOutEvents();

		/**
		 * 暂停 out 事件处理。
		 *
		 * pause 后 fireOut 不再立即调用监听者，而是进入 out 队列；
		 * processOutEvents 也不会继续消费 out doing 队列。
		 * fireIn/processInEvents 不受影响。
		 */
		static void pause();

		/**
		 * 恢复 out 事件处理。
		 *
		 * 解除 pause 后会立即调用 processOutEvents 处理 out 队列。
		 * 注意：resume 不会主动处理 in 队列。
		 */
		static void resume();

		/**
		 * 当前是否处于 out 事件暂停状态。
		 *
		 * @return pause 后返回 true，resume 或 clearFiredEvents 后返回 false。
		 */
		static bool isPause();

		/**
		 * 旧接口：清理 out 队列中的待执行事件。
		 *
		 * eventName 和 funcName 都为空时，移除 objPtr 的所有 out 待执行事件；
		 * 否则只移除事件名、函数名和对象都匹配的待执行事件。
		 *
		 * @param objPtr 回调所属对象指针。
		 * @param eventName 事件名，可为空。
		 * @param funcName 回调函数名，可为空。
		 */
		static void removeFiredEvent(void* objPtr, const KBString& eventName = KBTEXT(""), const KBString& funcName = KBTEXT(""));

		/**
		 * 清理 out 队列中的待执行事件。
		 *
		 * 会同时清理 out fired 和 doing 队列。
		 *
		 * @param objPtr 回调所属对象指针。
		 * @param eventName 事件名，可为空。
		 * @param funcName 回调函数名，可为空。
		 */
		static void removeFiredEventOut(void* objPtr, const KBString& eventName = KBTEXT(""), const KBString& funcName = KBTEXT(""));

		/**
		 * 清理 in 队列中的待执行事件。
		 *
		 * 会同时清理 in fired 和 doing 队列。
		 *
		 * @param objPtr 回调所属对象指针。
		 * @param eventName 事件名，可为空。
		 * @param funcName 回调函数名，可为空。
		 */
		static void removeFiredEventIn(void* objPtr, const KBString& eventName = KBTEXT(""), const KBString& funcName = KBTEXT(""));

		/**
		 * out 事件是否尽量立即执行。
		 *
		 * true：fireOut 在未 pause 时直接调用监听者。
		 * false：fireOut 总是入队，等待 processOutEvents。
		 * 即使为 true，只要处于 pause 状态，out 事件仍会进入队列。
		 */
		static bool outEventsImmediately;

protected:
	// 单个事件监听项。
	struct EventObj
	{
		std::function<void(std::shared_ptr<UKBEventData>)> method;
		KBString funcName;
		void* objPtr;
	};

	// 已触发但尚未执行的事件对象。按监听者入队，用于固定触发瞬间的监听者快照。
	struct FiredEvent
	{
		EventObj evt;
		KBString eventName;
		std::shared_ptr<UKBEventData> args;
	};

	/**
	 * 通用注册逻辑。
	 *
	 * 注册前会先移除同对象同函数的旧注册，避免重复监听。
	 */
	static bool register_(KBMap<KBString, KBArray<EventObj>>& events, const KBString& eventName, const KBString& funcName, std::function<void(std::shared_ptr<UKBEventData>)> func, void* objPtr);

	/**
	 * 通用注销逻辑：从指定注册表中移除某个对象的指定事件监听。
	 */
	static bool deregister_(KBMap<KBString, KBArray<EventObj>>& events, void* objPtr, const KBString& eventName, const KBString& funcName);

	/**
	 * 通用注销逻辑：从指定注册表中移除某个对象的全部监听。
	 */
	static bool deregister_(KBMap<KBString, KBArray<EventObj>>& events, void* objPtr);

	/**
	 * 通用触发逻辑。
	 *
	 * eventsImmediately 为 true 且当前未 pause 时立即执行；
	 * 否则按当前监听者生成 FiredEvent 快照并进入队列。
	 */
	static void fire_(KBMap<KBString, KBArray<EventObj>>& events, KBArray<FiredEvent*>& firedEvents, const KBString& eventName, std::shared_ptr<UKBEventData> pEventData, bool eventsImmediately);

	/**
	 * 通用队列处理逻辑。
	 *
	 * 先把 firedEvents 搬到 doingEvents，再逐个执行 doingEvents。
	 * checkPause 为 true 时消费过程受 pause 控制。
	 */
	static void processFiredEvents_(KBArray<FiredEvent*>& firedEvents, KBArray<FiredEvent*>& doingEvents, bool checkPause);

	/**
	 * 清空并释放指定 fired/doing 队列中的 FiredEvent 指针。
	 */
	static void clearFiredEvents_(KBArray<FiredEvent*>& firedEvents);

	/**
	 * 从指定队列中移除匹配对象、事件名和函数名的待执行事件。
	 */
	static void removeFiredEvent_(KBArray<FiredEvent*>& firedEvents, void* objPtr, const KBString& eventName = KBTEXT(""), const KBString& funcName = KBTEXT(""));

	static KBMap<KBString, KBArray<EventObj>> events_out_;
	static KBArray<FiredEvent*> firedEvents_out_;
	static KBArray<FiredEvent*> doingEvents_out_;

	static KBMap<KBString, KBArray<EventObj>> events_in_;
	static KBArray<FiredEvent*> firedEvents_in_;
	static KBArray<FiredEvent*> doingEvents_in_;

	static bool isPauseOut_;
};


// 注册 out 事件
#define KBENGINE_REGISTER_EVENT(EVENT_NAME, EVENT_FUNC) \
	KBEvent::registerEvent(EVENT_NAME, #EVENT_FUNC, [this](std::shared_ptr<UKBEventData> pEventData) {	EVENT_FUNC(pEventData);	}, (void*)this);

// 注册 out 事件，显式 OUT 命名版本
#define KBENGINE_REGISTER_EVENT_OUT(EVENT_NAME, EVENT_FUNC) \
	KBEvent::registerOut(EVENT_NAME, #EVENT_FUNC, [this](std::shared_ptr<UKBEventData> pEventData) {	EVENT_FUNC(pEventData);	}, (void*)this);

// 注册 out 事件，可重写事件函数
#define KBENGINE_REGISTER_EVENT_OVERRIDE_FUNC(EVENT_NAME, FUNC_NAME, EVENT_FUNC) \
	KBEvent::registerEvent(EVENT_NAME, FUNC_NAME, EVENT_FUNC, (void*)this);

// 注册 out 事件，可重写事件函数，显式 OUT 命名版本
#define KBENGINE_REGISTER_EVENT_OUT_OVERRIDE_FUNC(EVENT_NAME, FUNC_NAME, EVENT_FUNC) \
	KBEvent::registerOut(EVENT_NAME, FUNC_NAME, EVENT_FUNC, (void*)this);

// 注册 in 事件
#define KBENGINE_REGISTER_EVENT_IN(EVENT_NAME, EVENT_FUNC) \
	KBEvent::registerIn(EVENT_NAME, #EVENT_FUNC, [this](std::shared_ptr<UKBEventData> pEventData) {	EVENT_FUNC(pEventData);	}, (void*)this);

// 注册 in 事件，可重写事件函数
#define KBENGINE_REGISTER_EVENT_IN_OVERRIDE_FUNC(EVENT_NAME, FUNC_NAME, EVENT_FUNC) \
	KBEvent::registerIn(EVENT_NAME, FUNC_NAME, EVENT_FUNC, (void*)this);

// 注销这个对象某个事件
#define KBENGINE_DEREGISTER_EVENT_BY_FUNCNAME(EVENT_NAME, FUNC_NAME) KBEvent::deregister((void*)this, EVENT_NAME, FUNC_NAME);
#define KBENGINE_DEREGISTER_EVENT(EVENT_NAME) KBEvent::deregister((void*)this, EVENT_NAME, KBTEXT(""));

// 注销这个对象某个 out 事件
#define KBENGINE_DEREGISTER_EVENT_OUT_BY_FUNCNAME(EVENT_NAME, FUNC_NAME) KBEvent::deregisterOut((void*)this, EVENT_NAME, FUNC_NAME);
#define KBENGINE_DEREGISTER_EVENT_OUT(EVENT_NAME) KBEvent::deregisterOut((void*)this, EVENT_NAME, KBTEXT(""));

// 注销这个对象某个 in 事件
#define KBENGINE_DEREGISTER_EVENT_IN_BY_FUNCNAME(EVENT_NAME, FUNC_NAME) KBEvent::deregisterIn((void*)this, EVENT_NAME, FUNC_NAME);
#define KBENGINE_DEREGISTER_EVENT_IN(EVENT_NAME) KBEvent::deregisterIn((void*)this, EVENT_NAME, KBTEXT(""));

// 注销这个对象所有的事件
#define KBENGINE_DEREGISTER_ALL_EVENT()	KBEvent::deregister((void*)this);

// fire out event
#define KBENGINE_EVENT_FIRE(EVENT_NAME, EVENT_DATA) KBEvent::fire(EVENT_NAME, EVENT_DATA);

// fire out event，显式 OUT 命名版本
#define KBENGINE_EVENT_FIRE_OUT(EVENT_NAME, EVENT_DATA) KBEvent::fireOut(EVENT_NAME, EVENT_DATA);

// fire in / all event
#define KBENGINE_EVENT_FIRE_IN(EVENT_NAME, EVENT_DATA) KBEvent::fireIn(EVENT_NAME, EVENT_DATA);
#define KBENGINE_EVENT_FIRE_ALL(EVENT_NAME, EVENT_DATA) KBEvent::fireAll(EVENT_NAME, EVENT_DATA);

// 暂停事件
#define KBENGINE_EVENT_PAUSE() KBEvent::pause();

// 恢复事件
#define KBENGINE_EVENT_RESUME() KBEvent::resume();

// 清除所有的事件
#define KBENGINE_EVENT_CLEAR() KBEvent::clear();

class UKBEventData_Baseapp_importClientMessages : public UKBEventData
{
public:
};

class UKBEventData_onKicked : public UKBEventData
{

public:
	int32 failedcode;

	KBString errorStr;
};

class UKBEventData_createAccount : public UKBEventData
{
public:
	KBString username;

	KBString password;

	KBArray<uint8> datas;
};

class UKBEventData_login : public UKBEventData
{

public:
	KBString username;

	KBString password;

	KBArray<uint8> datas;
};

class UKBEventData_logout : public UKBEventData
{
public:
};

class UKBEventData_onLoginFailed : public UKBEventData
{
public:
	int32 failedcode;

	KBString errorStr;

	KBArray<uint8> serverdatas;
};

class UKBEventData_onLoginBaseapp : public UKBEventData
{
public:
};

class UKBEventData_onLoginSuccessfully : public UKBEventData
{
public:
	uint64  entity_uuid;

	int32 entity_id;
};

class UKBEventData_onReloginBaseapp : public UKBEventData
{

public:
};

class UKBEventData_onLoginBaseappFailed : public UKBEventData
{

public:
	int32 failedcode;

	KBString errorStr;
};

class UKBEventData_onReloginBaseappFailed : public UKBEventData
{
public:

	int32 failedcode;

	KBString errorStr;
};

class UKBEventData_onReloginBaseappSuccessfully : public UKBEventData
{

public:
};

class UKBEventData_onVersionNotMatch : public UKBEventData
{

public:
	KBString clientVersion;
	
	KBString serverVersion;
};

class UKBEventData_onScriptVersionNotMatch : public UKBEventData
{

public:
	KBString clientScriptVersion;
	
	KBString serverScriptVersion;
};

class UKBEventData_Loginapp_importClientMessages : public UKBEventData
{

public:
};

class UKBEventData_Baseapp_importClientEntityDef : public UKBEventData
{

public:
};

class UKBEventData_onControlled : public UKBEventData
{

public:
	int entityID;

	bool isControlled;
};

class UKBEventData_onLoseControlledEntity : public UKBEventData
{
public:
	int entityID;
};

class UKBEventData_updatePosition : public UKBEventData
{
public:
	KBVector3f position;

	KBRotator direction;

	int entityID;

	float moveSpeed;

	bool isOnGround;
};

class UKBEventData_set_position : public UKBEventData
{
public:
	KBVector3f position;

	int entityID;

	float moveSpeed;

	bool isOnGround;
};

class UKBEventData_set_direction : public UKBEventData
{
public:
	// roll, pitch, yaw
	KBRotator direction;

	int entityID;
};

class UKBEventData_onCreateAccountResult : public UKBEventData
{

public:
	int errorCode;

	KBString errorStr;

	KBArray<uint8> datas;
};

class UKBEventData_addSpaceGeometryMapping : public UKBEventData
{
public:
	KBString spaceResPath;
};

class UKBEventData_onSetSpaceData : public UKBEventData
{
public:
	int spaceID;

	KBString key;

	KBString value;
};
class UKBEventData_onDelSpaceData : public UKBEventData
{

public:
	int spaceID;

	KBString key;
};

class UKBEventData_onDisconnected : public UKBEventData
{

public:
};

class UKBEventData_onConnectionState : public UKBEventData
{
public:
	bool success;

	KBString address;
};

class UKBEventData_onEnterWorld : public UKBEventData
{
public:
	KBString entityClassName;

	int spaceID;

	int entityID;

	KBString res;

	KBVector3f position;

	// roll, pitch, yaw
	KBVector3f direction;

	float moveSpeed;

	bool isOnGround;

	bool isPlayer;
};

class UKBEventData_onLeaveWorld : public UKBEventData
{
public:
	int spaceID;

	int entityID;

	bool isPlayer;
};

class UKBEventData_onEnterSpace : public UKBEventData
{

public:
	KBString entityClassName;

	int spaceID;

	int entityID;

	KBString res;

	KBVector3f position;

	// roll, pitch, yaw
	KBVector3f direction;

	float moveSpeed;

	bool isOnGround;

	bool isPlayer;
};

class UKBEventData_onLeaveSpace : public UKBEventData
{
public:
	int spaceID;

	int entityID;

	bool isPlayer;
};

class UKBEventData_resetPassword : public UKBEventData
{

public:
	KBString username;
};

class UKBEventData_onResetPassword : public UKBEventData
{
public:
	int32 failedcode;

	KBString errorStr;
};

class UKBEventData_bindAccountEmail : public UKBEventData
{

public:
	KBString email;
};

class UKBEventData_onBindAccountEmail : public UKBEventData
{
public:
	int32 failedcode;

	KBString errorStr;
};

class UKBEventData_newPassword : public UKBEventData
{

public:
	KBString old_password;

	KBString new_password;
};

class UKBEventData_onNewPassword : public UKBEventData
{

public:
	int32 failedcode;

	KBString errorStr;
};

class UKBEventData_onStreamDataStarted : public UKBEventData
{

public:
	int resID;

	int dataSize;

	KBString dataDescr;
};

class UKBEventData_onStreamDataRecv : public UKBEventData
{

public:
	int resID;

	KBArray<uint8> data;
};

class UKBEventData_onStreamDataCompleted : public UKBEventData
{

public:
	int resID;
};

class UKBEventData_onImportClientSDK : public UKBEventData
{

public:
	int remainingFiles;

	int fileSize;

	KBString fileName;

	KBArray<uint8> fileDatas;
};

class UKBEventData_onImportClientSDKSuccessfully : public UKBEventData
{
public:

};

class UKBEventData_onDownloadSDK : public UKBEventData
{
public:
	bool isDownload;
};

