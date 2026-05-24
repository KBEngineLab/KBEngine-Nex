
#include "KBEMain.h"
#include "KBEngine.h"
#include "KBEngineArgs.h"
#include "MemoryStream.h"
#include "Bundle.h"
#include "KBDebug.h"
#include "Entity.h"


KBEMain::KBEMain()
{
	ip = KBTEXT("127.0.0.1");
	port = @{KBE_LOGIN_PORT};
	syncPlayerMS = 1000 / @{KBE_UPDATEHZ};
	useAliasEntityID = @{KBE_USE_ALIAS_ENTITYID};
	isOnInitCallPropertysSetMethods = true;
	forceDisableUDP = false;
	clientType = EKCLIENT_TYPE::CLIENT_TYPE_WIN;
	networkEncryptType = NETWORK_ENCRYPT_TYPE::ENCRYPT_TYPE_NONE;
	serverHeartbeatTick = @{KBE_SERVER_EXTERNAL_TIMEOUT};
	TCP_SEND_BUFFER_MAX = TCP_PACKET_MAX;
	TCP_RECV_BUFFER_MAX = TCP_PACKET_MAX;
	UDP_SEND_BUFFER_MAX = 128;
	UDP_RECV_BUFFER_MAX = 128;
	disableMainLoop = false;

}

// 析构
KBEMain::~KBEMain()
{
	// deregisterEvents();
}

bool KBEMain::init()
{
	KBEngine::KBEngineArgs* pArgs = new KBEngine::KBEngineArgs();
	pArgs->ip = ip;
	pArgs->port = port;
	pArgs->syncPlayerMS = syncPlayerMS;
	pArgs->useAliasEntityID = useAliasEntityID;
	pArgs->isOnInitCallPropertysSetMethods = isOnInitCallPropertysSetMethods;
	pArgs->forceDisableUDP = forceDisableUDP;
	pArgs->clientType = clientType;
	pArgs->networkEncryptType = networkEncryptType;
	pArgs->serverHeartbeatTick = serverHeartbeatTick / 2;
	pArgs->TCP_SEND_BUFFER_MAX = TCP_SEND_BUFFER_MAX;
	pArgs->TCP_RECV_BUFFER_MAX = TCP_RECV_BUFFER_MAX;
	pArgs->UDP_SEND_BUFFER_MAX = UDP_SEND_BUFFER_MAX;
	pArgs->UDP_RECV_BUFFER_MAX = UDP_RECV_BUFFER_MAX;
	pArgs->disableMainLoop = disableMainLoop;

	if(!KBEngine::KBEngineApp::getSingleton().initialize(pArgs)){
		delete pArgs;
		return false;
	}

	return true;
}


KBString KBEMain::getClientVersion()
{
	if (!KBEngine::KBEngineApp::getSingleton().isInitialized())
		return KBTEXT("");

	return KBEngine::KBEngineApp::getSingleton().clientVersion();
}

KBString KBEMain::getClientScriptVersion()
{
	if (!KBEngine::KBEngineApp::getSingleton().isInitialized())
		return KBTEXT("");

	return KBEngine::KBEngineApp::getSingleton().clientScriptVersion();
}

KBString KBEMain::getServerVersion()
{
	if (!KBEngine::KBEngineApp::getSingleton().isInitialized())
		return KBTEXT("");

	return KBEngine::KBEngineApp::getSingleton().serverVersion();
}

KBString KBEMain::getServerScriptVersion()
{
	if (!KBEngine::KBEngineApp::getSingleton().isInitialized())
		return KBTEXT("");

	return KBEngine::KBEngineApp::getSingleton().serverScriptVersion();
}

KBString KBEMain::getComponentName()
{
	if (!KBEngine::KBEngineApp::getSingleton().isInitialized())
		return KBTEXT("");

	return KBEngine::KBEngineApp::getSingleton().component();
}

bool KBEMain::destroyKBEngine()
{
	if (!KBEngine::KBEngineApp::getSingleton().isInitialized())
		return false;

	KBEngine::KBEngineApp::getSingleton().destroy();
	KBENGINE_EVENT_CLEAR();
	return true;
}

bool KBEMain::login(KBString username, KBString password, KBArray<uint8> datas)
{
	if (!KBEngine::KBEngineApp::getSingleton().isInitialized())
	{
		return false;
	}

	KBEngine::KBEngineApp::getSingleton().reset();

	auto pEventData = std::make_shared<UKBEventData_login>();
	pEventData->username = username;
	pEventData->password = password;
	pEventData->datas = datas;
	KBENGINE_EVENT_FIRE_IN(KBEngine::KBEventTypes::login, pEventData);
	return true;
}

bool KBEMain::createAccount(KBString username, KBString password, const KBArray<uint8>& datas)
{
	if (!KBEngine::KBEngineApp::getSingleton().isInitialized())
	{
		return false;
	}

	KBEngine::KBEngineApp::getSingleton().reset();

	auto pEventData = std::make_shared<UKBEventData_createAccount>();
	pEventData->username = username;
	pEventData->password = password;
	pEventData->datas = datas;
	KBENGINE_EVENT_FIRE_IN(KBEngine::KBEventTypes::createAccount, pEventData);
	return true;
}

bool KBEMain::resetPassword(KBString username)
{
	if (!KBEngine::KBEngineApp::getSingleton().isInitialized())
	{
		return false;
	}
	auto pEventData = std::make_shared<UKBEventData_resetPassword>();
	pEventData->username = username;
	KBENGINE_EVENT_FIRE_IN(KBEngine::KBEventTypes::resetPassword, pEventData);
	return true;
}
bool KBEMain::bindAccountEmail(KBString email)
{
	if (!KBEngine::KBEngineApp::getSingleton().isInitialized())
	{
		return false;
	}
	auto pEventData = std::make_shared<UKBEventData_bindAccountEmail>();
	pEventData->email = email;
	KBENGINE_EVENT_FIRE_IN(KBEngine::KBEventTypes::bindAccountEmail, pEventData);
	return true;
}
bool KBEMain::newPassword(KBString oldPassword, KBString newPassword)
{
	if (!KBEngine::KBEngineApp::getSingleton().isInitialized())
	{
		return false;
	}
	auto pEventData = std::make_shared<UKBEventData_newPassword>();
	pEventData->old_password = oldPassword;
	pEventData->new_password = newPassword;
	KBENGINE_EVENT_FIRE_IN(KBEngine::KBEventTypes::newPassword, pEventData);
	return true;
}
