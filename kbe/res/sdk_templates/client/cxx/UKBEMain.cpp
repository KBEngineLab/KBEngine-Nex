
// #include "UKBEMain.h"
// #include "KBEngine.h"
// #include "KBEngineArgs.h"
// #include "MemoryStream.h"
// #include "Bundle.h"
// #include "KBDebug.h"
// #include "Entity.h"



// UKBEMain::UKBEMain()
// {
	
// 	PrimaryComponentTick.bCanEverTick = true;
// 	bWantsInitializeComponent = true;


	// ip = TEXT("127.0.0.1");
	// port = @{KBE_LOGIN_PORT};
	// syncPlayerMS = 1000 / @{KBE_UPDATEHZ};
	// useAliasEntityID = @{KBE_USE_ALIAS_ENTITYID};
	// isOnInitCallPropertysSetMethods = true;
	// forceDisableUDP = false;
	// clientType = EKCLIENT_TYPE::CLIENT_TYPE_WIN;
	// networkEncryptType = NETWORK_ENCRYPT_TYPE::ENCRYPT_TYPE_NONE;
	// serverHeartbeatTick = @{KBE_SERVER_EXTERNAL_TIMEOUT};
	// TCP_SEND_BUFFER_MAX = TCP_PACKET_MAX;
	// TCP_RECV_BUFFER_MAX = TCP_PACKET_MAX;
	// UDP_SEND_BUFFER_MAX = 128;
	// UDP_RECV_BUFFER_MAX = 128;
	
// 	disableMainLoop = false;

// }

// void UKBEMain::InitializeComponent()
// {
// 	Super::InitializeComponent();
// }

// void UKBEMain::UninitializeComponent()
// {
// 	Super::UninitializeComponent();
// }

// // Called when the game starts
// void UKBEMain::BeginPlay()
// {
// 	Super::BeginPlay();
// 	KBEngine::KBEngineArgs* pArgs = new KBEngine::KBEngineArgs();
// 	pArgs->ip = ip;
// 	pArgs->port = port;
// 	pArgs->syncPlayerMS = syncPlayerMS;
// 	pArgs->useAliasEntityID = useAliasEntityID;
// 	pArgs->isOnInitCallPropertysSetMethods = isOnInitCallPropertysSetMethods;
// 	pArgs->forceDisableUDP = forceDisableUDP;
// 	pArgs->clientType = static_cast<EKCLIENT_TYPE>(clientType);
// 	pArgs->networkEncryptType = static_cast<NETWORK_ENCRYPT_TYPE>(networkEncryptType);
// 	pArgs->serverHeartbeatTick = serverHeartbeatTick / 2;
// 	pArgs->TCP_SEND_BUFFER_MAX = TCP_SEND_BUFFER_MAX;
// 	pArgs->TCP_RECV_BUFFER_MAX = TCP_RECV_BUFFER_MAX;
// 	pArgs->UDP_SEND_BUFFER_MAX = UDP_SEND_BUFFER_MAX;
// 	pArgs->UDP_RECV_BUFFER_MAX = UDP_RECV_BUFFER_MAX;
// 	pArgs->disableMainLoop = disableMainLoop;

// 	if(!KBEngine::KBEngineApp::getSingleton().initialize(pArgs))
// 		delete pArgs;

// 	// installEvents();
// }

// void UKBEMain::EndPlay(const EEndPlayReason::Type EndPlayReason)
// {
// 	// deregisterEvents();
// 	Super::EndPlay(EndPlayReason);
// }

// // Called every frame
// void UKBEMain::TickComponent( float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction )
// {

// }

// // void UKBEMain::installEvents()
// // {
// // 	KBENGINE_REGISTER_EVENT(KBEngine::KBEventTypes::onScriptVersionNotMatch, onScriptVersionNotMatch);
// // 	KBENGINE_REGISTER_EVENT(KBEngine::KBEventTypes::onVersionNotMatch, onVersionNotMatch);
// // }

// // void UKBEMain::deregisterEvents()
// // {
// // 	KBENGINE_DEREGISTER_EVENT(KBEngine::KBEventTypes::onScriptVersionNotMatch);
// // 	KBENGINE_DEREGISTER_EVENT(KBEngine::KBEventTypes::onVersionNotMatch);
// // }

// // void UKBEMain::onVersionNotMatch(std::shared_ptr<UKBEventData> pEventData)
// // {
// // }

