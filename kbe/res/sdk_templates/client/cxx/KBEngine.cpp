
#include "KBEngine.h"

#include <regex>


#include "KBEngineArgs.h"
#include "Entity.h"
#include "EntityDef.h"
#include "Messages.h"
#include "NetworkInterfaceTCP.h"
#include "NetworkInterfaceKCP.h"
#include "Bundle.h"
#include "MemoryStream.h"
#include "DataTypes.h"
#include "ScriptModule.h"
#include "Property.h"
#include "Method.h"
#include "EntityCall.h"
#include "KBDebug.h"
#include "KBEvent.h"
#include "EncryptionFilter.h"
#include "GameThreadDispatcher.h"

#include <chrono>

namespace KBEngine
{

ServerErrorDescrs KBEngineApp::serverErrs_;

KBEngineApp::KBEngineApp() :
	pArgs_(nullptr),
	pNetworkInterface_(nullptr),
	username_(KBTEXT("")),
	password_(KBTEXT("")),
	baseappIP_(KBTEXT("")),
	baseappTcpPort_(0),
	baseappUdpPort_(0),
	currserver_(KBTEXT("")),
	currstate_(KBTEXT("")),
	serverdatas_(),
	clientdatas_(),
	encryptedKey_(),
	serverVersion_(KBTEXT("")),
	clientVersion_(KBTEXT("")),
	serverScriptVersion_(KBTEXT("")),
	clientScriptVersion_(KBTEXT("")),
	serverProtocolMD5_(KBTEXT("@{KBE_SERVER_PROTO_MD5}")),
	serverEntitydefMD5_(KBTEXT("@{KBE_SERVER_ENTITYDEF_MD5}")),
	entity_uuid_(0),
	entity_id_(0),
	entity_type_(KBTEXT("")),
	useAliasEntityID_(@{KBE_USE_ALIAS_ENTITYID}),
	controlledEntities_(),
	entityServerPos_(),
	spacedatas_(),
	entities_(),
	entityIDAliasIDList_(),
	bufferedCreateEntityMessages_(),
	lastTickTime_(0.0),
	lastTickCBTime_(0.0),
	lastUpdateToServerTime_(0.0),
	spaceID_(0),
	spaceResPath_(KBTEXT("")),
	isLoadedGeometry_(false),
	component_(KBTEXT("client")),
	pFilter_(NULL),
	mainLoopRunning_(false)
	// ,
	// pUKBETicker_(nullptr)
{
	INFO_MSG("KBEngineApp::KBEngineApp(): hello!");
	// installUKBETicker();


}

KBEngineApp::KBEngineApp(KBEngineArgs* pArgs):
	pArgs_(nullptr),
	pNetworkInterface_(nullptr),
	username_(KBTEXT("")),
	password_(KBTEXT("")),
	baseappIP_(KBTEXT("")),
	baseappTcpPort_(0),
	baseappUdpPort_(0),
	currserver_(KBTEXT("")),
	currstate_(KBTEXT("")),
	serverdatas_(),
	clientdatas_(),
	encryptedKey_(),
	serverVersion_(KBTEXT("")),
	clientVersion_(KBTEXT("")),
	serverScriptVersion_(KBTEXT("")),
	clientScriptVersion_(KBTEXT("")),
	serverProtocolMD5_(KBTEXT("@{KBE_SERVER_PROTO_MD5}")),
	serverEntitydefMD5_(KBTEXT("@{KBE_SERVER_ENTITYDEF_MD5}")),
	entity_uuid_(0),
	entity_id_(0),
	entity_type_(KBTEXT("")),
	useAliasEntityID_(@{KBE_USE_ALIAS_ENTITYID}),
	controlledEntities_(),
	entityServerPos_(),
	spacedatas_(),
	entities_(),
	entityIDAliasIDList_(),
	bufferedCreateEntityMessages_(),
	lastTickTime_(0.0),
	lastTickCBTime_(0.0),
	lastUpdateToServerTime_(0.0),
	spaceID_(0),
	spaceResPath_(KBTEXT("")),
	isLoadedGeometry_(false),
	component_(KBTEXT("client")),
	pFilter_(nullptr),
	mainLoopRunning_(false)
	// pUKBETicker_(nullptr)
{
	INFO_MSG("KBEngineApp::KBEngineApp(): hello!");
	initialize(pArgs);
}

KBEngineApp::~KBEngineApp()
{
	// destroy();
	INFO_MSG("KBEngineApp::~KBEngineApp(): destructed!");
}

KBEngineApp* pKBEngineApp = nullptr;

KBEngineApp& KBEngineApp::getSingleton() 
{
	if(!pKBEngineApp)
		pKBEngineApp = new KBEngineApp();

	return *pKBEngineApp;
}

void KBEngineApp::destroyKBEngineApp() 
{
	if(pKBEngineApp)
	{
		delete pKBEngineApp;
		pKBEngineApp = nullptr;
		KBEvent::clear();
	}
}

bool KBEngineApp::initialize(KBEngineArgs* pArgs)
{
	if (isInitialized())
		return false;

	EntityDef::initialize();

	// 注册事件
	installEvents();

	pArgs_ = pArgs;

	installUKBETicker();

	reset();
	return true;
}

void KBEngineApp::installEvents()
{
	KBENGINE_REGISTER_EVENT_IN_OVERRIDE_FUNC(KBEventTypes::login, KBEventTypes::login, [this](std::shared_ptr<UKBEventData> pEventData)
	{
		const UKBEventData_login& data = static_cast<const UKBEventData_login&>(*pEventData);
		login(data.username, data.password, data.datas);
	});

	KBENGINE_REGISTER_EVENT_IN_OVERRIDE_FUNC(KBEventTypes::logout, KBEventTypes::logout, [this](std::shared_ptr<UKBEventData> pEventData)
	{
		logout();
	});

	KBENGINE_REGISTER_EVENT_IN_OVERRIDE_FUNC(KBEventTypes::createAccount, KBEventTypes::createAccount, [this](std::shared_ptr<UKBEventData> pEventData)
	{
		const UKBEventData_createAccount& data = static_cast<const UKBEventData_createAccount&>(*pEventData);
		createAccount(data.username, data.password, data.datas);
	});

	KBENGINE_REGISTER_EVENT_IN_OVERRIDE_FUNC(KBEventTypes::reloginBaseapp, KBEventTypes::reloginBaseapp, [this](std::shared_ptr<UKBEventData> pEventData)
	{
		reloginBaseapp();
	});

	KBENGINE_REGISTER_EVENT_IN_OVERRIDE_FUNC(KBEventTypes::resetPassword, KBEventTypes::resetPassword, [this](std::shared_ptr<UKBEventData> pEventData)
	{
		const UKBEventData_resetPassword& data = static_cast<const UKBEventData_resetPassword&>(*pEventData);
		resetPassword(data.username);
	});

	KBENGINE_REGISTER_EVENT_IN_OVERRIDE_FUNC(KBEventTypes::bindAccountEmail, KBEventTypes::bindAccountEmail, [this](std::shared_ptr<UKBEventData> pEventData)
	{
		const UKBEventData_bindAccountEmail& data = static_cast<const UKBEventData_bindAccountEmail&>(*pEventData);
		bindAccountEmail(data.email);
	});

	KBENGINE_REGISTER_EVENT_IN_OVERRIDE_FUNC(KBEventTypes::newPassword, KBEventTypes::newPassword, [this](std::shared_ptr<UKBEventData> pEventData)
	{
		const UKBEventData_newPassword& data = static_cast<const UKBEventData_newPassword&>(*pEventData);
		newPassword(data.old_password, data.new_password);
	});

	// 内部事件
	KBENGINE_REGISTER_EVENT_IN_OVERRIDE_FUNC("_closeNetwork", "_closeNetwork", [this](std::shared_ptr<UKBEventData> pEventData)
	{
		_closeNetwork();
	});
}

void KBEngineApp::destroy()
{
	if (currserver_ == KBTEXT("baseapp"))
	{
		logout();
	}
	reset();
	KBENGINE_DEREGISTER_ALL_EVENT();
	resetMessages();

	KBE_SAFE_RELEASE(pArgs_);
	
	if (pNetworkInterface_) pNetworkInterface_->destroy();
	// KBE_SAFE_RELEASE(pNetworkInterface_);
	Bundle::clearAllBundles();
	pNetworkInterface_ = nullptr;
	KBE_SAFE_RELEASE(pFilter_);
	uninstallUKBETicker();

	destroyKBEngineApp();
}

void KBEngineApp::resetMessages()
{
	// serverErrs_.Clear();

	Messages::clear();
	EntityDef::clear();
	Entity::clear();

	INFO_MSG("KBEngineApp::resetMessages(): done!");
}

void KBEngineApp::reset()
{
	KBEvent::clearFiredEvents();

	clearEntities(true);

	currserver_ = KBTEXT("");
	currstate_ = KBTEXT("");

	serverdatas_.Clear();

	serverVersion_ = KBTEXT("");
	clientVersion_ = KBTEXT("@{KBE_VERSION}");
	serverScriptVersion_ = KBTEXT("");
	clientScriptVersion_ = KBTEXT("@{KBE_SCRIPT_VERSION}");

	entity_uuid_ = 0;
	entity_id_ = 0;
	entity_type_ = KBTEXT("");

	entityIDAliasIDList_.Clear();
	bufferedCreateEntityMessages_.Clear();

	lastTickTime_ = getTimeSeconds();
	lastTickCBTime_ = getTimeSeconds();
	lastUpdateToServerTime_ = getTimeSeconds();

	spacedatas_.Clear();

	spaceID_ = 0;
	spaceResPath_ = KBTEXT("");
	isLoadedGeometry_ = false;
	
	baseappUdpPort_ = 0;

	initNetwork();
}

void KBEngineApp::installUKBETicker()
{
	if (mainLoopRunning_.load())
	{
		return;
	}

	if (pArgs_ != nullptr && pArgs_->disableMainLoop)
	{
		return;
	}

	mainLoopRunning_ = true;
	mainLoopThread_ = std::thread([this]()
	{
		// 用标准线程周期驱动，保持 15ms tick，避免引入额外事件循环依赖。
		while (mainLoopRunning_.load())
		{
			this->process();
			std::this_thread::sleep_for(std::chrono::milliseconds(15));
		}
	});
}

void KBEngineApp::uninstallUKBETicker()
{
	mainLoopRunning_ = false;

	if (mainLoopThread_.joinable())
	{
		mainLoopThread_.join();
	}
}

bool KBEngineApp::initNetwork()
{
	KBE_SAFE_RELEASE(pFilter_);

	if (pNetworkInterface_)
		// delete pNetworkInterface_;
		pNetworkInterface_ = nullptr;

	Messages::initialize();

	if (pArgs_)
	{
		if (pArgs_->forceDisableUDP || baseappUdpPort_ == 0) {
			// pNetworkInterface_ = new NetworkInterfaceTCP();
			pNetworkInterface_ = std::make_shared<NetworkInterfaceTCP>();
		}
		else{
			// pNetworkInterface_ = new NetworkInterfaceKCP();
			pNetworkInterface_ = std::make_shared<NetworkInterfaceKCP>();
		}
	}

	return true;
}

void KBEngineApp::_closeNetwork()
{
	if (pNetworkInterface_)
		pNetworkInterface_->close();
}

// bool KBEngineApp::validEmail(const KBString& strEmail)
// {
// 	const FRegexPattern spattern(KBString(KBTEXT("[a-z0-9._%+-]+@[a-z0-9.-]+\\.[a-z]{2,4}")));
//
// 	FRegexMatcher fMatcher(spattern, strEmail);
//
// 	if (fMatcher.FindNext()) {
// 		return true;
// 	}
//
// 	return false;
// }



bool KBEngineApp::validEmail(const KBString& strEmail)
{
	// 正则：兼容你原来的模式
	static const std::regex pattern(
		"[a-z0-9._%+-]+@[a-z0-9.-]+\\.[a-z]{2,4}",
		std::regex_constants::icase
	);

	// KBString → std::string
	const std::string email = static_cast<std::string>(strEmail);
	return std::regex_match(email, pattern);
}

void KBEngineApp::process()
{
	// 先处理 socket 收到的原始数据，MessageReader 会把解析出的消息投递到 GameThreadDispatcher。
	if (pNetworkInterface_)
		pNetworkInterface_->process();

	GameThreadDispatcher::Instance().Pump();

	KBEvent::processInEvents();
	
	KBEvent::processOutEvents();

	// 向服务端发送心跳以及同步角色信息到服务端
	sendTick();
}

void KBEngineApp::sendTick()
{

	if (!pNetworkInterface_ || !pNetworkInterface_->valid())
		return;


	double span = getTimeSeconds() - lastTickTime_;

	// 更新玩家的位置与朝向到服务端
	updatePlayerToServer();


	if (pArgs_->serverHeartbeatTick > 0 && span > pArgs_->serverHeartbeatTick)
	{

		span = lastTickCBTime_ - lastTickTime_;

		// 如果心跳回调接收时间小于心跳发送时间，说明没有收到回调
		// 此时应该通知客户端掉线了
		if (span < 0)
		{
			SCREEN_ERROR_MSG("KBEngineApp::sendTick(): Receive appTick timeout!");
			pNetworkInterface_->close();
			return;
		}

		Message** Loginapp_onClientActiveTickMsgFind = Messages::messages.Find("Loginapp_onClientActiveTick");
		Message** Baseapp_onClientActiveTickMsgFind = Messages::messages.Find("Baseapp_onClientActiveTick");

		if (currserver_ == KBTEXT("loginapp"))
		{
			if (Loginapp_onClientActiveTickMsgFind)
			{
				Bundle* pBundle = Bundle::createObject();
				pBundle->newMessage(*Loginapp_onClientActiveTickMsgFind);
				pBundle->send(pNetworkInterface_);
			}
		}
		else
		{
			if (Baseapp_onClientActiveTickMsgFind)
			{
				Bundle* pBundle = Bundle::createObject();
				pBundle->newMessage(*Baseapp_onClientActiveTickMsgFind);
				pBundle->send(pNetworkInterface_);
			}
		}

		lastTickTime_ = getTimeSeconds();
	}
}

Entity* KBEngineApp::player()
{
	return findEntity(entity_id_);
}

Entity* KBEngineApp::findEntity(int32 entityID)
{
	Entity** pEntity = entities_.Find(entityID);
	if (pEntity == nullptr)
		return nullptr;

	return *pEntity;
}

KBString KBEngineApp::serverErr(uint16 id)
{
	return serverErrs_.ServerErrStr(id);
}

void KBEngineApp::updatePlayerToServer()
{
	if (pArgs_->syncPlayerMS <= 0 || spaceID_ == 0)
		return;

	double tnow = getTimeSeconds();
	double span = tnow - lastUpdateToServerTime_;

	if (span < ((double)pArgs_->syncPlayerMS / 1000.0))
		return;

	Entity* pPlayerEntity = player();
	if (!pPlayerEntity || !pPlayerEntity->inWorld()  || pPlayerEntity->isControlled())
		return;

	lastUpdateToServerTime_ = tnow;
	const KBVector3f& position = pPlayerEntity->position;
	const KBVector3f& direction = pPlayerEntity->direction;

	bool posHasChanged = (pPlayerEntity->entityLastLocalPos - position).length() > 0.001f;
	bool dirHasChanged = (pPlayerEntity->entityLastLocalDir - direction).length() > 0.001f;

	if (posHasChanged || dirHasChanged)
	{
		pPlayerEntity->entityLastLocalPos = position;
		pPlayerEntity->entityLastLocalDir = direction;

		Bundle* pBundle = Bundle::createObject();
		pBundle->newMessage(Messages::messages[KBTEXT("Baseapp_onUpdateDataFromClient"]));
		(*pBundle) << position.x;
		(*pBundle) << position.y;
		(*pBundle) << position.z;

		(*pBundle) << direction.x;
		(*pBundle) << direction.y;
		(*pBundle) << direction.z;

		(*pBundle) << (uint8)pPlayerEntity->isOnGround();
		(*pBundle) << spaceID_;

		pBundle->send(pNetworkInterface_);
	}

	// 开始同步所有被控制了的entity的位置
	for(auto& item : controlledEntities_)
	{
		Entity* pEntity = item;
		const KBVector3f& e_position = pEntity->position;
		const KBVector3f& e_direction = pEntity->direction;

		posHasChanged = (pEntity->entityLastLocalPos - e_position).length() > 0.001f;
		dirHasChanged = (pEntity->entityLastLocalDir - e_direction).length() > 0.001f;

		if (posHasChanged || dirHasChanged)
		{
			pEntity->entityLastLocalPos = e_position;
			pEntity->entityLastLocalDir = e_direction;

			Bundle* pBundle = Bundle::createObject();
			pBundle->newMessage(Messages::messages[KBTEXT("Baseapp_onUpdateDataFromClientForControlledEntity"]));
			(*pBundle) << pEntity->id();
			(*pBundle) << e_position.x;
			(*pBundle) << e_position.y;
			(*pBundle) << e_position.z;

			(*pBundle) << e_direction.x;
			(*pBundle) << e_direction.y;
			(*pBundle) << e_direction.z;

			(*pBundle) << (uint8)pEntity->isOnGround();
			(*pBundle) << spaceID_;

			pBundle->send(pNetworkInterface_);
		}
	}
}

void KBEngineApp::Client_onAppActiveTickCB()
{
	lastTickCBTime_ = getTimeSeconds();
}

void KBEngineApp::hello()
{
	Bundle* pBundle = Bundle::createObject();
	if (currserver_ == KBTEXT("loginapp"))
		pBundle->newMessage(Messages::messages[KBTEXT("Loginapp_hello")]);
	else
		pBundle->newMessage(Messages::messages[KBTEXT("Baseapp_hello")]);

	KBE_SAFE_RELEASE(pFilter_);

	if (pArgs_->networkEncryptType == NETWORK_ENCRYPT_TYPE::ENCRYPT_TYPE_BLOWFISH)
	{
		pFilter_ = new BlowfishFilter();
		encryptedKey_ = ((BlowfishFilter*)pFilter_)->key();
		pNetworkInterface_->setFilter(nullptr);
	}

	(*pBundle) << clientVersion_;
	(*pBundle) << clientScriptVersion_;
	pBundle->appendBlob(encryptedKey_);
	pBundle->send(pNetworkInterface_);
}

void KBEngineApp::Client_onHelloCB(MemoryStream& stream)
{
	KBString str_serverVersion;
	stream >> str_serverVersion;
	stream >> serverScriptVersion_;

	KBString serverProtocolMD5;
	stream >> serverProtocolMD5;

	KBString serverEntitydefMD5;
	stream >> serverEntitydefMD5;

	int32 ctype;
	stream >> ctype;

	INFO_MSG("KBEngineApp::Client_onHelloCB(): verInfo(%s), scriptVersion(%s), srvProtocolMD5(%s), srvEntitydefMD5(%s), ctype(%d)!", 
		str_serverVersion.c_str(), serverScriptVersion_.c_str(), serverProtocolMD5_.c_str(), serverEntitydefMD5_.c_str(), ctype);

	if(str_serverVersion != "Getting")
	{
		serverVersion_ = str_serverVersion;

		/*
		if(serverProtocolMD5_ != serverProtocolMD5)
		{
			ERROR_MSG("KBEngineApp::Client_onHelloCB():  digest not match! serverProtocolMD5=%s(server: %s)", *serverProtocolMD5_, *serverProtocolMD5);

			UKBEventData_onVersionNotMatch* pEventData = NewObject<UKBEventData_onVersionNotMatch>();
			pEventData->clientVersion = clientVersion_;
			pEventData->serverVersion = serverVersion_;
			KBENGINE_EVENT_FIRE_ALL(KBEventTypes::onVersionNotMatch, pEventData);
			return;
		}
		*/

		if(serverEntitydefMD5_ != serverEntitydefMD5)
		{
			ERROR_MSG("KBEngineApp::Client_onHelloCB():  digest not match! serverEntitydefMD5=%s(server: %s)", serverEntitydefMD5_.c_str(), serverEntitydefMD5.c_str());

			// UKBEventData_onVersionNotMatch* pEventData = NewObject<UKBEventData_onVersionNotMatch>();
			auto pEventData = std::make_shared<UKBEventData_onVersionNotMatch>();
			pEventData->clientVersion = clientVersion_;
			pEventData->serverVersion = serverVersion_;

			// auto pEventData = std::make_shared<UKBEventData_onVersionNotMatch>();
			// pEventData->clientVersion = clientVersion_;
			// pEventData->serverVersion = serverVersion_;


			KBENGINE_EVENT_FIRE_ALL(KBEventTypes::onVersionNotMatch, pEventData);
			return;
		}
	}

	if (pArgs_->networkEncryptType == NETWORK_ENCRYPT_TYPE::ENCRYPT_TYPE_BLOWFISH)
	{
		pNetworkInterface_->setFilter(pFilter_);
		pFilter_ = nullptr;
	}

	onServerDigest();

	if (currserver_ == KBTEXT("baseapp"))
	{
		onLogin_baseapp();
	}
	else
	{
		onLogin_loginapp();
	}
}

void KBEngineApp::Client_onVersionNotMatch(MemoryStream& stream)
{
	stream >> serverVersion_;

	ERROR_MSG("KBEngineApp::Client_onVersionNotMatch(): verInfo=%s(server: %s)", clientVersion_.c_str(), serverVersion_.c_str());

	// UKBEventData_onVersionNotMatch* pEventData = NewObject<UKBEventData_onVersionNotMatch>();
	auto pEventData = std::make_shared<UKBEventData_onVersionNotMatch>();
	pEventData->clientVersion = clientVersion_;
	pEventData->serverVersion = serverVersion_;
	KBENGINE_EVENT_FIRE_ALL(KBEventTypes::onVersionNotMatch, pEventData);
}

void KBEngineApp::Client_onScriptVersionNotMatch(MemoryStream& stream)
{
	stream >> serverScriptVersion_;

	ERROR_MSG("KBEngineApp::Client_onScriptVersionNotMatch(): verInfo=%s(server: %s)", clientScriptVersion_.c_str(), serverScriptVersion_.c_str());

	// UKBEventData_onScriptVersionNotMatch* pEventData = NewObject<UKBEventData_onScriptVersionNotMatch>();
	auto pEventData = std::make_shared<UKBEventData_onScriptVersionNotMatch>();
	pEventData->clientScriptVersion = clientScriptVersion_;
	pEventData->serverScriptVersion = serverScriptVersion_;
	KBENGINE_EVENT_FIRE_ALL(KBEventTypes::onScriptVersionNotMatch, pEventData);
}

void KBEngineApp::Client_onImportClientSDK(MemoryStream& stream)
{
	// UKBEventData_onImportClientSDK* pEventData = NewObject<UKBEventData_onImportClientSDK>();
	auto pEventData = std::make_shared<UKBEventData_onImportClientSDK>();

	pEventData->remainingFiles = stream.readInt32();
	pEventData->fileName = stream.readString();
	pEventData->fileSize = stream.readInt32();
	stream.readBlob(pEventData->fileDatas);

	KBENGINE_EVENT_FIRE_ALL("onImportClientSDK", pEventData);
}

void KBEngineApp::Client_onKicked(uint16 failedcode)
{
	DEBUG_MSG("KBEngineApp::Client_onKicked(): failedcode=%d, %s", failedcode, serverErr(failedcode).c_str());

	// UKBEventData_onKicked* pEventData = NewObject<UKBEventData_onKicked>();

	auto pEventData = std::make_shared<UKBEventData_onKicked>();
	pEventData->failedcode = failedcode;
	pEventData->errorStr = serverErr(failedcode);
	KBENGINE_EVENT_FIRE_ALL(KBEventTypes::onKicked, pEventData);
}

void KBEngineApp::onServerDigest()
{
}

void KBEngineApp::onConnectCallback(KBString ip, uint16 port, bool success, int userdata)
{
	if (userdata == 0)
	{
		onConnectTo_loginapp_login_callback(ip, port, success);
	}
	else if (userdata == 1)
	{
		onConnectTo_loginapp_create_callback(ip, port, success);
	}
	else if (userdata == 2)
	{
		onConnectTo_baseapp_callback(ip, port, success);
	}
	else if (userdata == 3)
	{
		onReloginTo_baseapp_callback(ip, port, success);
	}
	else if (userdata == 4)
	{
		onConnectTo_resetpassword_callback(ip, port, success);
	}
	else if (userdata == 5)
	{
		//onConnectTo_resetpassword_callback(ip, port, success);
	}
	else
	{
		// check(false);
		KBE_ASSERT(false);
	}
}

bool KBEngineApp::login(const KBString& username, const KBString& password, const KBArray<uint8>& datas)
{
	if (username.length() == 0)
	{
		ERROR_MSG("KBEngineApp::login(): username is empty!");
		return false;
	}

	if (password.length() == 0)
	{
		ERROR_MSG("KBEngineApp::login(): password is empty!");
		return false;
	}

	username_ = username;
	password_ = password;
	clientdatas_ = datas;

	login_loginapp(true);
	return true;
}

bool KBEngineApp::logout()
{
	if (currserver_ != KBTEXT("baseapp"))
		return false;

	INFO_MSG("KBEngineApp::logout()");
	Bundle* pBundle = Bundle::createObject();
	pBundle->newMessage(Messages::messages[KBTEXT("Baseapp_logoutBaseapp"]));
	(*pBundle) << entity_uuid_;
	(*pBundle) << entity_id_;
	pBundle->send(pNetworkInterface_);
	return true;
}

void KBEngineApp::login_loginapp(bool noconnect)
{

	// printf("%s", std::string(pArgs_->ip).c_str());

	if (noconnect)
	{
		reset();
		pNetworkInterface_->connectTo(pArgs_->ip, pArgs_->port, this, 0);
	}
	else
	{
		INFO_MSG("KBEngineApp::login_loginapp(): send login! username=%s", username_.c_str());
		Bundle* pBundle = Bundle::createObject();
		pBundle->newMessage(Messages::messages[KBTEXT("Loginapp_login"]));
		(*pBundle) << (uint8)pArgs_->clientType;
		pBundle->appendBlob(clientdatas_);
		(*pBundle) << username_;
		(*pBundle) << password_;
		pBundle->send(pNetworkInterface_);
	}
}

void KBEngineApp::onConnectTo_loginapp_login_callback(KBString ip, uint16 port, bool success)
{
	if (!success)
	{
		ERROR_MSG("KBEngineApp::onConnectTo_loginapp_login_callback(): connect %s:%d is error!", ip.c_str(), port);
		return;
	}

	currserver_ = KBTEXT("loginapp");
	currstate_ = KBTEXT("login");

	INFO_MSG("KBEngineApp::onConnectTo_loginapp_login_callback(): connect %s:%d is success!", ip.c_str(), port);

	hello();
}

void KBEngineApp::onLogin_loginapp()
{
	lastTickCBTime_ = getTimeSeconds();
	login_loginapp(false);
}

void KBEngineApp::Client_onLoginFailed(MemoryStream& stream)
{
	uint16 failedcode = 0;
	stream >> failedcode;
	stream.readBlob(serverdatas_);
	ERROR_MSG("KBEngineApp::Client_onLoginFailed(): failedcode(%d:%s), datas(%zu)!", failedcode, serverErr(failedcode).c_str(), serverdatas_.Num());

	// UKBEventData_onLoginFailed* pEventData = NewObject<UKBEventData_onLoginFailed>();
	auto pEventData = std::make_shared<UKBEventData_onLoginFailed>();
	pEventData->failedcode = failedcode;
	pEventData->errorStr = serverErr(failedcode);
	pEventData->serverdatas = serverdatas_;
	KBENGINE_EVENT_FIRE_ALL(KBEventTypes::onLoginFailed, pEventData);
}

void KBEngineApp::Client_onLoginSuccessfully(MemoryStream& stream)
{
	KBString accountName;
	stream >> accountName;
	username_ = accountName;
	stream >> baseappIP_;
	stream >> baseappTcpPort_;
	stream >> baseappUdpPort_;
	stream.readBlob(serverdatas_);

	DEBUG_MSG("KBEngineApp::Client_onLoginSuccessfully(): accountName(%s), addr("
		 "%s:%d:%d), datas(%zu)!", accountName.c_str(), baseappIP_.c_str(), baseappTcpPort_, baseappUdpPort_, serverdatas_.Num());
	
	login_baseapp(true);
}

void KBEngineApp::login_baseapp(bool noconnect)
{
	if (noconnect)
	{
		auto pEventData = std::make_shared<UKBEventData_onLoginBaseapp>();
		// KBENGINE_EVENT_FIRE_ALL(KBEventTypes::onLoginBaseapp, NewObject<UKBEventData_onLoginBaseapp>());
		KBENGINE_EVENT_FIRE_ALL(KBEventTypes::onLoginBaseapp, pEventData);


		if (pNetworkInterface_)
		{
			pNetworkInterface_->destroy();
			pNetworkInterface_ = nullptr;

			initNetwork();
			pNetworkInterface_->connectTo(baseappIP_, (!pArgs_->forceDisableUDP && baseappUdpPort_ > 0) ? baseappUdpPort_ : baseappTcpPort_, this, 2);
		}
	}
	else
	{
		Bundle* pBundle = Bundle::createObject();
		pBundle->newMessage(Messages::messages[KBTEXT("Baseapp_loginBaseapp"]));
		(*pBundle) << username_;
		(*pBundle) << password_;
		pBundle->send(pNetworkInterface_);
	}
}

void KBEngineApp::onConnectTo_baseapp_callback(KBString ip, uint16 port, bool success)
{
	lastTickCBTime_ = getTimeSeconds();

	if (!success)
	{
		ERROR_MSG("KBEngineApp::onConnectTo_baseapp_callback(): connect %s:%d is error!", ip.c_str(), port);
		return;
	}

	currserver_ = KBTEXT("baseapp");
	currstate_ = KBTEXT("");

	DEBUG_MSG("KBEngineApp::onConnectTo_baseapp_callback(): connect %s:%d is successfully!", ip.c_str(), port);

	hello();
}

void KBEngineApp::onLogin_baseapp()
{
	lastTickCBTime_ = getTimeSeconds();
	login_baseapp(false);
}

void KBEngineApp::reloginBaseapp()
{
	lastTickTime_ = getTimeSeconds();
	lastTickCBTime_ = getTimeSeconds();

	if(pNetworkInterface_->valid())
		return;

	auto pEventData = std::make_shared<UKBEventData_onReloginBaseapp>();
	// UKBEventData_onReloginBaseapp* pEventData = NewObject<UKBEventData_onReloginBaseapp>();
	KBENGINE_EVENT_FIRE_ALL(KBEventTypes::onReloginBaseapp, pEventData);

	pNetworkInterface_->connectTo(baseappIP_, (!pArgs_->forceDisableUDP && baseappUdpPort_ > 0) ? baseappUdpPort_ : baseappTcpPort_, this, 3);
}

void KBEngineApp::onReloginTo_baseapp_callback(KBString ip, uint16 port, bool success)
{
	if (!success)
	{
		ERROR_MSG("KBEngineApp::onReloginTo_baseapp_callback(): connect %s:%d is error!", ip.c_str(), port);
		return;
	}

	INFO_MSG("KBEngineApp::onReloginTo_baseapp_callback(): connect %s:%d is success!", ip.c_str(), port);

	Bundle* pBundle = Bundle::createObject();
	pBundle->newMessage(Messages::messages[KBTEXT("Baseapp_reloginBaseapp"]));
	(*pBundle) << username_;
	(*pBundle) << password_;
	(*pBundle) << entity_uuid_;
	(*pBundle) << entity_id_;
	pBundle->send(pNetworkInterface_);

	lastTickCBTime_ = getTimeSeconds();
}

void KBEngineApp::Client_onLoginBaseappFailed(uint16 failedcode)
{
	ERROR_MSG("KBEngineApp::Client_onLoginBaseappFailed(): failedcode(%d:%s)!", failedcode, *serverErr(failedcode));

	// UKBEventData_onLoginBaseappFailed* pEventData = NewObject<UKBEventData_onLoginBaseappFailed>();
	auto pEventData = std::make_shared<UKBEventData_onLoginBaseappFailed>();
	pEventData->failedcode = failedcode;
	pEventData->errorStr = serverErr(failedcode);
	KBENGINE_EVENT_FIRE_ALL(KBEventTypes::onLoginBaseappFailed, pEventData);
}

void KBEngineApp::Client_onReloginBaseappFailed(uint16 failedcode)
{
	ERROR_MSG("KBEngineApp::Client_onReloginBaseappFailed(): failedcode(%d:%s)!", failedcode, serverErr(failedcode).c_str());

	// UKBEventData_onReloginBaseappFailed* pEventData = NewObject<UKBEventData_onReloginBaseappFailed>();
	auto pEventData = std::make_shared<UKBEventData_onReloginBaseappFailed>();
	pEventData->failedcode = failedcode;
	pEventData->errorStr = serverErr(failedcode);
	KBENGINE_EVENT_FIRE_ALL(KBEventTypes::onReloginBaseappFailed, pEventData);
}

void KBEngineApp::Client_onReloginBaseappSuccessfully(MemoryStream& stream)
{
	stream >> entity_uuid_;
	ERROR_MSG("KBEngineApp::Client_onReloginBaseappSuccessfully(): name(%s)!", username_.c_str());
	// UKBEventData_onReloginBaseappSuccessfully* pEventData = NewObject<UKBEventData_onReloginBaseappSuccessfully>();
	auto pEventData = std::make_shared<UKBEventData_onReloginBaseappSuccessfully>();
	KBENGINE_EVENT_FIRE_ALL(KBEventTypes::onReloginBaseappSuccessfully, pEventData);
}

void KBEngineApp::Client_onCreatedProxies(uint64 rndUUID, int32 eid, KBString& entityType)
{
	DEBUG_MSG("KBEngineApp::Client_onCreatedProxies(): eid(%d), entityType(%s)!", eid, entityType.c_str());

	entity_uuid_ = rndUUID;
	entity_id_ = eid;
	entity_type_ = entityType;

	if (!entities_.Contains(eid))
	{
		ScriptModule** pModuleFind = EntityDef::moduledefs.Find(entityType);
		if (!pModuleFind)
		{
			SCREEN_ERROR_MSG("KBEngineApp::Client_onCreatedProxies(): not found ScriptModule(%s)!", entityType.c_str());
			return;
		}

		ScriptModule* pModule = *pModuleFind;

		Entity* pEntity = pModule->createEntity();
		if (!pEntity)
		{
			SCREEN_ERROR_MSG("KBEngineApp::Client_onCreatedProxies(): createEntity() failed! entityType(%s)", entityType.c_str());
			return;
		}
		pEntity->id(eid);
		pEntity->className(entityType);
		pEntity->onGetBase();

		try
		{
			entities_.Add(eid, pEntity);

			MemoryStream** entityMessageFind = bufferedCreateEntityMessages_.Find(eid);
			if (entityMessageFind)
			{
				MemoryStream* entityMessage = *entityMessageFind;
				Client_onUpdatePropertys(*entityMessage);
				bufferedCreateEntityMessages_.Remove(eid);
				MemoryStream::reclaimObject(entityMessage);
			}

			pEntity->__init__();
			pEntity->attachComponents();
			pEntity->inited(true);

			if (pArgs_->isOnInitCallPropertysSetMethods)
				pEntity->callPropertysSetMethods();
		}
		catch (...)
		{
			SCREEN_ERROR_MSG("KBEngineApp::Client_onCreatedProxies(): init entity failed! eid(%d), entityType(%s)", eid, entityType.c_str());
			pEntity->destroy();
		}
	}
	else
	{
		MemoryStream** entityMessageFind = bufferedCreateEntityMessages_.Find(eid);
		if (entityMessageFind)
		{
			MemoryStream* entityMessage = *entityMessageFind;
			Client_onUpdatePropertys(*entityMessage);
			bufferedCreateEntityMessages_.Remove(eid);
			MemoryStream::reclaimObject(entityMessage);
		}
	}
}

ENTITY_ID KBEngineApp::getViewEntityIDFromStream(MemoryStream& stream)
{
	ENTITY_ID id = 0;

	if (!pArgs_->useAliasEntityID)
	{
		stream >> id;
		return id;
	}

	if (entityIDAliasIDList_.Num()> 255)
	{
		stream >> id;
	}
	else
	{
		uint8 aliasID = 0;
		stream >> aliasID;

		// 如果为0且客户端上一步是重登陆或者重连操作并且服务端entity在断线期间一直处于在线状态
		// 则可以忽略这个错误, 因为cellapp可能一直在向baseapp发送同步消息， 当客户端重连上时未等
		// 服务端初始化步骤开始则收到同步信息, 此时这里就会出错。
		if (entityIDAliasIDList_.Num() <= aliasID)
			return 0;

		id = entityIDAliasIDList_[aliasID];
	}

	return id;
}

void KBEngineApp::Client_onUpdatePropertysOptimized(MemoryStream& stream)
{
	ENTITY_ID eid = getViewEntityIDFromStream(stream);
	onUpdatePropertys_(eid, stream);
}

void KBEngineApp::Client_onUpdatePropertys(MemoryStream& stream)
{
	ENTITY_ID eid;
	stream >> eid;
	onUpdatePropertys_(eid, stream);
}

void KBEngineApp::onUpdatePropertys_(ENTITY_ID eid, MemoryStream& stream)
{
	Entity** pEntityFind = entities_.Find(eid);

	if (!pEntityFind)
	{
		MemoryStream** entityMessageFind = bufferedCreateEntityMessages_.Find(eid);
		if (entityMessageFind)
		{
			ERROR_MSG("KBEngineApp::onUpdatePropertys_(): entity(%d) not found!", eid);
			return;
		}

		MemoryStream* stream1 = MemoryStream::createObject();
		stream1->append(stream);
		stream1->rpos(stream.rpos() - 4);
		bufferedCreateEntityMessages_.Add(eid, stream1);
		return;
	}
	
	Entity* pEntity = *pEntityFind;

	pEntity->onUpdatePropertys(stream);
}

void KBEngineApp::Client_onEntityDestroyed(int32 eid)
{
	DEBUG_MSG("KBEngineApp::Client_onEntityDestroyed(): entity(%d)!", eid);

	Entity** pEntityFind = entities_.Find(eid);

	if (!pEntityFind)
	{
		ERROR_MSG("KBEngineApp::Client_onEntityDestroyed(): entity(%d) not found!", eid);
		return;
	}

	Entity* pEntity = (*pEntityFind);

	if (pEntity->inWorld())
	{
		if (entity_id_ == eid)
			clearSpace(false);

		(*pEntityFind)->leaveWorld();
	}

	if (controlledEntities_.Contains(pEntity))
	{
		controlledEntities_.Remove(pEntity);

		// UKBEventData_onLoseControlledEntity* pEventData = NewObject<UKBEventData_onLoseControlledEntity>();
	auto pEventData = std::make_shared<UKBEventData_onLoseControlledEntity>();
		pEventData->entityID = pEntity->id();
		KBENGINE_EVENT_FIRE_OUT(KBEventTypes::onLoseControlledEntity, pEventData);
	}

	entities_.Remove(eid);
	pEntity->destroy();
}

void KBEngineApp::clearSpace(bool isall)
{
	entityIDAliasIDList_.Clear();
	spacedatas_.Clear();
	clearEntities(isall);
	isLoadedGeometry_ = false;
	spaceID_ = 0;
}

void KBEngineApp::clearEntities(bool isall)
{
	controlledEntities_.Clear();

	if (!isall)
	{
		Entity* pEntity = player();

		for(auto& item : entities_)
		{
			if (item.first == pEntity->id())
				continue;

			if (item.second->inWorld())
				item.second->leaveWorld();

			item.second->destroy();
		}

		entities_.Clear();
		entities_.Add(pEntity->id(), pEntity);
	}
	else
	{
		for (auto& item : entities_)
		{
			if (item.second->inWorld())
				item.second->leaveWorld();

			item.second->destroy();
		}

		entities_.Clear();
	}
}

void KBEngineApp::Client_initSpaceData(MemoryStream& stream)
{
	clearSpace(false);
	stream >> spaceID_;

	while (stream.length() > 0)
	{
		KBString key;
		KBString val;

		stream >> key >> val;
		Client_setSpaceData(spaceID_, key, val);
	}

	DEBUG_MSG("KBEngineApp::Client_initSpaceData(): spaceID(%d), size(%zu)!", spaceID_, spacedatas_.Num());
}

void KBEngineApp::Client_setSpaceData(uint32 spaceID, const KBString& key, const KBString& value)
{
	DEBUG_MSG("KBEngineApp::Client_setSpaceData(): spaceID(%d), key(%s), value(%s)!", spaceID_, *key, *value);
	spacedatas_.Add(key, value);

	if (key == KBTEXT("_mapping"))
		addSpaceGeometryMapping(spaceID, value);

	// UKBEventData_onSetSpaceData* pEventData = NewObject<UKBEventData_onSetSpaceData>();
	auto pEventData = std::make_shared<UKBEventData_onSetSpaceData>();
	pEventData->spaceID = spaceID_;
	pEventData->key = key;
	pEventData->value = value;
	KBENGINE_EVENT_FIRE_OUT(KBEventTypes::onSetSpaceData, pEventData);
}

void KBEngineApp::Client_delSpaceData(uint32 spaceID, const KBString& key)
{
	DEBUG_MSG("KBEngineApp::Client_delSpaceData(): spaceID(%d), key(%s)!", spaceID_, *key);

	spacedatas_.Remove(key);

	// UKBEventData_onDelSpaceData* pEventData = NewObject<UKBEventData_onDelSpaceData>();
	auto pEventData = std::make_shared<UKBEventData_onDelSpaceData>();
	pEventData->spaceID = spaceID_;
	pEventData->key = key;
	KBENGINE_EVENT_FIRE_OUT(KBEventTypes::onDelSpaceData, pEventData);
}

void KBEngineApp::addSpaceGeometryMapping(uint32 uspaceID, const KBString& respath)
{
	DEBUG_MSG("KBEngineApp::addSpaceGeometryMapping(): spaceID(%d), respath(%s)!", spaceID_, respath.c_str());

	isLoadedGeometry_ = true;
	spaceID_ = uspaceID;
	spaceResPath_ = respath;

	// UKBEventData_addSpaceGeometryMapping* pEventData = NewObject<UKBEventData_addSpaceGeometryMapping>();
	auto pEventData = std::make_shared<UKBEventData_addSpaceGeometryMapping>();
	pEventData->spaceResPath = spaceResPath_;
	KBENGINE_EVENT_FIRE_OUT(KBEventTypes::addSpaceGeometryMapping, pEventData);
}

KBString KBEngineApp::getSpaceData(const KBString& key)
{
	KBString* valFind = spacedatas_.Find(key);

	if(!valFind)
		return KBString();

	return (*valFind);
}

void KBEngineApp::resetPassword(const KBString& username)
{
	username_ = username;
	resetpassword_loginapp(true);
}

void KBEngineApp::resetpassword_loginapp(bool noconnect)
{
	if (noconnect)
	{
		reset();
		pNetworkInterface_->connectTo(pArgs_->ip, pArgs_->port, this, 4);
	}
	else
	{
		INFO_MSG("KBEngineApp::resetpassword_loginapp(): send resetpassword! username=%s", username_.c_str());
		Bundle* pBundle = Bundle::createObject();
		pBundle->newMessage(Messages::messages[KBTEXT("Loginapp_reqAccountResetPassword"]));
		(*pBundle) << username_;
		pBundle->send(pNetworkInterface_);
	}
}

void KBEngineApp::onOpenLoginapp_resetpassword()
{
	DEBUG_MSG("KBEngineApp::onOpenLoginapp_resetpassword(): successfully!");
	currserver_ = "loginapp";
	currstate_ = "resetpassword";
	lastTickCBTime_ = getTimeSeconds();

	resetpassword_loginapp(false);
}

void KBEngineApp::onConnectTo_resetpassword_callback(KBString ip, uint16 port, bool success)
{
	lastTickCBTime_ = getTimeSeconds();

	if (!success)
	{
		ERROR_MSG("KBEngineApp::onConnectTo_resetpassword_callback(): connect %s:%d is error!", ip.c_str(), port);
		return;
	}

	INFO_MSG("KBEngineApp::onConnectTo_resetpassword_callback(): connect %s:%d is success!", ip.c_str(), port);

	onOpenLoginapp_resetpassword();
}

void KBEngineApp::Client_onReqAccountResetPasswordCB(uint16 failcode)
{
	// auto* pEventData = NewObject<UKBEventData_onResetPassword>();
	auto pEventData = std::make_shared<UKBEventData_onResetPassword>();
	pEventData->failedcode = failcode;
	pEventData->errorStr = serverErr(failcode);
	KBENGINE_EVENT_FIRE_OUT(KBEventTypes::onResetPassword, pEventData);
	
	if (failcode != 0)
	{
		ERROR_MSG("KBEngineApp::Client_onReqAccountResetPasswordCB(): reset failed! code=%d, error=%s! username=%s", failcode, serverErr(failcode).c_str(), username_.c_str());
		return;
	}

	DEBUG_MSG("KBEngineApp::Client_onReqAccountResetPasswordCB(): successfully! username=%s", username_.c_str());
}

bool KBEngineApp::createAccount(const KBString& username, const KBString& password, const KBArray<uint8>& datas)
{
	if (username.length() == 0)
	{
		ERROR_MSG("KBEngineApp::createAccount(): username is empty!");
		return false;
	}

	if (password.length() == 0)
	{
		ERROR_MSG("KBEngineApp::createAccount(): password is empty!");
		return false;
	}

	username_ = username;
	password_ = password;
	clientdatas_ = datas;

	createAccount_loginapp(true);
	return true;
}

void KBEngineApp::createAccount_loginapp(bool noconnect)
{
	if (noconnect)
	{
		reset();
		pNetworkInterface_->connectTo(pArgs_->ip, pArgs_->port, this, 1);
	}
	else
	{
		INFO_MSG("KBEngineApp::createAccount_loginapp(): send create! username=%s", username_.c_str());
		Bundle* pBundle = Bundle::createObject();
		pBundle->newMessage(Messages::messages[KBTEXT("Loginapp_reqCreateAccount"]));
		(*pBundle) << username_;
		(*pBundle) << password_;
		pBundle->appendBlob(clientdatas_);
		pBundle->send(pNetworkInterface_);
	}
}

void KBEngineApp::onOpenLoginapp_createAccount()
{
	DEBUG_MSG("KBEngineApp::onOpenLoginapp_createAccount(): successfully!");

	currserver_ = KBTEXT("loginapp");
	currstate_ = KBTEXT("createAccount");
	lastTickCBTime_ = getTimeSeconds();

	createAccount_loginapp(false);
}

void KBEngineApp::onConnectTo_loginapp_create_callback(KBString ip, uint16 port, bool success)
{
	lastTickCBTime_ = getTimeSeconds();

	if (!success)
	{
		ERROR_MSG("KBEngineApp::onConnectTo_loginapp_create_callback(): connect %s:%d is error!", ip.c_str(), port);
		return;
	}

	INFO_MSG("KBEngineApp::onConnectTo_loginapp_create_callback(): connect %s:%d is success!", ip.c_str(), port);

	onOpenLoginapp_createAccount();
}

void KBEngineApp::Client_onCreateAccountResult(MemoryStream& stream)
{
	uint16 retcode;
	stream >> retcode;

	KBArray<uint8> datas;
	stream.readBlob(datas);

	// UKBEventData_onCreateAccountResult* pEventData = NewObject<UKBEventData_onCreateAccountResult>();
	auto pEventData = std::make_shared<UKBEventData_onCreateAccountResult>();
	pEventData->errorCode = retcode;
	pEventData->errorStr = serverErr(retcode);
	pEventData->datas = datas;
	KBENGINE_EVENT_FIRE_OUT(KBEventTypes::onCreateAccountResult, pEventData);

	if (retcode != 0)
	{
		WARNING_MSG("KBEngineApp::Client_onCreateAccountResult(): create(%s) failed! error=%d(%s)!", username_.c_str(), retcode, serverErr(retcode).c_str());
		return;
	}

	DEBUG_MSG("KBEngineApp::Client_onCreateAccountResult(): create(%s) is successfully!", username_.c_str());
}

void KBEngineApp::bindAccountEmail(const KBString& emailAddress)
{
	INFO_MSG("KBEngineApp::bindAccountEmail(): send bindAccountEmail! username=%s", username_.c_str());
	Bundle* pBundle = Bundle::createObject();
	pBundle->newMessage(Messages::messages[KBTEXT("Baseapp_reqAccountBindEmail"]));
	(*pBundle) << entity_id_;
	(*pBundle) << password_;
	(*pBundle) << emailAddress;
	pBundle->send(pNetworkInterface_);
}

void KBEngineApp::Client_onReqAccountBindEmailCB(uint16 failcode)
{
	// auto* pEventData = NewObject<UKBEventData_onBindAccountEmail>();
	auto pEventData = std::make_shared<UKBEventData_onBindAccountEmail>();
	pEventData->failedcode = failcode;
	pEventData->errorStr = serverErr(failcode);
	KBENGINE_EVENT_FIRE_OUT(KBEventTypes::onBindAccountEmail, pEventData);

	if (failcode != 0)
	{
		ERROR_MSG("KBEngineApp::Client_onReqAccountBindEmailCB(): bind failed! code=%d, error=%s! username=%s", failcode, serverErr(failcode).c_str(), username_.c_str());
		return;
	}

	DEBUG_MSG("KBEngineApp::Client_onReqAccountBindEmailCB(): successfully! username=%s", username_.c_str());
}

void KBEngineApp::newPassword(const KBString& old_password, const KBString& new_password)
{
	INFO_MSG("KBEngineApp::newPassword(): send newPassword! username=%s", username_.c_str());
	Bundle* pBundle = Bundle::createObject();
	pBundle->newMessage(Messages::messages[KBTEXT("Baseapp_reqAccountNewPassword"]));
	(*pBundle) << entity_id_;
	(*pBundle) << old_password;
	(*pBundle) << new_password;
	pBundle->send(pNetworkInterface_);
}

void KBEngineApp::Client_onReqAccountNewPasswordCB(uint16 failcode)
{
	// auto* pEventData = NewObject<UKBEventData_onNewPassword>();
	auto pEventData = std::make_shared<UKBEventData_onNewPassword>();
	pEventData->failedcode = failcode;
	pEventData->errorStr = serverErr(failcode);
	KBENGINE_EVENT_FIRE_OUT(KBEventTypes::onNewPassword, pEventData);

	if (failcode != 0)
	{
		ERROR_MSG("KBEngineApp::Client_onReqAccountNewPasswordCB(): newPassword failed! code=%d, error=%s! username=%s", failcode, serverErr(failcode).c_str(), username_.c_str());
		return;
	}

	DEBUG_MSG("KBEngineApp::Client_onReqAccountNewPasswordCB(): successfully! username=%s", username_.c_str());
}

void KBEngineApp::Client_onRemoteMethodCallOptimized(MemoryStream& stream)
{
	ENTITY_ID eid = getViewEntityIDFromStream(stream);
	onRemoteMethodCall_(eid, stream);
}

void KBEngineApp::Client_onRemoteMethodCall(MemoryStream& stream)
{
	ENTITY_ID eid = 0;
	stream >> eid;
	onRemoteMethodCall_(eid, stream);
}

void KBEngineApp::onRemoteMethodCall_(ENTITY_ID eid, MemoryStream& stream)
{
	Entity** pEntityFind = entities_.Find(eid);

	if (!pEntityFind)
	{
		ERROR_MSG("KBEngineApp::onRemoteMethodCall_(): entity(%d) not found!", eid);
		return;
	}

	Entity* pEntity = *pEntityFind;
	pEntity->onRemoteMethodCall(stream);
}

void KBEngineApp::Client_onControlEntity(ENTITY_ID eid, int8 isControlled)
{
	Entity** pEntityFind = entities_.Find(eid);

	if (!pEntityFind)
	{
		ERROR_MSG("KBEngineApp::Client_onControlEntity(): entity(%d) not found!", eid);
		return;
	}

	bool isCont = isControlled != 0;

	if (isCont)
	{
		// 如果被控制者是玩家自己，那表示玩家自己被其它人控制了
		// 所以玩家自己不应该进入这个被控制列表
		if (entity_id_ != (*pEntityFind)->id())
		{
			controlledEntities_.Add((*pEntityFind));
		}
	}
	else
	{
		controlledEntities_.Remove((*pEntityFind));
	}

	(*pEntityFind)->isControlled(isCont);
	(*pEntityFind)->onControlled(isCont);

	// UKBEventData_onControlled* pEventData = NewObject<UKBEventData_onControlled>();
	auto pEventData = std::make_shared<UKBEventData_onControlled>();
	pEventData->entityID = (*pEntityFind)->id();
	pEventData->isControlled = isCont;
	KBENGINE_EVENT_FIRE_OUT(KBEventTypes::onControlled, pEventData);
}

void KBEngineApp::Client_onStreamDataStarted(int16 id, uint32 datasize, KBString descr)
{
	// UKBEventData_onStreamDataStarted* pEventData = NewObject<UKBEventData_onStreamDataStarted>();
	auto pEventData = std::make_shared<UKBEventData_onStreamDataStarted>();
	pEventData->resID = id;
	pEventData->dataSize = datasize;
	pEventData->dataDescr = descr;
	KBENGINE_EVENT_FIRE_OUT(KBEventTypes::onStreamDataStarted, pEventData);
}

void KBEngineApp::Client_onStreamDataRecv(MemoryStream& stream)
{
	// UKBEventData_onStreamDataRecv* pEventData = NewObject<UKBEventData_onStreamDataRecv>();
	auto pEventData = std::make_shared<UKBEventData_onStreamDataRecv>();

	uint16 id = stream.read<uint16>();
	pEventData->resID = id;
	stream.readBlob(pEventData->data);

	KBENGINE_EVENT_FIRE_OUT(KBEventTypes::onStreamDataRecv, pEventData);
}

void KBEngineApp::Client_onStreamDataCompleted(int16 id)
{
	// UKBEventData_onStreamDataCompleted* pEventData = NewObject<UKBEventData_onStreamDataCompleted>();
	auto pEventData = std::make_shared<UKBEventData_onStreamDataCompleted>();
	pEventData->resID = id;
	KBENGINE_EVENT_FIRE_OUT(KBEventTypes::onStreamDataCompleted, pEventData);
}

void KBEngineApp::Client_onEntityEnterWorld(MemoryStream& stream)
{
	ENTITY_ID eid;
	stream >> eid;

	if (entity_id_ > 0 && entity_id_ != eid)
		entityIDAliasIDList_.Add(eid);

	uint16 uEntityType;

	if (EntityDef::idmoduledefs.Num() > 255)
		uEntityType = stream.read<uint16>();
	else
		uEntityType = stream.read<uint8>();

	int8 isOnGround = 1;

	if (stream.length() > 0)
		isOnGround = stream.read<int8>();

	ScriptModule** pScriptModuleFind = EntityDef::idmoduledefs.Find(uEntityType);
	if (!pScriptModuleFind)
	{
		SCREEN_ERROR_MSG("KBEngineApp::Client_onEntityEnterWorld(): not found ScriptModule(utype = %d)!", uEntityType);
		return;
	}

	ScriptModule* pScriptModule = *pScriptModuleFind;
	KBString entityType = pScriptModule->name;
	// DEBUG_MSG("KBEngineApp::Client_onEntityEnterWorld(): %s(%d), spaceID(%d)!", *entityType, eid, spaceID_);

	Entity** pEntityFind = entities_.Find(eid);

	if (!pEntityFind)
	{
		MemoryStream** entityMessageFind = bufferedCreateEntityMessages_.Find(eid);
		if (!entityMessageFind)
		{
			ERROR_MSG("KBEngineApp::Client_onEntityEnterWorld(): entity(%d) not found!", eid);
			return;
		}

		pScriptModuleFind = EntityDef::moduledefs.Find(entityType);
		if (!pScriptModuleFind)
		{
			SCREEN_ERROR_MSG("KBEngineApp::Client_onEntityEnterWorld(): not found ScriptModule(%s)!", entityType.c_str());
			return;
		}

		ScriptModule* pModule = *pScriptModuleFind;

		Entity* pEntity = pModule->createEntity();
		if (!pEntity)
		{
			SCREEN_ERROR_MSG("KBEngineApp::Client_onEntityEnterWorld(): createEntity() failed! entityType(%s)", entityType.c_str());
			return;
		}
		pEntity->id(eid);
		pEntity->className(entityType);
		pEntity->onGetCell();

		try
		{
			entities_.Add(eid, pEntity);

			Client_onUpdatePropertys(*(*entityMessageFind));
			MemoryStream::reclaimObject((*entityMessageFind));
			bufferedCreateEntityMessages_.Remove(eid);

			pEntity->isOnGround(isOnGround > 0);
			pEntity->onDirectionChanged(pEntity->direction);
			pEntity->onPositionChanged(pEntity->position);

			pEntity->__init__();
			pEntity->attachComponents();
			pEntity->inited(true);
			pEntity->inWorld(true);
			pEntity->enterWorld();

			if (pArgs_->isOnInitCallPropertysSetMethods)
				pEntity->callPropertysSetMethods();
		}
		catch (...)
		{
			SCREEN_ERROR_MSG("KBEngineApp::Client_onEntityEnterWorld(): init entity failed! eid(%d), entityType(%s)", eid, entityType.c_str());
			pEntity->destroy();
		}
	}
	else
	{
		Entity* pEntity = (*pEntityFind);

		if (!pEntity->inWorld())
		{
			// 安全起见， 这里清空一下
			// 如果服务端上使用giveClientTo切换控制权
			// 之前的实体已经进入世界， 切换后的实体也进入世界， 这里可能会残留之前那个实体进入世界的信息
			entityIDAliasIDList_.Clear();
			clearEntities(false);
			entities_.Add(pEntity->id(), pEntity);

			pEntity->onGetCell();

			pEntity->onDirectionChanged(pEntity->direction);
			pEntity->onPositionChanged(pEntity->position);

			entityServerPos_ = pEntity->position;
			pEntity->isOnGround(isOnGround > 0);
			pEntity->inWorld(true);
			pEntity->enterWorld();

			if (pArgs_->isOnInitCallPropertysSetMethods)
				pEntity->callPropertysSetMethods();
		}
	}
}

void KBEngineApp::Client_onEntityLeaveWorldOptimized(MemoryStream& stream)
{
	ENTITY_ID eid = getViewEntityIDFromStream(stream);
	Client_onEntityLeaveWorld(eid);
}

void KBEngineApp::Client_onEntityLeaveWorld(ENTITY_ID eid)
{
	Entity** pEntityFind = entities_.Find(eid);
	
	if (!pEntityFind)
	{
		ERROR_MSG("KBEngineApp::Client_onEntityLeaveWorld(): entity(%d) not found!", eid);
		return;
	}

	Entity* pEntity = *pEntityFind;

	if (pEntity->inWorld())
		pEntity->leaveWorld();

	if (entity_id_ == eid)
	{
		clearSpace(false);
		pEntity->onLoseCell();
	}
	else
	{
		if (controlledEntities_.Contains(pEntity))
		{
			controlledEntities_.Remove(pEntity);

			// UKBEventData_onLoseControlledEntity* pEventData = NewObject<UKBEventData_onLoseControlledEntity>();
			auto pEventData = std::make_shared<UKBEventData_onLoseControlledEntity>();
			pEventData->entityID = pEntity->id();
			KBENGINE_EVENT_FIRE_OUT(KBEventTypes::onLoseControlledEntity, pEventData);
		}

		entities_.Remove(eid);
		pEntity->destroy();
		entityIDAliasIDList_.Remove(eid);
	}
}

void KBEngineApp::Client_onEntityEnterSpace(MemoryStream& stream)
{
	ENTITY_ID eid = stream.read<int32>();
	spaceID_ = stream.read<uint32>();

	int8 isOnGround = 1;

	if (stream.length() > 0)
		isOnGround = stream.read<int8>();

	Entity** pEntityFind = entities_.Find(eid);

	if (!pEntityFind)
	{
		ERROR_MSG("KBEngineApp::Client_onEntityEnterSpace(): entity(%d) not found!", eid);
		return;
	}

	Entity* pEntity = *pEntityFind;
	pEntity->isOnGround(isOnGround > 0);
	entityServerPos_ = pEntity->position;
	pEntity->enterSpace();
}

void KBEngineApp::Client_onEntityLeaveSpace(ENTITY_ID eid)
{
	Entity** pEntityFind = entities_.Find(eid);

	if (!pEntityFind)
	{
		ERROR_MSG("KBEngineApp::Client_onEntityLeaveSpace(): entity(%d) not found!", eid);
		return;
	}

	Entity* pEntity = *pEntityFind;
	pEntity->leaveSpace();

	clearSpace(false);
}

void KBEngineApp::Client_onUpdateBasePos(float x, float y, float z)
{
	
	KBVector3f oldPos(entityServerPos_.x, entityServerPos_.y, entityServerPos_.z);
	entityServerPos_.x = x;
	entityServerPos_.y = y;
	entityServerPos_.z = z;

	Entity* pEntity = player();
	if (pEntity && pEntity->isControlled())
	{
		pEntity->position.Set(entityServerPos_.x, entityServerPos_.y, entityServerPos_.z);

		// auto pEventData = std::make_shared<UKBEventData_updatePosition>();
		// // KBPos2UE4Pos(pEventData->position, entityServerPos_);
		// // KBDir2UE4Dir(pEventData->direction, pEntity->direction);
		// pEventData->entityID = pEntity->id();
		// pEventData->moveSpeed = pEntity->velocity();
		// KBENGINE_EVENT_FIRE_OUT(KBEventTypes::updatePosition, pEventData);


		pEntity->onSmoothPositionChanged(oldPos);
		
		

		pEntity->onUpdateVolatileData();
	}
}

void KBEngineApp::Client_onUpdateBasePosXZ(float x, float z)
{
	KBVector3f oldPos(entityServerPos_.x, entityServerPos_.y, entityServerPos_.z);
	entityServerPos_.x = x;
	entityServerPos_.z = z;

	Entity* pEntity = player();
	if (pEntity && pEntity->isControlled())
	{
		pEntity->position.x = entityServerPos_.x;
		pEntity->position.z = entityServerPos_.z;

		// auto pEventData = std::make_shared<UKBEventData_updatePosition>();
		// // KBPos2UE4Pos(pEventData->position, entityServerPos_);
		// // KBDir2UE4Dir(pEventData->direction, pEntity->direction);
		// pEventData->entityID = pEntity->id();
		// pEventData->moveSpeed = pEntity->velocity();
		// KBENGINE_EVENT_FIRE_OUT(KBEventTypes::updatePosition, pEventData);

		pEntity->onSmoothPositionChanged(oldPos);

		pEntity->onUpdateVolatileData();
	}
}

void KBEngineApp::Client_onUpdateBaseDir(MemoryStream& stream)
{
	float yaw, pitch, roll;
	stream >> yaw >> pitch >> roll;

	Entity* pEntity = player();
	if (pEntity && pEntity->isControlled())
	{
		
		KBVector3f old_direction(pEntity->direction.x, pEntity->direction.y, pEntity->direction.z);

		pEntity->direction.Set(roll, pitch, yaw);

		// auto pEventData = std::make_shared<UKBEventData_set_direction>();
		// // KBDir2UE4Dir(pEventData->direction, pEntity->direction);
		// pEventData->entityID = pEntity->id();
		// KBENGINE_EVENT_FIRE_OUT(KBEventTypes::set_direction, pEventData);

		pEntity->onDirectionChanged(old_direction);

		pEntity->onUpdateVolatileData();
	}
}

void KBEngineApp::Client_onUpdateData(MemoryStream& stream)
{
	ENTITY_ID eid = getViewEntityIDFromStream(stream);

	Entity** pEntityFind = entities_.Find(eid);

	if (!pEntityFind)
	{
		ERROR_MSG("KBEngineApp::Client_onUpdateData(): entity(%d) not found!", eid);
		return;
	}
}

void KBEngineApp::Client_onSetEntityPosAndDir(MemoryStream& stream)
{
	ENTITY_ID eid;
	stream >> eid;

	Entity** pEntityFind = entities_.Find(eid);

	if (!pEntityFind)
	{
		ERROR_MSG("KBEngineApp::Client_onSetEntityPosAndDir(): entity(%d) not found!", eid);
		return;
	}
	
	Entity& entity = *(*pEntityFind);

	KBVector3f old_position = entity.position;
	KBVector3f old_direction = entity.direction;

	entity.position.x = stream.read<float>();
	entity.position.y = stream.read<float>();
	entity.position.z = stream.read<float>();

	entity.direction.x = stream.read<float>();
	entity.direction.y = stream.read<float>();
	entity.direction.z = stream.read<float>();

	entity.entityLastLocalPos = entity.position;
	entity.entityLastLocalDir = entity.direction;

	entity.onDirectionChanged(old_direction);
	entity.onPositionChanged(old_position);	
}

void KBEngineApp::Client_onUpdateData_ypr(MemoryStream& stream)
{
	ENTITY_ID eid = getViewEntityIDFromStream(stream);

	float y = stream.read<float>();
	float p = stream.read<float>();
	float r = stream.read<float>();

	_updateVolatileData(eid, KBE_FLT_MAX, KBE_FLT_MAX, KBE_FLT_MAX, y, p, r, -1, false);
}

void KBEngineApp::Client_onUpdateData_yp(MemoryStream& stream)
{
	ENTITY_ID eid = getViewEntityIDFromStream(stream);

	float y = stream.read<float>();
	float p = stream.read<float>();

	_updateVolatileData(eid, KBE_FLT_MAX, KBE_FLT_MAX, KBE_FLT_MAX, y, p, KBE_FLT_MAX, -1, false);
}

void KBEngineApp::Client_onUpdateData_yr(MemoryStream& stream)
{
	ENTITY_ID eid = getViewEntityIDFromStream(stream);

	float y = stream.read<float>();
	float r = stream.read<float>();

	_updateVolatileData(eid, KBE_FLT_MAX, KBE_FLT_MAX, KBE_FLT_MAX, y, KBE_FLT_MAX, r, -1, false);
}

void KBEngineApp::Client_onUpdateData_pr(MemoryStream& stream)
{
	ENTITY_ID eid = getViewEntityIDFromStream(stream);

	float p = stream.read<float>();
	float r = stream.read<float>();

	_updateVolatileData(eid, KBE_FLT_MAX, KBE_FLT_MAX, KBE_FLT_MAX, KBE_FLT_MAX, p, r, -1, false);
}

void KBEngineApp::Client_onUpdateData_y(MemoryStream& stream)
{
	ENTITY_ID eid = getViewEntityIDFromStream(stream);

	float y = stream.read<float>();

	_updateVolatileData(eid, KBE_FLT_MAX, KBE_FLT_MAX, KBE_FLT_MAX, y, KBE_FLT_MAX, KBE_FLT_MAX, -1, false);
}

void KBEngineApp::Client_onUpdateData_p(MemoryStream& stream)
{
	ENTITY_ID eid = getViewEntityIDFromStream(stream);

	float p = stream.read<float>();

	_updateVolatileData(eid, KBE_FLT_MAX, KBE_FLT_MAX, KBE_FLT_MAX, KBE_FLT_MAX, p, KBE_FLT_MAX, -1, false);
}

void KBEngineApp::Client_onUpdateData_r(MemoryStream& stream)
{
	ENTITY_ID eid = getViewEntityIDFromStream(stream);

	float r = stream.read<float>();

	_updateVolatileData(eid, KBE_FLT_MAX, KBE_FLT_MAX, KBE_FLT_MAX, KBE_FLT_MAX, KBE_FLT_MAX, r, -1, false);
}

void KBEngineApp::Client_onUpdateData_xz(MemoryStream& stream)
{
	ENTITY_ID eid = getViewEntityIDFromStream(stream);

	float x = stream.read<float>();
	float z = stream.read<float>();

	_updateVolatileData(eid, x, KBE_FLT_MAX, z, KBE_FLT_MAX, KBE_FLT_MAX, KBE_FLT_MAX, 1, false);
}

void KBEngineApp::Client_onUpdateData_xz_ypr(MemoryStream& stream)
{
	ENTITY_ID eid = getViewEntityIDFromStream(stream);

	float x = stream.read<float>();
	float z = stream.read<float>();

	float y = stream.read<float>();
	float p = stream.read<float>();
	float r = stream.read<float>();

	_updateVolatileData(eid, x, KBE_FLT_MAX, z, y, p, r, 1, false);
}

void KBEngineApp::Client_onUpdateData_xz_yp(MemoryStream& stream)
{
	ENTITY_ID eid = getViewEntityIDFromStream(stream);

	float x = stream.read<float>();
	float z = stream.read<float>();

	float y = stream.read<float>();
	float p = stream.read<float>();

	_updateVolatileData(eid, x, KBE_FLT_MAX, z, y, p, KBE_FLT_MAX, 1, false);
}

void KBEngineApp::Client_onUpdateData_xz_yr(MemoryStream& stream)
{
	ENTITY_ID eid = getViewEntityIDFromStream(stream);

	float x = stream.read<float>();
	float z = stream.read<float>();

	float y = stream.read<float>();
	float r = stream.read<float>();

	_updateVolatileData(eid, x, KBE_FLT_MAX, z, y, KBE_FLT_MAX, r, 1, false);
}

void KBEngineApp::Client_onUpdateData_xz_pr(MemoryStream& stream)
{
	ENTITY_ID eid = getViewEntityIDFromStream(stream);

	float x = stream.read<float>();
	float z = stream.read<float>();

	float p = stream.read<float>();
	float r = stream.read<float>();

	_updateVolatileData(eid, x, KBE_FLT_MAX, z, KBE_FLT_MAX, p, r, 1, false);
}

void KBEngineApp::Client_onUpdateData_xz_y(MemoryStream& stream)
{
	ENTITY_ID eid = getViewEntityIDFromStream(stream);

	float x = stream.read<float>();
	float z = stream.read<float>();

	float y = stream.read<float>();

	_updateVolatileData(eid, x, KBE_FLT_MAX, z, y, KBE_FLT_MAX, KBE_FLT_MAX, 1, false);
}

void KBEngineApp::Client_onUpdateData_xz_p(MemoryStream& stream)
{
	ENTITY_ID eid = getViewEntityIDFromStream(stream);

	float x = stream.read<float>();
	float z = stream.read<float>();

	float p = stream.read<float>();

	_updateVolatileData(eid, x, KBE_FLT_MAX, z, KBE_FLT_MAX, p, KBE_FLT_MAX, 1, false);
}

void KBEngineApp::Client_onUpdateData_xz_r(MemoryStream& stream)
{
	ENTITY_ID eid = getViewEntityIDFromStream(stream);

	float x = stream.read<float>();
	float z = stream.read<float>();

	float r = stream.read<float>();

	_updateVolatileData(eid, x, KBE_FLT_MAX, z, KBE_FLT_MAX, KBE_FLT_MAX, r, 1, false);
}

void KBEngineApp::Client_onUpdateData_xyz(MemoryStream& stream)
{
	ENTITY_ID eid = getViewEntityIDFromStream(stream);

	float x = stream.read<float>();
	float y = stream.read<float>();
	float z = stream.read<float>();

	_updateVolatileData(eid, x, y, z, KBE_FLT_MAX, KBE_FLT_MAX, KBE_FLT_MAX, 0, false);
}

void KBEngineApp::Client_onUpdateData_xyz_ypr(MemoryStream& stream)
{
	ENTITY_ID eid = getViewEntityIDFromStream(stream);

	float x = stream.read<float>();
	float y = stream.read<float>();
	float z = stream.read<float>();

	float yaw = stream.read<float>();
	float p = stream.read<float>();
	float r = stream.read<float>();

	_updateVolatileData(eid, x, y, z, yaw, p, r, 0, false);
}

void KBEngineApp::Client_onUpdateData_xyz_yp(MemoryStream& stream)
{
	ENTITY_ID eid = getViewEntityIDFromStream(stream);

	float x = stream.read<float>();
	float y = stream.read<float>();
	float z = stream.read<float>();

	float yaw = stream.read<float>();
	float p = stream.read<float>();

	_updateVolatileData(eid, x, y, z, yaw, p, KBE_FLT_MAX, 0, false);
}

void KBEngineApp::Client_onUpdateData_xyz_yr(MemoryStream& stream)
{
	ENTITY_ID eid = getViewEntityIDFromStream(stream);

	float x = stream.read<float>();
	float y = stream.read<float>();
	float z = stream.read<float>();

	float yaw = stream.read<float>();
	float r = stream.read<float>();

	_updateVolatileData(eid, x, y, z, yaw, KBE_FLT_MAX, r, 0, false);
}

void KBEngineApp::Client_onUpdateData_xyz_pr(MemoryStream& stream)
{
	ENTITY_ID eid = getViewEntityIDFromStream(stream);

	float x = stream.read<float>();
	float y = stream.read<float>();
	float z = stream.read<float>();

	float p = stream.read<float>();
	float r = stream.read<float>();

	_updateVolatileData(eid, x, y, z, KBE_FLT_MAX, p, r, 0, false);
}

void KBEngineApp::Client_onUpdateData_xyz_y(MemoryStream& stream)
{
	ENTITY_ID eid = getViewEntityIDFromStream(stream);

	float x = stream.read<float>();
	float y = stream.read<float>();
	float z = stream.read<float>();

	float yaw = stream.read<float>();

	_updateVolatileData(eid, x, y, z, yaw, KBE_FLT_MAX, KBE_FLT_MAX, 0, false);
}

void KBEngineApp::Client_onUpdateData_xyz_p(MemoryStream& stream)
{
	ENTITY_ID eid = getViewEntityIDFromStream(stream);

	float x = stream.read<float>();
	float y = stream.read<float>();
	float z = stream.read<float>();

	float p = stream.read<float>();

	_updateVolatileData(eid, x, y, z, KBE_FLT_MAX, p, KBE_FLT_MAX, 0, false);
}

void KBEngineApp::Client_onUpdateData_xyz_r(MemoryStream& stream)
{
	ENTITY_ID eid = getViewEntityIDFromStream(stream);

	float x = stream.read<float>();
	float y = stream.read<float>();
	float z = stream.read<float>();

	float r = stream.read<float>();

	_updateVolatileData(eid, x, y, z, KBE_FLT_MAX, KBE_FLT_MAX, r, 0, false);
}

void KBEngineApp::Client_onUpdateData_ypr_optimized(MemoryStream& stream)
{
	ENTITY_ID eid = getViewEntityIDFromStream(stream);

	int8 y = stream.read<int8>();
	int8 p = stream.read<int8>();
	int8 r = stream.read<int8>();

	_updateVolatileData(eid, KBE_FLT_MAX, KBE_FLT_MAX, KBE_FLT_MAX, y, p, r, -1, true);
}

void KBEngineApp::Client_onUpdateData_yp_optimized(MemoryStream& stream)
{
	ENTITY_ID eid = getViewEntityIDFromStream(stream);

	int8 y = stream.read<int8>();
	int8 p = stream.read<int8>();

	_updateVolatileData(eid, KBE_FLT_MAX, KBE_FLT_MAX, KBE_FLT_MAX, y, p, KBE_FLT_MAX, -1, true);
}

void KBEngineApp::Client_onUpdateData_yr_optimized(MemoryStream& stream)
{
	ENTITY_ID eid = getViewEntityIDFromStream(stream);

	int8 y = stream.read<int8>();
	int8 r = stream.read<int8>();

	_updateVolatileData(eid, KBE_FLT_MAX, KBE_FLT_MAX, KBE_FLT_MAX, y, KBE_FLT_MAX, r, -1, true);
}

void KBEngineApp::Client_onUpdateData_pr_optimized(MemoryStream& stream)
{
	ENTITY_ID eid = getViewEntityIDFromStream(stream);

	int8 p = stream.read<int8>();
	int8 r = stream.read<int8>();

	_updateVolatileData(eid, KBE_FLT_MAX, KBE_FLT_MAX, KBE_FLT_MAX, KBE_FLT_MAX, p, r, -1, true);
}

void KBEngineApp::Client_onUpdateData_y_optimized(MemoryStream& stream)
{
	ENTITY_ID eid = getViewEntityIDFromStream(stream);

	int8 y = stream.read<int8>();

	_updateVolatileData(eid, KBE_FLT_MAX, KBE_FLT_MAX, KBE_FLT_MAX, y, KBE_FLT_MAX, KBE_FLT_MAX, -1, true);
}

void KBEngineApp::Client_onUpdateData_p_optimized(MemoryStream& stream)
{
	ENTITY_ID eid = getViewEntityIDFromStream(stream);

	int8 p = stream.read<int8>();

	_updateVolatileData(eid, KBE_FLT_MAX, KBE_FLT_MAX, KBE_FLT_MAX, KBE_FLT_MAX, p, KBE_FLT_MAX, -1, true);
}

void KBEngineApp::Client_onUpdateData_r_optimized(MemoryStream& stream)
{
	ENTITY_ID eid = getViewEntityIDFromStream(stream);

	int8 r = stream.read<int8>();

	_updateVolatileData(eid, KBE_FLT_MAX, KBE_FLT_MAX, KBE_FLT_MAX, KBE_FLT_MAX, KBE_FLT_MAX, r, -1, true);
}

void KBEngineApp::Client_onUpdateData_xz_optimized(MemoryStream& stream)
{
	ENTITY_ID eid = getViewEntityIDFromStream(stream);

	KBVector3f xz;
	stream.readPackXZ(xz.x, xz.z);

	_updateVolatileData(eid, xz.x, KBE_FLT_MAX, xz.z, KBE_FLT_MAX, KBE_FLT_MAX, KBE_FLT_MAX, 1, true);
}

void KBEngineApp::Client_onUpdateData_xz_ypr_optimized(MemoryStream& stream)
{
	ENTITY_ID eid = getViewEntityIDFromStream(stream);

	KBVector3f xz;
	stream.readPackXZ(xz.x, xz.z);

	int8 y = stream.read<int8>();
	int8 p = stream.read<int8>();
	int8 r = stream.read<int8>();

	_updateVolatileData(eid, xz.x, KBE_FLT_MAX, xz.z, y, p, r, 1, true);
}

void KBEngineApp::Client_onUpdateData_xz_yp_optimized(MemoryStream& stream)
{
	ENTITY_ID eid = getViewEntityIDFromStream(stream);

	KBVector3f xz;
	stream.readPackXZ(xz.x, xz.z);

	int8 y = stream.read<int8>();
	int8 p = stream.read<int8>();

	_updateVolatileData(eid, xz.x, KBE_FLT_MAX, xz.z, y, p, KBE_FLT_MAX, 1, true);
}

void KBEngineApp::Client_onUpdateData_xz_yr_optimized(MemoryStream& stream)
{
	ENTITY_ID eid = getViewEntityIDFromStream(stream);

	KBVector3f xz;
	stream.readPackXZ(xz.x, xz.z);

	int8 y = stream.read<int8>();
	int8 r = stream.read<int8>();

	_updateVolatileData(eid, xz.x, KBE_FLT_MAX, xz.z, y, KBE_FLT_MAX, r, 1, true);
}

void KBEngineApp::Client_onUpdateData_xz_pr_optimized(MemoryStream& stream)
{
	ENTITY_ID eid = getViewEntityIDFromStream(stream);

	KBVector3f xz;
	stream.readPackXZ(xz.x, xz.z);

	int8 p = stream.read<int8>();
	int8 r = stream.read<int8>();

	_updateVolatileData(eid, xz.x, KBE_FLT_MAX, xz.z, KBE_FLT_MAX, p, r, 1, true);
}

void KBEngineApp::Client_onUpdateData_xz_y_optimized(MemoryStream& stream)
{
	ENTITY_ID eid = getViewEntityIDFromStream(stream);

	KBVector3f xz;
	stream.readPackXZ(xz.x, xz.z);

	int8 y = stream.read<int8>();

	_updateVolatileData(eid, xz.x, KBE_FLT_MAX, xz.z, y, KBE_FLT_MAX, KBE_FLT_MAX, 1, true);
}

void KBEngineApp::Client_onUpdateData_xz_p_optimized(MemoryStream& stream)
{
	ENTITY_ID eid = getViewEntityIDFromStream(stream);

	KBVector3f xz;
	stream.readPackXZ(xz.x, xz.z);

	int8 p = stream.read<int8>();

	_updateVolatileData(eid, xz.x, KBE_FLT_MAX, xz.z, KBE_FLT_MAX, p, KBE_FLT_MAX, 1, true);
}

void KBEngineApp::Client_onUpdateData_xz_r_optimized(MemoryStream& stream)
{
	ENTITY_ID eid = getViewEntityIDFromStream(stream);

	KBVector3f xz;
	stream.readPackXZ(xz.x, xz.z);

	int8 r = stream.read<int8>();

	_updateVolatileData(eid, xz.x, KBE_FLT_MAX, xz.z, KBE_FLT_MAX, KBE_FLT_MAX, r, 1, true);
}

void KBEngineApp::Client_onUpdateData_xyz_optimized(MemoryStream& stream)
{
	ENTITY_ID eid = getViewEntityIDFromStream(stream);

	KBVector3f xz;
	stream.readPackXZ(xz.x, xz.z);
	stream.readPackY(xz.y);

	_updateVolatileData(eid, xz.x, xz.y, xz.z, KBE_FLT_MAX, KBE_FLT_MAX, KBE_FLT_MAX, 0, true);
}

void KBEngineApp::Client_onUpdateData_xyz_ypr_optimized(MemoryStream& stream)
{
	ENTITY_ID eid = getViewEntityIDFromStream(stream);

	KBVector3f xz;
	stream.readPackXZ(xz.x, xz.z);
	stream.readPackY(xz.y);

	int8 y = stream.read<int8>();
	int8 p = stream.read<int8>();
	int8 r = stream.read<int8>();

	_updateVolatileData(eid, xz.x, xz.y, xz.z, y, p, r, 0, true);
}

void KBEngineApp::Client_onUpdateData_xyz_yp_optimized(MemoryStream& stream)
{
	ENTITY_ID eid = getViewEntityIDFromStream(stream);

	KBVector3f xz;
	stream.readPackXZ(xz.x, xz.z);
	stream.readPackY(xz.y);

	int8 y = stream.read<int8>();
	int8 p = stream.read<int8>();

	_updateVolatileData(eid, xz.x, xz.y, xz.z, y, p, KBE_FLT_MAX, 0, true);
}

void KBEngineApp::Client_onUpdateData_xyz_yr_optimized(MemoryStream& stream)
{
	ENTITY_ID eid = getViewEntityIDFromStream(stream);

	KBVector3f xz;
	stream.readPackXZ(xz.x, xz.z);
	stream.readPackY(xz.y);

	int8 y = stream.read<int8>();
	int8 r = stream.read<int8>();

	_updateVolatileData(eid, xz.x, xz.y, xz.z, y, KBE_FLT_MAX, r, 0, true);
}

void KBEngineApp::Client_onUpdateData_xyz_pr_optimized(MemoryStream& stream)
{
	ENTITY_ID eid = getViewEntityIDFromStream(stream);

	KBVector3f xz;
	stream.readPackXZ(xz.x, xz.z);
	stream.readPackY(xz.y);

	int8 p = stream.read<int8>();
	int8 r = stream.read<int8>();

	_updateVolatileData(eid, xz.x, xz.y, xz.z, KBE_FLT_MAX, p, r, 0, true);
}

void KBEngineApp::Client_onUpdateData_xyz_y_optimized(MemoryStream& stream)
{
	ENTITY_ID eid = getViewEntityIDFromStream(stream);

	KBVector3f xz;
	stream.readPackXZ(xz.x, xz.z);
	stream.readPackY(xz.y);

	int8 y = stream.read<int8>();

	_updateVolatileData(eid, xz.x, xz.y, xz.z, y, KBE_FLT_MAX, KBE_FLT_MAX, 0, true);
}

void KBEngineApp::Client_onUpdateData_xyz_p_optimized(MemoryStream& stream)
{
	ENTITY_ID eid = getViewEntityIDFromStream(stream);

	KBVector3f xz;
	stream.readPackXZ(xz.x, xz.z);
	stream.readPackY(xz.y);

	int8 p = stream.read<int8>();

	_updateVolatileData(eid, xz.x, xz.y, xz.z, KBE_FLT_MAX, p, KBE_FLT_MAX, 0, true);
}

void KBEngineApp::Client_onUpdateData_xyz_r_optimized(MemoryStream& stream)
{
	ENTITY_ID eid = getViewEntityIDFromStream(stream);

	KBVector3f xz;
	stream.readPackXZ(xz.x, xz.z);
	stream.readPackY(xz.y);

	int8 r = stream.read<int8>();

	_updateVolatileData(eid, xz.x, xz.y, xz.z, KBE_FLT_MAX, KBE_FLT_MAX, r, 0, true);
}

void KBEngineApp::_updateVolatileData(ENTITY_ID entityID, float x, float y, float z, float yaw, float pitch, float roll, int8 isOnGround, bool isOptimized)
{
	Entity** pEntityFind = entities_.Find(entityID);

	if (!pEntityFind)
	{
		// 如果为0且客户端上一步是重登陆或者重连操作并且服务端entity在断线期间一直处于在线状态
		// 则可以忽略这个错误, 因为cellapp可能一直在向baseapp发送同步消息， 当客户端重连上时未等
		// 服务端初始化步骤开始则收到同步信息, 此时这里就会出错。
		ERROR_MSG("KBEngineApp::_updateVolatileData(): entity(%d) not found!", entityID);
		return;
	}

	Entity& entity = *(*pEntityFind);

	KBVector3f old_direction(entity.direction.x, entity.direction.y, entity.direction.z);

	// 小于0不设置
	if (isOnGround >= 0)
	{
		entity.isOnGround(isOnGround > 0);
	}

	bool changeDirection = false;

	if (roll != KBE_FLT_MAX)
	{
		changeDirection = true;
		entity.direction.x = isOptimized ? int82angle((int8)roll, false) : roll;
	}

	if (pitch != KBE_FLT_MAX)
	{
		changeDirection = true;
		entity.direction.y = isOptimized ? int82angle((int8)pitch, false) : pitch;
	}

	if (yaw != KBE_FLT_MAX)
	{
		changeDirection = true;
		entity.direction.z = isOptimized ? int82angle((int8)yaw, false) : yaw;
	}

	bool done = false;
	if (changeDirection == true)
	{
		// auto pEventData = std::make_shared<UKBEventData_set_direction>();
		// // KBDir2UE4Dir(pEventData->direction, entity.direction);
		// pEventData->entityID = entity.id();
		// KBENGINE_EVENT_FIRE_OUT(KBEventTypes::set_direction, pEventData);

		entity.onDirectionChanged(old_direction);

		done = true;
	}

	bool positionChanged = x != KBE_FLT_MAX || y != KBE_FLT_MAX || z != KBE_FLT_MAX;
	if (x == KBE_FLT_MAX) x = isOptimized ? 0.0f : entity.position.x;
	if (y == KBE_FLT_MAX) y = isOptimized ? 0.0f : entity.position.y;
	if (z == KBE_FLT_MAX) z = isOptimized ? 0.0f : entity.position.z;
	            
	if (positionChanged)
	{
		KBVector3f oldPos(entity.position.x, entity.position.y, entity.position.z);
		entity.position = isOptimized ? KBVector3f(x + entityServerPos_.x, y + entityServerPos_.y, z + entityServerPos_.z) : KBVector3f(x, y, z);
		done = true;

		// auto pEventData = std::make_shared<UKBEventData_updatePosition>();
		// // KBPos2UE4Pos(pEventData->position, entity.position);
		// // KBDir2UE4Dir(pEventData->direction, entity.direction);
		// pEventData->entityID = entity.id();
		// pEventData->moveSpeed = entity.velocity();
		// pEventData->isOnGround = entity.isOnGround();
		// KBENGINE_EVENT_FIRE_OUT(KBEventTypes::updatePosition, pEventData);


		entity.onSmoothPositionChanged(oldPos);
	}

	if (done)
		entity.onUpdateVolatileData();
}

}
