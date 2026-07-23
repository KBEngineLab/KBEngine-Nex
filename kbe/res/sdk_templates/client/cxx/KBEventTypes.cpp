#include "KBEventTypes.h"

namespace KBEngine
{

// ------------------------------------账号相关------------------------------------
const KBString KBEventTypes::createAccount = "createAccount";
const KBString KBEventTypes::onCreateAccountResult = "onCreateAccountResult";
const KBString KBEventTypes::bindAccountEmail = "bindAccountEmail";
const KBString KBEventTypes::onBindAccountEmail = "onBindAccountEmail";
const KBString KBEventTypes::newPassword = "newPassword";
const KBString KBEventTypes::onNewPassword = "onNewPassword";
const KBString KBEventTypes::resetPassword = "resetPassword";
const KBString KBEventTypes::onResetPassword = "onResetPassword";

// ------------------------------------连接相关------------------------------------
const KBString KBEventTypes::onKicked = "onKicked";
const KBString KBEventTypes::onDisconnected = "onDisconnected";
const KBString KBEventTypes::onConnectionState = "onConnectionState";

// ------------------------------------logon相关------------------------------------
const KBString KBEventTypes::login = "login";
const KBString KBEventTypes::logout = "logout";
const KBString KBEventTypes::reloginBaseapp = "reloginBaseapp";
const KBString KBEventTypes::onVersionNotMatch = "onVersionNotMatch";
const KBString KBEventTypes::onScriptVersionNotMatch = "onScriptVersionNotMatch";
const KBString KBEventTypes::onLoginFailed = "onLoginFailed";
const KBString KBEventTypes::onLoginBaseapp = "onLoginBaseapp";
const KBString KBEventTypes::onLoginBaseappFailed = "onLoginBaseappFailed";
const KBString KBEventTypes::onReloginBaseapp = "onReloginBaseapp";
const KBString KBEventTypes::onReloginBaseappSuccessfully = "onReloginBaseappSuccessfully";
const KBString KBEventTypes::onReloginBaseappFailed = "onReloginBaseappFailed";

// ------------------------------------实体cell相关事件------------------------------------
const KBString KBEventTypes::onEnterWorld = "onEnterWorld";
const KBString KBEventTypes::onLeaveWorld = "onLeaveWorld";
const KBString KBEventTypes::onEnterSpace = "onEnterSpace";
const KBString KBEventTypes::onLeaveSpace = "onLeaveSpace";
const KBString KBEventTypes::set_position = "set_position";
const KBString KBEventTypes::set_direction = "set_direction";
const KBString KBEventTypes::updatePosition = "updatePosition";
const KBString KBEventTypes::addSpaceGeometryMapping = "addSpaceGeometryMapping";
const KBString KBEventTypes::onSetSpaceData = "onSetSpaceData";
const KBString KBEventTypes::onDelSpaceData = "onDelSpaceData";
const KBString KBEventTypes::onControlled = "onControlled";
const KBString KBEventTypes::onLoseControlledEntity = "onLoseControlledEntity";

// ------------------------------------数据下载相关------------------------------------
const KBString KBEventTypes::onStreamDataStarted = "onStreamDataStarted";
const KBString KBEventTypes::onStreamDataRecv = "onStreamDataRecv";
const KBString KBEventTypes::onStreamDataCompleted = "onStreamDataCompleted";

// ------------------------------------SDK更新相关-------------------------------------
const KBString KBEventTypes::onImportClientSDKSuccessfully = "onImportClientSDKSuccessfully";
const KBString KBEventTypes::onDownloadSDK = "onDownloadSDK";

}