// // void UKBEMain::onScriptVersionNotMatch(std::shared_ptr<UKBEventData> pEventData)
// // {
// // }


// FString UKBEMain::getClientVersion()
// {
// 	if (!KBEngine::KBEngineApp::getSingleton().isInitialized())
// 		return TEXT("");

// 	return KBEngine::KBEngineApp::getSingleton().clientVersion();
// }

// FString UKBEMain::getClientScriptVersion()
// {
// 	if (!KBEngine::KBEngineApp::getSingleton().isInitialized())
// 		return TEXT("");

// 	return KBEngine::KBEngineApp::getSingleton().clientScriptVersion();
// }

// FString UKBEMain::getServerVersion()
// {
// 	if (!KBEngine::KBEngineApp::getSingleton().isInitialized())
// 		return TEXT("");

// 	return KBEngine::KBEngineApp::getSingleton().serverVersion();
// }

// FString UKBEMain::getServerScriptVersion()
// {
// 	if (!KBEngine::KBEngineApp::getSingleton().isInitialized())
// 		return TEXT("");

// 	return KBEngine::KBEngineApp::getSingleton().serverScriptVersion();
// }

// FString UKBEMain::getComponentName()
// {
// 	if (!KBEngine::KBEngineApp::getSingleton().isInitialized())
// 		return TEXT("");

// 	return KBEngine::KBEngineApp::getSingleton().component();
// }

// bool UKBEMain::destroyKBEngine()
// {
// 	if (!KBEngine::KBEngineApp::getSingleton().isInitialized())
// 		return false;

// 	KBEngine::KBEngineApp::getSingleton().destroy();
// 	KBENGINE_EVENT_CLEAR();
// 	return true;
// }

// bool UKBEMain::login(FString username, FString password, TArray<uint8> datas)
// {
// 	if (!KBEngine::KBEngineApp::getSingleton().isInitialized())
// 	{
// 		return false;
// 	}

// 	KBEngine::KBEngineApp::getSingleton().reset();

// 	auto pEventData = std::make_shared<UKBEventData_login>();
// 	pEventData->username = username;
// 	pEventData->password = password;
// 	pEventData->datas = datas;
// 	KBENGINE_EVENT_FIRE_IN(KBEngine::KBEventTypes::login, pEventData);
// 	return true;
// }

// bool UKBEMain::createAccount(FString username, FString password, const TArray<uint8>& datas)
// {
// 	if (!KBEngine::KBEngineApp::getSingleton().isInitialized())
// 	{
// 		return false;
// 	}

// 	KBEngine::KBEngineApp::getSingleton().reset();

// 	auto pEventData = std::make_shared<UKBEventData_createAccount>();
// 	pEventData->username = username;
// 	pEventData->password = password;
// 	pEventData->datas = datas;
// 	KBENGINE_EVENT_FIRE_IN(KBEngine::KBEventTypes::createAccount, pEventData);
// 	return true;
// }

// bool UKBEMain::resetPassword(FString username)
// {
// 	if (!KBEngine::KBEngineApp::getSingleton().isInitialized())
// 	{
// 		return false;
// 	}
// 	auto pEventData = std::make_shared<UKBEventData_resetPassword>();
// 	pEventData->username = username;
// 	KBENGINE_EVENT_FIRE_IN(KBEngine::KBEventTypes::resetPassword, pEventData);
// 	return true;
// }
// bool UKBEMain::bindAccountEmail(FString email)
// {
// 	if (!KBEngine::KBEngineApp::getSingleton().isInitialized())
// 	{
// 		return false;
// 	}
// 	auto pEventData = std::make_shared<UKBEventData_bindAccountEmail>();
// 	pEventData->email = email;
// 	KBENGINE_EVENT_FIRE_IN(KBEngine::KBEventTypes::bindAccountEmail, pEventData);
// 	return true;
// }
// bool UKBEMain::newPassword(FString oldPassword, FString newPassword)
// {
// 	if (!KBEngine::KBEngineApp::getSingleton().isInitialized())
// 	{
// 		return false;
// 	}
// 	auto pEventData = std::make_shared<UKBEventData_newPassword>();
// 	pEventData->old_password = oldPassword;
// 	pEventData->new_password = newPassword;
// 	KBENGINE_EVENT_FIRE_IN(KBEngine::KBEventTypes::newPassword, pEventData);
// 	return true;
// }