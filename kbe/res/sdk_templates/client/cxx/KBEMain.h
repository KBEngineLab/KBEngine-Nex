// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "KBECommon.h"
#include "KBEvent.h"

/*
可以理解为插件的入口模块
在这个入口中安装了需要监听的事件(installEvents)，同时初始化KBEngine(initKBEngine)
*/
class KBEngineApp;


class  KBEMain
{

public:	
	// Sets default values for this component's properties
	KBEMain();
	// 析构
	~KBEMain();

	bool init();


	// void installEvents();
	// void deregisterEvents();

	// void onVersionNotMatch(std::shared_ptr<UKBEventData> pEventData);
	// void onScriptVersionNotMatch(std::shared_ptr<UKBEventData> pEventData);



	/**
	 * 客户端版本
	 */
	static KBString getClientVersion();

	/**
	 * 客户端脚本版本
	 */
	static KBString getClientScriptVersion();

	/**
	 * 服务器版本
	 */
	static KBString getServerVersion();

	/**
	 * 服务器脚本版本
	 */
	static KBString getServerScriptVersion();

	/*
		客户端属于KBE框架中的一个功能组件，这里获取将固定返回client
	*/
	static KBString getComponentName();

	/**
		在程序关闭时需要主动调用, 彻底销毁KBEngine
	*/
	bool destroyKBEngine();

	/**
	 * 用户登录
	 */
	bool login(KBString username, KBString password, KBArray<uint8> datas);

	/*
		创建账号
	*/
	bool createAccount(KBString username, KBString password, const KBArray<uint8>& datas);

	/**
	 * 重置密码
	 */
	bool resetPassword(KBString username);

	/**
	 * 绑定账号邮箱
	 */
	bool bindAccountEmail(KBString email);

	/**
	 * 修改密码
	 */
	bool newPassword(KBString oldPassword, KBString newPassword);

	/**
	 * KBEngine服务器IP地址
	 */
	KBString ip;

	/**
	 * KBEngine服务器端口
	 */
	int port;

	/**
	 * 客户端类型
	 */
	EKCLIENT_TYPE clientType;

	/**
	 * 网络加密类型
	 */
	NETWORK_ENCRYPT_TYPE networkEncryptType;

	/**
	 * 同步玩家数据的时间间隔，单位：毫秒
	 */
	int syncPlayerMS;


	/**
	 * 是否使用别名实体ID
	 */
	bool useAliasEntityID;

	/**
	 * 是否在初始化时调用属性的set方法
	 */
	bool isOnInitCallPropertysSetMethods;

	/**
	 * 是否强制禁用UDP
	 */
	bool forceDisableUDP;

	/**
	 * 服务器心跳间隔时间，单位：秒
	 */
	int serverHeartbeatTick;

	/**
	 * TCP发送缓冲区大小
	 */
	int TCP_SEND_BUFFER_MAX;

	/**
	 * TCP接收缓冲区大小
	 */
	int TCP_RECV_BUFFER_MAX;

	/**
	 * UDP发送缓冲区大小
	 */
	int UDP_SEND_BUFFER_MAX;

	/**
	 * UDP接收缓冲区大小
	 */
	int UDP_RECV_BUFFER_MAX;

	/**
	* 控制主循环是否禁用
	* true表示禁用主循环，false表示启用主循环
	* 禁用后需要手动调用 KBEngine::KBEngineApp::getSingleton().process();
	*/
	bool disableMainLoop;



};
