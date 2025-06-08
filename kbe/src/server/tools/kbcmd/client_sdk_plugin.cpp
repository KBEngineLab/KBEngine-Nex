#include "kbcmd.h"
#include "client_sdk.h"
#include "client_sdk_plugin.h"	
#include "entitydef/entitydef.h"
#include "entitydef/scriptdef_module.h"
#include "entitydef/property.h"
#include "entitydef/method.h"
#include "entitydef/datatype.h"
#include "network/fixed_messages.h"

namespace KBEngine {	

//-------------------------------------------------------------------------------------
ClientSDKPlugin::ClientSDKPlugin():
	ClientSDK(),
	initBody_()
{
}

//-------------------------------------------------------------------------------------
ClientSDKPlugin::~ClientSDKPlugin()
{

}

bool ClientSDKPlugin::create(const std::string& path)
{
	/*
	strutil::kbe_replace(filebody, "@{KBE_VERSION}", KBEVersion::versionString());
	strutil::kbe_replace(filebody, "@{KBE_SCRIPT_VERSION}", KBEVersion::scriptVersionString());
	strutil::kbe_replace(filebody, "@{KBE_SERVER_PROTO_MD5}", Network::MessageHandlers::getDigestStr());
	strutil::kbe_replace(filebody, "@{KBE_SERVER_ENTITYDEF_MD5}", EntityDef::md5().getDigestStr());
	strutil::kbe_replace(filebody, "@{KBE_USE_ALIAS_ENTITYID}", g_kbeSrvConfig.getCellApp().aliasEntityID ? "true" : "false");
	strutil::kbe_replace(filebody, "@{KBE_UPDATEHZ}", fmt::format("{}", g_kbeSrvConfig.gameUpdateHertz()));
	strutil::kbe_replace(filebody, "@{KBE_LOGIN_PORT}", fmt::format("{}", g_kbeSrvConfig.getLoginApp().externalTcpPorts_min));
	strutil::kbe_replace(filebody, "@{KBE_SERVER_EXTERNAL_TIMEOUT}", fmt::format("{}", (int)g_kbeSrvConfig.channelExternalTimeout()));*/



	basepath_ = path;

	if (basepath_[basepath_.size() - 1] != '\\' && basepath_[basepath_.size() - 1] != '/')
		basepath_ += "/";


	if (KBCMD::creatDir(basepath_.c_str()) == -1)
	{
		ERROR_MSG(fmt::format("creating directory error! path={}\n", basepath_));
		return false;
	}


	std::string configPath = basepath_ + "kbex.json";

	DEBUG_MSG(fmt::format("ClientSDK::saveFile(): {}\n",
		configPath));

	FILE* fp = fopen(configPath.c_str(), "w");

	if (NULL == fp)
	{
		ERROR_MSG(fmt::format("ClientSDK::saveFile(): fopen error! {}\n",
			configPath));

		return false;
	}


	sourcefileBody_ = fmt::format(R"({{"KBE_VERSION":"{}","KBE_SCRIPT_VERSION":"{}","KBE_SERVER_PROTO_MD5":"{}","KBE_SERVER_ENTITYDEF_MD5":"{}","KBE_USE_ALIAS_ENTITYID":"{}","KBE_UPDATEHZ":"{}","KBE_LOGIN_PORT":"{}","KBE_SERVER_EXTERNAL_TIMEOUT":"{}"}})",
		KBEVersion::versionString(),
		KBEVersion::scriptVersionString(),
		Network::MessageHandlers::getDigestStr(),
		EntityDef::md5().getDigestStr(),
		g_kbeSrvConfig.getCellApp().aliasEntityID ? "true" : "false",
		g_kbeSrvConfig.gameUpdateHertz(),
		g_kbeSrvConfig.getLoginApp().externalTcpPorts_min,
		(int)g_kbeSrvConfig.channelExternalTimeout()
	);


	int written = fwrite(sourcefileBody_.c_str(), 1, sourcefileBody_.size(), fp);
	if (written != (int)sourcefileBody_.size())
	{
		ERROR_MSG(fmt::format("ClientSDK::saveFile(): fwrite error! {}\n",
			configPath));

		fclose(fp);
		return false;
	}

	if (fclose(fp))
	{
		ERROR_MSG(fmt::format("ClientSDK::saveFile(): fclose error! {}\n",
			configPath));

		return false;
	}

	return true;
}

}
