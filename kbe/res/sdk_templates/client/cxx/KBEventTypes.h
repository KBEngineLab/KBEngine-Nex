#pragma once

#include "KBECommon.h"

namespace KBEngine
{

struct KBEventTypes
{
	// ------------------------------------账号相关------------------------------------

	// Create new account.
	// <para> param1(string): accountName</para>
	// <para> param2(string): password</para>
	// <para> param2(bytes): datas. // If you use third-party account system, the system may fill some of the third-party additional datas. </para>
	static const KBString createAccount;

	// Create account feedback results.
	// <para> param1(uint16): retcode. // server_errors</para>
	// <para> param2(bytes): datas. // If you use third-party account system, the system may fill some of the third-party additional datas. </para>
	static const KBString onCreateAccountResult;
	
	// Request server binding account Email.
	// <para> param1(string): emailAddress</para>
	static const KBString bindAccountEmail;

	// Response from binding account Email request.
	// <para> param1(uint16): retcode. // server_errors</para>
	static const KBString onBindAccountEmail;

	// Request to set up a new password for the account. Note: account must be online.
	// <para> param1(string): old_password</para>
	// <para> param2(string): new_password</para>
	static const KBString newPassword;

	// Response from a new password request.
	// <para> param1(uint16): retcode. // server_errors</para>
	static const KBString onNewPassword;

	// Request to reset password for the account. Note: account must be online.
	// <para> param1(string): username</para>
	static const KBString resetPassword;

	// Response from a reset password request.
	// <para> param1(uint16): retcode. // server_errors</para>
	static const KBString onResetPassword;

	// ------------------------------------连接相关------------------------------------

	// Kicked of the current server.
	// <para> param1(uint16): retcode. // server_errors</para>
	static const KBString onKicked;

	// Disconnected from the server.
	static const KBString onDisconnected;

	// Status of connection server.
	// <para> param1(bool): success or fail</para>
	static const KBString onConnectionState;

	// ------------------------------------logon相关------------------------------------

	// Login to server.
	// <para> param1(string): accountName</para>
	// <para> param2(string): password</para>
	// <para> param3(bytes): datas // Datas by user defined. Data will be recorded into the KBE account database, you can access the datas through the script layer. If you use third-party account system, datas will be submitted to the third-party system.</para>
	static const KBString login;

	// Relogin to baseapp.
	static const KBString logout;

	// Relogin to baseapp.
	static const KBString reloginBaseapp;

	// Engine version mismatch.
	// <para> param1(string): clientVersion
	// <para> param2(string): serverVersion
	static const KBString onVersionNotMatch;

	// script version mismatch.
	// <para> param1(string): clientScriptVersion
	// <para> param2(string): serverScriptVersion
	static const KBString onScriptVersionNotMatch;

	// Login failed.
	// <para> param1(uint16): retcode. // server_errors</para>
	static const KBString onLoginFailed;

	// Login to baseapp.
	static const KBString onLoginBaseapp;

	// Login baseapp failed.
	// <para> param1(uint16): retcode. // server_errors</para>
	static const KBString onLoginBaseappFailed;

	// Relogin to baseapp.
	static const KBString onReloginBaseapp;

	// Relogin baseapp success.
	static const KBString onReloginBaseappSuccessfully;

	// Relogin baseapp failed.
	// <para> param1(uint16): retcode. // server_errors</para>
	static const KBString onReloginBaseappFailed;

	// ------------------------------------实体cell相关事件------------------------------------

	// Entity enter the client-world.
	// <para> param1: Entity</para>
	static const KBString onEnterWorld;

	// Entity leave the client-world.
	// <para> param1: Entity</para>
	static const KBString onLeaveWorld;

	// Player enter the new space.
	// <para> param1: Entity</para>
	static const KBString onEnterSpace;

	// Player leave the space.
	// <para> param1: Entity</para>
	static const KBString onLeaveSpace;

	// Sets the current position of the entity.
	// <para> param1: Entity</para>
	static const KBString set_position;

	// Sets the current direction of the entity.
	// <para> param1: Entity</para>
	static const KBString set_direction;

	// The entity position is updated, you can smooth the moving entity to new location.
	// <para> param1: Entity</para>
	static const KBString updatePosition;

	// The current space is specified by the geometry mapping.
	// Popular said is to load the specified Map Resources.
	// <para> param1(string): resPath</para>
	static const KBString addSpaceGeometryMapping;

	// Server spaceData set data.
	// <para> param1(int32): spaceID</para>
	// <para> param2(string): key</para>
	// <para> param3(string): value</para>
	static const KBString onSetSpaceData;

	// Start downloading data.
	// <para> param1(int32): rspaceID</para>
	// <para> param2(string): key</para>
	static const KBString onDelSpaceData;

	// Triggered when the entity is controlled or out of control.
	// <para> param1: Entity</para>
	// <para> param2(bool): isControlled</para>
	static const KBString onControlled;

	// Lose controlled entity.
	// <para> param1: Entity</para>
	static const KBString onLoseControlledEntity;

	// ------------------------------------数据下载相关------------------------------------

	// Start downloading data.
	// <para> param1(uint16): resouce id</para>
	// <para> param2(uint32): data size</para>
	// <para> param3(string): description</para>
	static const KBString onStreamDataStarted;

	// Receive data.
	// <para> param1(uint16): resouce id</para>
	// <para> param2(bytes): datas</para>
	static const KBString onStreamDataRecv;

	// The downloaded data is completed.
	// <para> param1(uint16): resouce id</para>
	static const KBString onStreamDataCompleted;

	// ------------------------------------SDK更新相关-----------------------------------
	static const KBString onImportClientSDKSuccessfully;
	static const KBString onDownloadSDK;

};

}