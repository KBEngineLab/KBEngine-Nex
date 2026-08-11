/*
This source file is part of KBEngine
For the latest info, see http://www.kbengine.org/

Copyright (c) 2008-2018 KBEngine.

KBEngine is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

KBEngine is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.
 
You should have received a copy of the GNU Lesser General Public License
along with KBEngine.  If not, see <http://www.gnu.org/licenses/>.
*/


#include "serverconfig.h"
#include "network/common.h"
#include "network/address.h"
#include "resmgr/resmgr.h"
#include "common/kbekey.h"
#include "common/kbeversion.h"

#include <cerrno>

#ifndef CODE_INLINE
#include "serverconfig.inl"
#endif

namespace KBEngine{
KBE_SINGLETON_INIT(ServerConfig);

static bool g_dbmgr_addDefaultAddress = true;

static void loadRawDatabaseCommandBlacklist(XML* xml, TiXmlNode* rootNode, const char* dbType,
	std::map<std::string, std::vector<std::string> >& blacklist)
{
	TiXmlNode* node = xml->enterNode(rootNode, dbType);
	if (node == NULL)
		return;

	std::vector<std::string>& words = blacklist[strutil::toLower(dbType)];
	words.clear();

	std::vector<std::string> values;
	strutil::kbe_splits(xml->getValStr(node), ",", values, false);
	for (std::vector<std::string>::iterator iter = values.begin(); iter != values.end(); ++iter)
	{
		std::string word = strutil::toLower(strutil::kbe_trim(*iter));
		if (!word.empty())
			words.push_back(word);
	}
}

//-------------------------------------------------------------------------------------
ServerConfig::ServerConfig():
	gameUpdateHertz_(10),
	asyncioRepeatOffset_(0.f),
	performanceProbesEnabled_(false),
	guiConsoleAdminToken_("8be0f32909da7702f7cf2424af6e9422b4881f70bd5991f973424fed24004e5e"),
	tick_max_buffered_logs_(4096),
	tick_max_sync_logs_(32),
	channelCommon_(),
	bitsPerSecondToClient_(0),
	interfacesAddress_(),
	interfacesPort_min_(0),
	interfacesPort_max_(0),
	interfacesAddrs_(),
	interfaces_orders_timeout_(0),
	shutdown_time_(1.f),
	shutdown_waitTickTime_(1.f),
	callback_timeout_(180.f),
	thread_timeout_(300.f),
	thread_init_create_(1),
	thread_pre_create_(2),
	thread_max_create_(8),
	emailServerInfo_(),
	emailAtivationInfo_(),
	emailResetPasswordInfo_(),
	emailBindInfo_()
{
}

//-------------------------------------------------------------------------------------
ServerConfig::~ServerConfig()
{
}

//-------------------------------------------------------------------------------------
bool ServerConfig::loadConfig(std::string fileName)
{
	TiXmlNode* node = NULL, *rootNode = NULL;
	SmartPointer<XML> xml(new XML(Resmgr::getSingleton().matchRes(fileName).c_str()));

	if(!xml->isGood())
	{
		ERROR_MSG(fmt::format("ServerConfig::loadConfig: load {} is failed!\n",
			fileName.c_str()));

		return false;
	}
	
	if(xml->getRootNode() == NULL)
	{
		// root节点下没有子节点了
		return true;
	}

	rootNode = xml->getRootNode("performanceProbes");
	if (rootNode != NULL)
	{
		TiXmlNode* childnode = xml->enterNode(rootNode, "enabled");
		if (childnode != NULL)
		{
			performanceProbesEnabled_ = strutil::toLower(strutil::kbe_trim(
				xml->getValStr(childnode))) == "true";
			// 全局只读快路径避免网络、AOI 和脚本库反向依赖 ServerConfig。
			// A process-wide read-mostly switch avoids reverse dependencies from network, AOI and script libraries to ServerConfig.
			g_performanceProbesEnabled = performanceProbesEnabled_;
		}
	}

	rootNode = xml->getRootNode("guiConsole");
	if (rootNode != NULL)
	{
		TiXmlNode* childnode = xml->enterNode(rootNode, "adminToken");
		if (childnode != NULL)
		{
			guiConsoleAdminToken_ = strutil::kbe_trim(xml->getValStr(childnode));
		}
	}

	std::string email_service_config;
	rootNode = xml->getRootNode("email_service_config");
	if(rootNode != NULL)
	{
		email_service_config = xml->getValStr(rootNode);
	}

	rootNode = xml->getRootNode("packetAlwaysContainLength");
	if(rootNode != NULL){
		Network::g_packetAlwaysContainLength = xml->getValInt(rootNode) != 0;
	}

	rootNode = xml->getRootNode("trace_packet");
	if(rootNode != NULL)
	{
		TiXmlNode* childnode = xml->enterNode(rootNode, "debug_type");
		if(childnode)
			Network::g_trace_packet = xml->getValInt(childnode);

		if(Network::g_trace_packet > 3)
			Network::g_trace_packet = 0;

		childnode = xml->enterNode(rootNode, "use_logfile");
		if(childnode)
			Network::g_trace_packet_use_logfile = (xml->getValStr(childnode) == "true");

		childnode = xml->enterNode(rootNode, "disables");
		if(childnode)
		{
			do
			{
				if(childnode->FirstChild() != NULL)
				{
					std::string c = childnode->FirstChild()->Value();
					c = strutil::kbe_trim(c);
					if(c.size() > 0)
					{
						Network::g_trace_packet_disables.push_back(c);
						
						// 不debug加密包
						if(c == "Encrypted::packets")
							Network::g_trace_encrypted_packet = false;
					}
				}
			}while((childnode = childnode->NextSibling()));
		}
	}

	rootNode = xml->getRootNode("debugEntity");
	if(rootNode != NULL)
	{
		g_debugEntity = xml->getValInt(rootNode) > 0;
	}

	rootNode = xml->getRootNode("publish");
	if(rootNode != NULL)
	{
		TiXmlNode* childnode = xml->enterNode(rootNode, "state");
		if(childnode)
		{
			g_appPublish = xml->getValInt(childnode);
		}

		childnode = xml->enterNode(rootNode, "script_version");
		if(childnode)
		{
			KBEVersion::setScriptVersion(xml->getValStr(childnode));
		}
	}

	rootNode = xml->getRootNode("shutdown_time");
	if(rootNode != NULL)
	{
		shutdown_time_ = float(xml->getValFloat(rootNode));
	}
	
	rootNode = xml->getRootNode("shutdown_waittick");
	if(rootNode != NULL)
	{
		shutdown_waitTickTime_ = float(xml->getValFloat(rootNode));
	}

	rootNode = xml->getRootNode("callback_timeout");
	if(rootNode != NULL)
	{
		callback_timeout_ = float(xml->getValFloat(rootNode));
		if(callback_timeout_ < 5.f)
			callback_timeout_ = 5.f;
	}

	rootNode = xml->getRootNode("urlopen");
	if(rootNode != NULL)
	{
		// 所有值以秒或bytes/s表示；负值归零，0沿用libcurl的禁用或默认语义。
		// Values are seconds or bytes/s; negatives clamp to zero, while zero keeps libcurl's disabled or default semantics.
		TiXmlNode* childnode = xml->enterNode(rootNode, "timeout");
		if(childnode)
			Network::g_httpRequestTimeoutConfig.totalSeconds =
				static_cast<uint32>(KBE_MAX(0, xml->getValInt(childnode)));

		childnode = xml->enterNode(rootNode, "connectTimeout");
		if(childnode)
			Network::g_httpRequestTimeoutConfig.connectSeconds =
				static_cast<uint32>(KBE_MAX(0, xml->getValInt(childnode)));

		childnode = xml->enterNode(rootNode, "lowSpeedTime");
		if(childnode)
			Network::g_httpRequestTimeoutConfig.lowSpeedSeconds =
				static_cast<uint32>(KBE_MAX(0, xml->getValInt(childnode)));

		childnode = xml->enterNode(rootNode, "lowSpeedLimit");
		if(childnode)
			Network::g_httpRequestTimeoutConfig.lowSpeedBytesPerSecond =
				static_cast<uint32>(KBE_MAX(0, xml->getValInt(childnode)));
	}
	
	rootNode = xml->getRootNode("thread_pool");
	if(rootNode != NULL)
	{
		TiXmlNode* childnode = xml->enterNode(rootNode, "timeout");
		if(childnode)
		{
			thread_timeout_ = float(KBE_MAX(1.0, xml->getValFloat(childnode)));
		}

		childnode = xml->enterNode(rootNode, "init_create");
		if(childnode)
		{
			thread_init_create_ = KBE_MAX(1, xml->getValInt(childnode));
		}

		childnode = xml->enterNode(rootNode, "pre_create");
		if(childnode)
		{
			thread_pre_create_ = KBE_MAX(1, xml->getValInt(childnode));
		}

		childnode = xml->enterNode(rootNode, "max_create");
		if(childnode)
		{
			thread_max_create_ = KBE_MAX(1, xml->getValInt(childnode));
		}
	}

	rootNode = xml->getRootNode("channelCommon");
	if(rootNode != NULL)
	{
		TiXmlNode* childnode = xml->enterNode(rootNode, "timeout");
		if(childnode)
		{
			TiXmlNode* childnode1 = xml->enterNode(childnode, "internal");
			if(childnode1)
			{
				channelCommon_.channelInternalTimeout = KBE_MAX(0.f, float(xml->getValFloat(childnode1)));
				Network::g_channelInternalTimeout = channelCommon_.channelInternalTimeout;
			}

			childnode1 = xml->enterNode(childnode, "external");
			if(childnode)
			{
				channelCommon_.channelExternalTimeout = KBE_MAX(0.f, float(xml->getValFloat(childnode1)));
				Network::g_channelExternalTimeout = channelCommon_.channelExternalTimeout;
			}
		}

		childnode = xml->enterNode(rootNode, "resend");
		if(childnode)
		{
			TiXmlNode* childnode1 = xml->enterNode(childnode, "internal");
			if(childnode1)
			{
				TiXmlNode* childnode2 = xml->enterNode(childnode1, "interval");
				if(childnode2)
				{
					Network::g_intReSendInterval = uint32(xml->getValInt(childnode2));
				}

				childnode2 = xml->enterNode(childnode1, "retries");
				if(childnode2)
				{
					Network::g_intReSendRetries = uint32(xml->getValInt(childnode2));
				}
			}

			childnode1 = xml->enterNode(childnode, "external");
			if(childnode)
			{
				TiXmlNode* childnode2 = xml->enterNode(childnode1, "interval");
				if(childnode2)
				{
					Network::g_extReSendInterval = uint32(xml->getValInt(childnode2));
				}

				childnode2 = xml->enterNode(childnode1, "retries");
				if(childnode2)
				{
					Network::g_extReSendRetries = uint32(xml->getValInt(childnode2));
				}
			}
		}

		childnode = xml->enterNode(rootNode, "readBufferSize");
		if(childnode)
		{
			TiXmlNode* childnode1 = xml->enterNode(childnode, "internal");
			if(childnode1)
				channelCommon_.intReadBufferSize = KBE_MAX(0, xml->getValInt(childnode1));

			childnode1 = xml->enterNode(childnode, "external");
			if(childnode1)
				channelCommon_.extReadBufferSize = KBE_MAX(0, xml->getValInt(childnode1));
		}

		childnode = xml->enterNode(rootNode, "writeBufferSize");
		if(childnode)
		{
			TiXmlNode* childnode1 = xml->enterNode(childnode, "internal");
			if(childnode1)
				channelCommon_.intWriteBufferSize = KBE_MAX(0, xml->getValInt(childnode1));

			childnode1 = xml->enterNode(childnode, "external");
			if(childnode1)
				channelCommon_.extWriteBufferSize = KBE_MAX(0, xml->getValInt(childnode1));
		}

		childnode = xml->enterNode(rootNode, "windowOverflow");
		if(childnode)
		{
			TiXmlNode* sendNode = xml->enterNode(childnode, "send");
			if(sendNode)
			{
				TiXmlNode* childnode1 = xml->enterNode(sendNode, "messages");
				if(childnode1)
				{
					TiXmlNode* childnode2 = xml->enterNode(childnode1, "internal");
					if(childnode2)
						Network::g_intSendWindowMessagesOverflow = KBE_MAX(0, xml->getValInt(childnode2));

					childnode2 = xml->enterNode(childnode1, "external");
					if(childnode2)
						Network::g_extSendWindowMessagesOverflow = KBE_MAX(0, xml->getValInt(childnode2));

					childnode2 = xml->enterNode(childnode1, "critical");
					if(childnode2)
						Network::g_sendWindowMessagesOverflowCritical = KBE_MAX(0, xml->getValInt(childnode2));
				}

				childnode1 = xml->enterNode(sendNode, "bytes");
				if(childnode1)
				{
					TiXmlNode* childnode2 = xml->enterNode(childnode1, "internal");
					if(childnode2)
						Network::g_intSendWindowBytesOverflow = KBE_MAX(0, xml->getValInt(childnode2));
				
					childnode2 = xml->enterNode(childnode1, "external");
					if(childnode2)
						Network::g_extSendWindowBytesOverflow = KBE_MAX(0, xml->getValInt(childnode2));
				}

				childnode1 = xml->enterNode(sendNode, "tickSentBytes");
				if (childnode1)
				{
					TiXmlNode* childnode2 = xml->enterNode(childnode1, "internal");
					if (childnode2)
						Network::g_intSentWindowBytesOverflow = KBE_MAX(0, xml->getValInt(childnode2));

					childnode2 = xml->enterNode(childnode1, "external");
					if (childnode2)
						Network::g_extSentWindowBytesOverflow = KBE_MAX(0, xml->getValInt(childnode2));
				}
			}

			TiXmlNode* recvNode = xml->enterNode(childnode, "receive");
			if(recvNode)
			{
				TiXmlNode* childnode1 = xml->enterNode(recvNode, "messages");
				if(childnode1)
				{
					TiXmlNode* childnode2 = xml->enterNode(childnode1, "internal");
					if(childnode2)
						Network::g_intReceiveWindowMessagesOverflow = KBE_MAX(0, xml->getValInt(childnode2));

					childnode2 = xml->enterNode(childnode1, "external");
					if(childnode2)
						Network::g_extReceiveWindowMessagesOverflow = KBE_MAX(0, xml->getValInt(childnode2));

					childnode2 = xml->enterNode(childnode1, "critical");
					if(childnode2)
						Network::g_receiveWindowMessagesOverflowCritical = KBE_MAX(0, xml->getValInt(childnode2));
				}

				childnode1 = xml->enterNode(recvNode, "bytes");
				if(childnode1)
				{
					TiXmlNode* childnode2 = xml->enterNode(childnode1, "internal");
					if(childnode2)
						Network::g_intReceiveWindowBytesOverflow = KBE_MAX(0, xml->getValInt(childnode2));
				
					childnode2 = xml->enterNode(childnode1, "external");
					if(childnode2)
						Network::g_extReceiveWindowBytesOverflow = KBE_MAX(0, xml->getValInt(childnode2));
				}
			}
		};

		childnode = xml->enterNode(rootNode, "encrypt_type");
		if(childnode)
		{
			Network::g_channelExternalEncryptType = xml->getValInt(childnode);
		}

		childnode = xml->enterNode(rootNode, "sslCertificate");
		if (childnode)
		{
			Network::g_sslCertificate = xml->getValStr(childnode);
		}

		childnode = xml->enterNode(rootNode, "sslPrivateKey");
		if (childnode)
		{
			Network::g_sslPrivateKey = xml->getValStr(childnode);
		}

		TiXmlNode* rudpNode = xml->enterNode(rootNode, "reliableUDP");
		if (rudpNode)
		{
			childnode = xml->enterNode(rudpNode, "readPacketsQueueSize");
			if (childnode)
			{
				TiXmlNode* valueNode = xml->enterNode(childnode, "internal");
				if (valueNode)
					Network::g_rudp_intReadPacketsQueueSize = KBE_MAX(0, xml->getValInt(valueNode));

				valueNode = xml->enterNode(childnode, "external");
				if (valueNode)
					Network::g_rudp_extReadPacketsQueueSize = KBE_MAX(0, xml->getValInt(valueNode));
			}

			childnode = xml->enterNode(rudpNode, "writePacketsQueueSize");
			if (childnode)
			{
				TiXmlNode* valueNode = xml->enterNode(childnode, "internal");
				if (valueNode)
					Network::g_rudp_intWritePacketsQueueSize = KBE_MAX(0, xml->getValInt(valueNode));

				valueNode = xml->enterNode(childnode, "external");
				if (valueNode)
					Network::g_rudp_extWritePacketsQueueSize = KBE_MAX(0, xml->getValInt(valueNode));
			}

			childnode = xml->enterNode(rudpNode, "flushSegmentsBudget");
			if (childnode)
			{
				TiXmlNode* valueNode = xml->enterNode(childnode, "internal");
				if (valueNode)
					Network::g_rudp_intFlushSegmentsBudget = KBE_MAX(0, xml->getValInt(valueNode));

				valueNode = xml->enterNode(childnode, "external");
				if (valueNode)
					Network::g_rudp_extFlushSegmentsBudget = KBE_MAX(0, xml->getValInt(valueNode));
			}

			childnode = xml->enterNode(rudpNode, "writeQueueMaxBytes");
			if (childnode)
			{
				TiXmlNode* valueNode = xml->enterNode(childnode, "external");
				if (valueNode)
					Network::g_rudp_extWriteQueueMaxBytes = KBE_MAX(0, xml->getValInt(valueNode));
			}

			childnode = xml->enterNode(rudpNode, "volatileBackpressure");
			if (childnode)
			{
				TiXmlNode* valueNode = xml->enterNode(childnode, "highSegments");
				if (valueNode)
					Network::g_rudp_extVolatileBackpressureHighSegments = KBE_MAX(0, xml->getValInt(valueNode));

				valueNode = xml->enterNode(childnode, "lowSegments");
				if (valueNode)
					Network::g_rudp_extVolatileBackpressureLowSegments = KBE_MAX(0, xml->getValInt(valueNode));

				valueNode = xml->enterNode(childnode, "highBytes");
				if (valueNode)
					Network::g_rudp_extVolatileBackpressureHighBytes = KBE_MAX(0, xml->getValInt(valueNode));

				valueNode = xml->enterNode(childnode, "lowBytes");
				if (valueNode)
					Network::g_rudp_extVolatileBackpressureLowBytes = KBE_MAX(0, xml->getValInt(valueNode));

				// 低水位必须位于高水位内，才能形成稳定迟滞而不是每次采样反复切换。
				// The low watermark must stay within high so sampling cannot oscillate between states.
				if (Network::g_rudp_extVolatileBackpressureHighSegments > 0)
				{
					Network::g_rudp_extVolatileBackpressureLowSegments = KBE_MIN(
						Network::g_rudp_extVolatileBackpressureLowSegments,
						Network::g_rudp_extVolatileBackpressureHighSegments);
				}
				if (Network::g_rudp_extVolatileBackpressureHighBytes > 0)
				{
					Network::g_rudp_extVolatileBackpressureLowBytes = KBE_MIN(
						Network::g_rudp_extVolatileBackpressureLowBytes,
						Network::g_rudp_extVolatileBackpressureHighBytes);
				}
			}

			childnode = xml->enterNode(rudpNode, "tickInterval");
			if (childnode)
				Network::g_rudp_tickInterval = KBE_MAX(0, xml->getValInt(childnode));

			childnode = xml->enterNode(rudpNode, "minRTO");
			if (childnode)
				Network::g_rudp_minRTO = KBE_MAX(0, xml->getValInt(childnode));

			childnode = xml->enterNode(rudpNode, "missAcksResend");
			if (childnode)
				Network::g_rudp_missAcksResend = KBE_MAX(0, xml->getValInt(childnode));

			childnode = xml->enterNode(rudpNode, "mtu");
			if (childnode)
				Network::g_rudp_mtu = KBE_MAX(0, xml->getValInt(childnode));

			childnode = xml->enterNode(rudpNode, "congestionControl");
			if (childnode)
				Network::g_rudp_congestionControl = xml->getValStr(childnode) == "true";

			childnode = xml->enterNode(rudpNode, "nodelay");
			if (childnode)
				Network::g_rudp_nodelay = xml->getValStr(childnode) == "true";
		}
	}

	rootNode = xml->getRootNode("gameUpdateHertz");
	if(rootNode != NULL){
		gameUpdateHertz_ = xml->getValInt(rootNode);
	}

	rootNode = xml->getRootNode("asyncioRepeatOffset");
	if (rootNode != NULL)
	{
		// 零表示关闭实验性 asyncio；启用时最小间隔为 10ms，避免错误配置形成忙循环。
		// Zero disables experimental asyncio; when enabled, clamp to 10ms so an invalid value cannot create a busy loop.
		const float configuredOffset = float(xml->getValFloat(rootNode));
		asyncioRepeatOffset_ = configuredOffset <= 0.f ? 0.f : KBE_MAX(0.01f, configuredOffset);
	}

	rootNode = xml->getRootNode("bitsPerSecondToClient");
	if(rootNode != NULL){
		bitsPerSecondToClient_ = xml->getValInt(rootNode);
	}

	rootNode = xml->getRootNode("interfaces");
	if(rootNode != NULL)
	{
		TiXmlNode* childnode = xml->enterNode(rootNode, "entryScriptFile");	
		if(childnode != NULL)
			strncpy((char*)&_interfacesInfo.entryScriptFile, xml->getValStr(childnode).c_str(), MAX_NAME - 1);

		childnode = xml->enterNode(rootNode, "host");
		if (childnode)
		{
			interfacesAddress_ = xml->getValStr(childnode);
		}

		childnode = xml->enterNode(rootNode, "port_min");
		if (childnode)
		{
			interfacesPort_min_ = xml->getValInt(childnode);

			if (interfacesPort_min_ <= 0)
				interfacesPort_min_ = KBE_INTERFACES_TCP_PORT;
		}

		childnode = xml->enterNode(rootNode, "port_max");
		if (childnode)
		{
			interfacesPort_max_ = xml->getValInt(childnode);

			if (interfacesPort_max_ <= 0)
				interfacesPort_max_ = interfacesPort_min_;
		}

		node = xml->enterNode(rootNode, "SOMAXCONN");
		if(node != NULL){
			_interfacesInfo.tcp_SOMAXCONN = xml->getValInt(node);
		}

		node = xml->enterNode(rootNode, "orders_timeout");
		if(node != NULL){
			interfaces_orders_timeout_ = xml->getValInt(node);
		}
	
		node = xml->enterNode(rootNode, "telnet_service");
		if (node != NULL)
		{
			TiXmlNode* childnode = xml->enterNode(node, "port");
			if (childnode)
			{
				_interfacesInfo.telnet_port = xml->getValInt(childnode);
			}

			childnode = xml->enterNode(node, "password");
			if (childnode)
			{
				_interfacesInfo.telnet_passwd = xml->getValStr(childnode);
			}

			childnode = xml->enterNode(node, "default_layer");
			if (childnode)
			{
				_interfacesInfo.telnet_deflayer = xml->getValStr(childnode);
			}
		}
	}

	rootNode = xml->getRootNode("cellapp");
	if(rootNode != NULL)
	{
		// 黑名单位于 dbmgr 根节点而不是单个接口中，使同类型数据库接口共享一致的安全边界。
		// The blacklist lives at the dbmgr root so every interface of the same backend shares one security boundary.
		node = xml->enterNode(rootNode, "rawDatabaseCommandBlacklist");
		_dbmgrInfo.enableRawDatabaseCommandBlacklist = false;
		_dbmgrInfo.rawDatabaseCommandBlacklist.clear();
		if (node != NULL)
		{
			TiXmlNode* childnode = xml->enterNode(node, "enable");
			_dbmgrInfo.enableRawDatabaseCommandBlacklist =
				childnode != NULL && xml->getValStr(childnode) == "true";

			if (_dbmgrInfo.enableRawDatabaseCommandBlacklist)
			{
				loadRawDatabaseCommandBlacklist(xml.get(), node, "mysql", _dbmgrInfo.rawDatabaseCommandBlacklist);
				loadRawDatabaseCommandBlacklist(xml.get(), node, "mongodb", _dbmgrInfo.rawDatabaseCommandBlacklist);
				loadRawDatabaseCommandBlacklist(xml.get(), node, "postgresql", _dbmgrInfo.rawDatabaseCommandBlacklist);
			}
		}

		node = xml->enterNode(rootNode, "internalInterface");	
		if(node != NULL)
			strncpy((char*)&_cellAppInfo.internalInterface, xml->getValStr(node).c_str(), MAX_NAME - 1);

		node = xml->enterNode(rootNode, "entryScriptFile");	
		if(node != NULL)
			strncpy((char*)&_cellAppInfo.entryScriptFile, xml->getValStr(node).c_str(), MAX_NAME - 1);
		
		TiXmlNode* viewNode = xml->enterNode(rootNode, "defaultViewRadius");
		if(viewNode != NULL)
		{
			node = NULL;
			node = xml->enterNode(viewNode, "radius");
			if(node != NULL)
				_cellAppInfo.defaultViewRadius = float(xml->getValFloat(node));
					
			node = xml->enterNode(viewNode, "hysteresisArea");
			if(node != NULL)
				_cellAppInfo.defaultViewHysteresisArea = float(xml->getValFloat(node));
		}
			
		node = xml->enterNode(rootNode, "ids");
		if(node != NULL)
		{
			TiXmlNode* childnode = xml->enterNode(node, "criticallyLowSize");
			if(childnode)
			{
				_cellAppInfo.ids_criticallyLowSize = xml->getValInt(childnode);
				if (_cellAppInfo.ids_criticallyLowSize < 100)
					_cellAppInfo.ids_criticallyLowSize = 100;
			}
		}
		
		node = xml->enterNode(rootNode, "profiles");
		if(node != NULL)
		{
			TiXmlNode* childnode = xml->enterNode(node, "cprofile");
			if(childnode)
			{
				_cellAppInfo.profiles.open_cprofile = (xml->getValStr(childnode) == "true");
			}

			childnode = xml->enterNode(node, "pyprofile");
			if(childnode)
			{
				_cellAppInfo.profiles.open_pyprofile = (xml->getValStr(childnode) == "true");
			}

			childnode = xml->enterNode(node, "eventprofile");
			if(childnode)
			{
				_cellAppInfo.profiles.open_eventprofile = (xml->getValStr(childnode) == "true");
			}

			childnode = xml->enterNode(node, "networkprofile");
			if(childnode)
			{
				_cellAppInfo.profiles.open_networkprofile = (xml->getValStr(childnode) == "true");
			}
		}

		node = xml->enterNode(rootNode, "SOMAXCONN");
		if(node != NULL){
			_cellAppInfo.tcp_SOMAXCONN = xml->getValInt(node);
		}

		node = xml->enterNode(rootNode, "aliasEntityID");
		if(node != NULL){
			_cellAppInfo.aliasEntityID = (xml->getValStr(node) == "true");
		}

		node = xml->enterNode(rootNode, "entitydefAliasID");
		if(node != NULL){
			_cellAppInfo.entitydefAliasID = (xml->getValStr(node) == "true");
		}

		node = xml->enterNode(rootNode, "loadSmoothingBias");
		if(node != NULL)
			_cellAppInfo.loadSmoothingBias = float(xml->getValFloat(node));

		node = xml->enterNode(rootNode, "ghostDistance");
		if(node != NULL){
			_cellAppInfo.ghostDistance = (float)xml->getValFloat(node);
		}

		node = xml->enterNode(rootNode, "ghostingMaxPerCheck");
		if(node != NULL){
			_cellAppInfo.ghostingMaxPerCheck = xml->getValInt(node);
		}

		node = xml->enterNode(rootNode, "ghostUpdateHertz");
		if(node != NULL){
			_cellAppInfo.ghostUpdateHertz = xml->getValInt(node);
		}

		node = xml->enterNode(rootNode, "coordinate_system");
		if(node != NULL)
		{
			TiXmlNode* childnode = xml->enterNode(node, "enable");
			if(childnode)
			{
				_cellAppInfo.use_coordinate_system = (xml->getValStr(childnode) == "true");
			}
			
			childnode = xml->enterNode(node, "rangemgr_y");
			if(childnode)
			{
				_cellAppInfo.coordinateSystem_hasY = (xml->getValStr(childnode) == "true");
			}

			childnode = xml->enterNode(node, "entity_posdir_additional_updates");
			if(childnode)
			{
				_cellAppInfo.entity_posdir_additional_updates = xml->getValInt(childnode);
			}

			childnode = xml->enterNode(node, "witness_volatile_bytes_per_tick");
			if (childnode)
			{
				_cellAppInfo.witness_volatile_bytes_per_tick = xml->getValInt(childnode);
			}

			childnode = xml->enterNode(node, "witness_total_bytes_per_tick");
			if (childnode)
			{
				_cellAppInfo.witness_total_bytes_per_tick = xml->getValInt(childnode);
			}

			childnode = xml->enterNode(node, "witness_global_bytes_per_tick");
			if (childnode)
			{
				_cellAppInfo.witness_global_bytes_per_tick = xml->getValInt(childnode);
			}

			childnode = xml->enterNode(node, "witness_global_updates_per_tick");
			if (childnode)
			{
				_cellAppInfo.witness_global_updates_per_tick = xml->getValInt(childnode);
			}

			childnode = xml->enterNode(node, "witness_volatile_lod");
			if (childnode)
			{
				TiXmlNode* lodNode = xml->enterNode(childnode, "enable");
				if (lodNode)
					_cellAppInfo.witness_volatile_lod_enabled = (xml->getValStr(lodNode) == "true");

				lodNode = xml->enterNode(childnode, "minimumViewEntities");
				if (lodNode)
					_cellAppInfo.witness_volatile_lod_minimum_view_entities = KBE_MAX(1, xml->getValInt(lodNode));

				lodNode = xml->enterNode(childnode, "nearDistance");
				if (lodNode)
					_cellAppInfo.witness_volatile_lod_near_distance = KBE_MAX(0.f, float(xml->getValFloat(lodNode)));

				lodNode = xml->enterNode(childnode, "mediumDistance");
				if (lodNode)
					_cellAppInfo.witness_volatile_lod_medium_distance = KBE_MAX(
						_cellAppInfo.witness_volatile_lod_near_distance, float(xml->getValFloat(lodNode)));

				lodNode = xml->enterNode(childnode, "mediumIntervalTicks");
				if (lodNode)
					_cellAppInfo.witness_volatile_lod_medium_interval_ticks = KBE_MIN(64, KBE_MAX(1, xml->getValInt(lodNode)));

				lodNode = xml->enterNode(childnode, "farIntervalTicks");
				if (lodNode)
					_cellAppInfo.witness_volatile_lod_far_interval_ticks = KBE_MIN(64, KBE_MAX(1, xml->getValInt(lodNode)));
			}

			childnode = xml->enterNode(node, "entity_posdir_updates");
			if (childnode)
			{
				TiXmlNode* node = xml->enterNode(childnode, "type");
				if (node)
					_cellAppInfo.entity_posdir_updates_type = xml->getValInt(node);

				node = xml->enterNode(childnode, "smartThreshold");
				if (node)
					_cellAppInfo.entity_posdir_updates_smart_threshold = xml->getValInt(node);
			}
		}

		node = xml->enterNode(rootNode, "telnet_service");
		if(node != NULL)
		{
			TiXmlNode* childnode = xml->enterNode(node, "port");
			if(childnode)
			{
				_cellAppInfo.telnet_port = xml->getValInt(childnode);
			}

			childnode = xml->enterNode(node, "password");
			if(childnode)
			{
				_cellAppInfo.telnet_passwd = xml->getValStr(childnode);
			}

			childnode = xml->enterNode(node, "default_layer");
			if(childnode)
			{
				_cellAppInfo.telnet_deflayer = xml->getValStr(childnode);
			}
		}

		node = xml->enterNode(rootNode, "shutdown");
		if(node != NULL)
		{
			TiXmlNode* childnode = xml->enterNode(node, "perSecsDestroyEntitySize");
			if(childnode)
			{
				_cellAppInfo.perSecsDestroyEntitySize = uint32(xml->getValInt(childnode));
			}
		}

		node = xml->enterNode(rootNode, "witness");
		if(node != NULL)
		{
			TiXmlNode* childnode = xml->enterNode(node, "timeout");
			if(childnode)
			{
				_cellAppInfo.witness_timeout = uint16(xml->getValInt(childnode));
			}
		}
	}
	
	rootNode = xml->getRootNode("baseapp");
	if(rootNode != NULL)
	{
		node = xml->enterNode(rootNode, "entryScriptFile");	
		if(node != NULL)
			strncpy((char*)&_baseAppInfo.entryScriptFile, xml->getValStr(node).c_str(), MAX_NAME - 1);

		node = xml->enterNode(rootNode, "internalInterface");	
		if(node != NULL)
			strncpy((char*)&_baseAppInfo.internalInterface, xml->getValStr(node).c_str(), MAX_NAME - 1);

		node = xml->enterNode(rootNode, "externalInterface");	
		if(node != NULL)
			strncpy((char*)&_baseAppInfo.externalInterface, xml->getValStr(node).c_str(), MAX_NAME - 1);

		node = xml->enterNode(rootNode, "externalAddress");	
		if(node != NULL)
			strncpy((char*)&_baseAppInfo.externalAddress, xml->getValStr(node).c_str(), MAX_NAME - 1);

		node = xml->enterNode(rootNode, "externalTcpPorts_min");
		if(node != NULL)	
			_baseAppInfo.externalTcpPorts_min = xml->getValInt(node);

		node = xml->enterNode(rootNode, "externalTcpPorts_max");
		if(node != NULL)	
			_baseAppInfo.externalTcpPorts_max = xml->getValInt(node);

		if(_baseAppInfo.externalTcpPorts_min < 0)
			_baseAppInfo.externalTcpPorts_min = 0;
		if(_baseAppInfo.externalTcpPorts_max < _baseAppInfo.externalTcpPorts_min)
			_baseAppInfo.externalTcpPorts_max = _baseAppInfo.externalTcpPorts_min;

		node = xml->enterNode(rootNode, "externalUdpPorts_min");
		if (node != NULL)
			_baseAppInfo.externalUdpPorts_min = xml->getValInt(node);

		node = xml->enterNode(rootNode, "externalUdpPorts_max");
		if (node != NULL)
			_baseAppInfo.externalUdpPorts_max = xml->getValInt(node);

		if (_baseAppInfo.externalUdpPorts_min < 0)
			_baseAppInfo.externalUdpPorts_min = -1;
		if (_baseAppInfo.externalUdpPorts_max < _baseAppInfo.externalUdpPorts_min)
			_baseAppInfo.externalUdpPorts_max = _baseAppInfo.externalUdpPorts_min;

		node = xml->enterNode(rootNode, "archivePeriod");
		if(node != NULL)
			_baseAppInfo.archivePeriod = float(xml->getValFloat(node));
				
		node = xml->enterNode(rootNode, "backupPeriod");
		if(node != NULL)
			_baseAppInfo.backupPeriod = float(xml->getValFloat(node));
		
		node = xml->enterNode(rootNode, "backUpUndefinedProperties");
		if(node != NULL)
			_baseAppInfo.backUpUndefinedProperties = xml->getValInt(node) > 0;
			
		node = xml->enterNode(rootNode, "loadSmoothingBias");
		if(node != NULL)
			_baseAppInfo.loadSmoothingBias = float(xml->getValFloat(node));
		
		node = xml->enterNode(rootNode, "ids");
		if(node != NULL)
		{
			TiXmlNode* childnode = xml->enterNode(node, "criticallyLowSize");
			if(childnode)
			{
				_baseAppInfo.ids_criticallyLowSize = xml->getValInt(childnode);
				if (_baseAppInfo.ids_criticallyLowSize < 100)
					_baseAppInfo.ids_criticallyLowSize = 100;
			}
		}
		
		node = xml->enterNode(rootNode, "downloadStreaming");
		if(node != NULL)
		{
			TiXmlNode* childnode = xml->enterNode(node, "bitsPerSecondTotal");
			if(childnode)
				_baseAppInfo.downloadBitsPerSecondTotal = xml->getValInt(childnode);

			childnode = xml->enterNode(node, "bitsPerSecondPerClient");
			if(childnode)
				_baseAppInfo.downloadBitsPerSecondPerClient = xml->getValInt(childnode);
		}
	
		node = xml->enterNode(rootNode, "profiles");
		if(node != NULL)
		{
			TiXmlNode* childnode = xml->enterNode(node, "cprofile");
			if(childnode)
			{
				_baseAppInfo.profiles.open_cprofile = (xml->getValStr(childnode) == "true");
			}

			childnode = xml->enterNode(node, "pyprofile");
			if(childnode)
			{
				_baseAppInfo.profiles.open_pyprofile = (xml->getValStr(childnode) == "true");
			}

			childnode = xml->enterNode(node, "eventprofile");
			if(childnode)
			{
				_baseAppInfo.profiles.open_eventprofile = (xml->getValStr(childnode) == "true");
			}

			childnode = xml->enterNode(node, "networkprofile");
			if(childnode)
			{
				_baseAppInfo.profiles.open_networkprofile = (xml->getValStr(childnode) == "true");
			}
		}

		node = xml->enterNode(rootNode, "SOMAXCONN");
		if(node != NULL){
			_baseAppInfo.tcp_SOMAXCONN = xml->getValInt(node);
		}

		node = xml->enterNode(rootNode, "entityRestoreSize");
		if(node != NULL){
			_baseAppInfo.entityRestoreSize = xml->getValInt(node);
		}
		
		if(_baseAppInfo.entityRestoreSize <= 0)
			_baseAppInfo.entityRestoreSize = 32;

		node = xml->enterNode(rootNode, "telnet_service");
		if(node != NULL)
		{
			TiXmlNode* childnode = xml->enterNode(node, "port");
			if(childnode)
			{
				_baseAppInfo.telnet_port = xml->getValInt(childnode);
			}

			childnode = xml->enterNode(node, "password");
			if(childnode)
			{
				_baseAppInfo.telnet_passwd = xml->getValStr(childnode);
			}

			childnode = xml->enterNode(node, "default_layer");
			if(childnode)
			{
				_baseAppInfo.telnet_deflayer = xml->getValStr(childnode);
			}
		}

		node = xml->enterNode(rootNode, "shutdown");
		if(node != NULL)
		{
			TiXmlNode* childnode = xml->enterNode(node, "perSecsDestroyEntitySize");
			if(childnode)
			{
				_baseAppInfo.perSecsDestroyEntitySize = uint32(xml->getValInt(childnode));
			}
		}

		node = xml->enterNode(rootNode, "respool");
		if(node != NULL)
		{
			TiXmlNode* childnode = xml->enterNode(node, "buffer_size");
			if(childnode)
				_baseAppInfo.respool_buffersize = xml->getValInt(childnode);

			childnode = xml->enterNode(node, "timeout");
			if(childnode)
				_baseAppInfo.respool_timeout = uint64(xml->getValInt(childnode));

			childnode = xml->enterNode(node, "checktick");
			if(childnode)
				Resmgr::respool_checktick = xml->getValInt(childnode);

			Resmgr::respool_timeout = _baseAppInfo.respool_timeout;
			Resmgr::respool_buffersize = _baseAppInfo.respool_buffersize;
		}
	}

	rootNode = xml->getRootNode("dbmgr");
	if(rootNode != NULL)
	{
		node = xml->enterNode(rootNode, "entryScriptFile");
		if (node != NULL)
			strncpy((char*)&_dbmgrInfo.entryScriptFile, xml->getValStr(node).c_str(), MAX_NAME - 1);
		
		node = xml->enterNode(rootNode, "telnet_service");
		if (node != NULL)
		{
			TiXmlNode* childnode = xml->enterNode(node, "port");
			if (childnode)
			{
				_dbmgrInfo.telnet_port = xml->getValInt(childnode);
			}

			childnode = xml->enterNode(node, "password");
			if (childnode)
			{
				_dbmgrInfo.telnet_passwd = xml->getValStr(childnode);
			}

			childnode = xml->enterNode(node, "default_layer");
			if (childnode)
			{
				_dbmgrInfo.telnet_deflayer = xml->getValStr(childnode);
			}
		}

		node = xml->enterNode(rootNode, "ids");
		if (node != NULL)
		{
			TiXmlNode* childnode = xml->enterNode(node, "increasing_range");
			if (childnode)
			{
				_dbmgrInfo.ids_increasing_range = xml->getValInt(childnode);
			}
		}

		node = xml->enterNode(rootNode, "InterfacesServiceAddr");
		if (node != NULL)
		{
			TiXmlNode* loopNode = node;

			do
			{
				if (TiXmlNode::TINYXML_COMMENT == loopNode->Type())
					continue;

				std::string name = loopNode->Value();
				name = strutil::kbe_trim(name);

				if (name == "item")
				{
					if (loopNode->FirstChild() != NULL)
					{
						TiXmlNode* host_node = xml->enterNode(loopNode->FirstChild(), "host");
						TiXmlNode* port_node = xml->enterNode(loopNode->FirstChild(), "port");
						if (host_node && port_node)
						{
							std::string ip = xml->getValStr(host_node);
							int port = xml->getValInt(port_node);

							if (port <= 0)
								port = KBE_INTERFACES_TCP_PORT;

							Network::Address addr(ip, port);
							interfacesAddrs_.push_back(addr);
						}
					}
				}
			} while ((loopNode = loopNode->NextSibling()));

			TiXmlNode* childnode = xml->enterNode(node, "addDefaultAddress");
			if (childnode)
			{
				g_dbmgr_addDefaultAddress = xml->getValStr(childnode) == "true";
			}

			childnode = xml->enterNode(node, "enable");
			if (childnode)
			{
				if (xml->getValStr(childnode) != "true")
				{
					interfacesAddrs_.clear();
					g_dbmgr_addDefaultAddress = false;
				}
			}
		}

		node = xml->enterNode(rootNode, "internalInterface");	
		if(node != NULL)
			strncpy((char*)&_dbmgrInfo.internalInterface, xml->getValStr(node).c_str(), MAX_NAME - 1);

		TiXmlNode* databaseInterfacesNode = xml->enterNode(rootNode, "databaseInterfaces");	
		if(databaseInterfacesNode != NULL)
		{
			if (databaseInterfacesNode->FirstChild() != NULL)
			{
				do
				{
					if (TiXmlNode::TINYXML_COMMENT == databaseInterfacesNode->Type())
						continue;
					
					std::vector<std::string> missingFields;
					missingFields.clear();

					std::string name = databaseInterfacesNode->Value();

					DBInterfaceInfo dbinfo;
					DBInterfaceInfo* pDBInfo = dbInterface(name);
					if (!pDBInfo)
						pDBInfo = &dbinfo;
					
					strncpy((char*)&pDBInfo->name, name.c_str(), MAX_NAME - 1);

					TiXmlNode* interfaceNode = databaseInterfacesNode->FirstChild();
					
					node = xml->enterNode(interfaceNode, "pure");
					if (node)
						pDBInfo->isPure = xml->getValStr(node) == "true";
					else
						missingFields.push_back("pure");

					// 默认库不允许是纯净库，引擎需要创建实体表
					if (name == "default")
						pDBInfo->isPure = false;

					node = xml->enterNode(interfaceNode, "type");
					if(node != NULL)
						strncpy((char*)&pDBInfo->db_type, xml->getValStr(node).c_str(), MAX_NAME - 1);
					else
						missingFields.push_back("type");

					node = xml->enterNode(interfaceNode, "host");
					if(node != NULL)
						strncpy((char*)&pDBInfo->db_ip, xml->getValStr(node).c_str(), MAX_IP - 1);
					else
						missingFields.push_back("host");

					node = xml->enterNode(interfaceNode, "port");
					if(node != NULL)
						pDBInfo->db_port = xml->getValInt(node);
					else
						missingFields.push_back("port");

					// DBID策略允许在用户配置中覆盖默认值；未知值必须在启动期失败，避免各后端静默采用不同语义。
					// The DBID policy may override defaults, but unknown values fail at startup so backends never silently diverge.
					node = xml->enterNode(interfaceNode, "idType");
					if (node != NULL)
					{
						const std::string idType = strutil::kbe_trim(xml->getValStr(node));
						if (kbe_stricmp(idType.c_str(), "Default") == 0)
							pDBInfo->db_idType = DBInterfaceInfo::DBID_TYPE_DEFAULT;
						else if (kbe_stricmp(idType.c_str(), "UUID64") == 0)
							pDBInfo->db_idType = DBInterfaceInfo::DBID_TYPE_UUID64;
						else
						{
							ERROR_MSG(fmt::format(
								"ServerConfig::loadConfig: databaseInterface({}) has invalid idType({}), expected Default or UUID64, file={}!\n",
								name, idType, fileName));
							return false;
						}
					}

					node = xml->enterNode(interfaceNode, "autoIncrementInit");
					if (node != NULL)
					{
						const std::string value = strutil::kbe_trim(xml->getValStr(node));
						if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
						{
							ERROR_MSG(fmt::format(
								"ServerConfig::loadConfig: databaseInterface({}) has invalid autoIncrementInit({}), expected a positive integer, file={}!\n",
								name, value, fileName));
							return false;
						}

						errno = 0;
						char* end = NULL;
						const uint64 autoIncrementInit = strtoull(value.c_str(), &end, 10);
						if (errno == ERANGE || end == NULL || *end != '\0' || autoIncrementInit == 0)
						{
							ERROR_MSG(fmt::format(
								"ServerConfig::loadConfig: databaseInterface({}) has invalid autoIncrementInit({}), expected a positive integer, file={}!\n",
								name, value, fileName));
							return false;
						}

						pDBInfo->db_autoIncrementInit = autoIncrementInit;
					}

					// replicaSet 是可选的 MongoDB 拓扑配置；指定后驱动会从种子节点发现并切换主节点。
					// replicaSet is optional MongoDB topology configuration; when set, the driver discovers members and follows primary changes from the seed node.
					node = xml->enterNode(interfaceNode, "replicaSet");
					if (node != NULL)
						strncpy((char*)&pDBInfo->db_replicaSet, xml->getValStr(node).c_str(), MAX_NAME - 1);

					node = xml->enterNode(interfaceNode, "auth");
					if(node != NULL)
					{
						TiXmlNode* childnode = xml->enterNode(node, "password");
						if(childnode)
						{
							strncpy((char*)&pDBInfo->db_password, xml->getValStr(childnode).c_str(), MAX_BUF * 10 - 1);
						}
						else
						{
							missingFields.push_back("auth->password");
						}

						childnode = xml->enterNode(node, "username");
						if(childnode)
						{
							strncpy((char*)&pDBInfo->db_username, xml->getValStr(childnode).c_str(), MAX_NAME - 1);
						}
						else
						{
							missingFields.push_back("auth->username");
						}

						// authSource 仅供 MongoDB 指定独立认证数据库；省略时由驱动适配层回退到业务数据库。
						// authSource lets MongoDB use a separate authentication database; omission falls back to the application database in the driver adapter.
						childnode = xml->enterNode(node, "authSource");
						if(childnode)
						{
							strncpy((char*)&pDBInfo->db_authSource, xml->getValStr(childnode).c_str(), MAX_NAME - 1);
						}

						childnode = xml->enterNode(node, "encrypt");
						if(childnode)
						{
							pDBInfo->db_passwordEncrypt = xml->getValStr(childnode) == "true";
						}
						else
						{
							missingFields.push_back("auth->encrypt");
						}

						// MySQL TLS块是可选配置，省略时保持现有明文连接行为。
						// The MySQL TLS block is optional; omission preserves the existing plaintext connection behavior.
						childnode = xml->enterNode(node, "MySQL");
						if (childnode)
						{
							TiXmlNode* mysqlNode = xml->enterNode(childnode, "ssl");
							if (mysqlNode)
								pDBInfo->db_mysqlTLS.enabled = xml->getValStr(mysqlNode) == "true";

							mysqlNode = xml->enterNode(childnode, "sslVerifyServerCert");
							if (mysqlNode)
								pDBInfo->db_mysqlTLS.verifyServerCert = xml->getValStr(mysqlNode) == "true";

							mysqlNode = xml->enterNode(childnode, "sslCa");
							if (mysqlNode)
								pDBInfo->db_mysqlTLS.caPath = xml->getValStr(mysqlNode);

							mysqlNode = xml->enterNode(childnode, "sslCert");
							if (mysqlNode)
								pDBInfo->db_mysqlTLS.clientCertPath = xml->getValStr(mysqlNode);

							mysqlNode = xml->enterNode(childnode, "sslKey");
							if (mysqlNode)
								pDBInfo->db_mysqlTLS.clientKeyPath = xml->getValStr(mysqlNode);
						}
					}
					else
					{
						missingFields.push_back("auth");
					}
						
					node = xml->enterNode(interfaceNode, "databaseName");
					if(node != NULL)
						strncpy((char*)&pDBInfo->db_name, xml->getValStr(node).c_str(), MAX_NAME - 1);
					else
						missingFields.push_back("databaseName");

					node = xml->enterNode(interfaceNode, "numConnections");
					if(node != NULL)
						pDBInfo->db_numConnections = xml->getValInt(node);
					else
						missingFields.push_back("numConnections");
						
					node = xml->enterNode(interfaceNode, "unicodeString");
					if(node != NULL)
					{
						TiXmlNode* childnode = xml->enterNode(node, "characterSet");
						if(childnode)
						{
							pDBInfo->db_unicodeString_characterSet = xml->getValStr(childnode);
						}
						else
						{
							missingFields.push_back("unicodeString->characterSet");
						}

						childnode = xml->enterNode(node, "collation");
						if(childnode)
						{
							pDBInfo->db_unicodeString_collation = xml->getValStr(childnode);
						}
						else
						{
							missingFields.push_back("unicodeString->collation");
						}
					}
					else
					{
						missingFields.push_back("unicodeString");
					}

					if (pDBInfo->db_unicodeString_characterSet.size() == 0)
						pDBInfo->db_unicodeString_characterSet = "utf8";

					if (pDBInfo->db_unicodeString_collation.size() == 0)
						pDBInfo->db_unicodeString_collation = "utf8_bin";
	
					if (pDBInfo == &dbinfo)
					{
						// 检查不能在不同的接口中使用相同的数据库与相同的表
						std::vector<DBInterfaceInfo>::iterator dbinfo_iter = _dbmgrInfo.dbInterfaceInfos.begin();
						for (; dbinfo_iter != _dbmgrInfo.dbInterfaceInfos.end(); ++dbinfo_iter)
						{
							if (kbe_stricmp((*dbinfo_iter).db_ip, dbinfo.db_ip) == 0 && 
								kbe_stricmp((*dbinfo_iter).db_type, dbinfo.db_type) == 0 &&
								(*dbinfo_iter).db_port == dbinfo.db_port &&
								strcmp(dbinfo.db_name, (*dbinfo_iter).db_name) == 0)
							{
								ERROR_MSG(fmt::format("ServerConfig::loadConfig: databaseInterfaces, Conflict between \"{}=(databaseName={})\" and \"{}=(databaseName={})\", file={}!\n",
									(*dbinfo_iter).name, (*dbinfo_iter).db_name, dbinfo.name, dbinfo.db_name, fileName.c_str()));

								return false;
							}
						}

						if (fileName == "server/kbengine_defaults.xml" && !missingFields.empty())
						{
							std::vector<std::string>::const_iterator iter = missingFields.begin();
							for (; iter != missingFields.end(); iter++)
							{
								ERROR_MSG(fmt::format("ServerConfig::loadConfig: kbengine_defaults.xml error, databaseInterface({}) missing filed:{}.\n", name, *iter));
							}

							return false;
						}

						_dbmgrInfo.dbInterfaceInfos.push_back(dbinfo);
					}

				} while ((databaseInterfacesNode = databaseInterfacesNode->NextSibling()));
			}
		}

		node = xml->enterNode(rootNode, "SOMAXCONN");
		if(node != NULL){
			_dbmgrInfo.tcp_SOMAXCONN = xml->getValInt(node);
		}
		
		node = xml->enterNode(rootNode, "debug");
		if(node != NULL){
			_dbmgrInfo.debugDBMgr = (xml->getValStr(node) == "true");
		}

		node = xml->enterNode(rootNode, "allowEmptyDigest");
		if(node != NULL){
			_dbmgrInfo.allowEmptyDigest = (xml->getValStr(node) == "true");
		}

		node = xml->enterNode(rootNode, "shareDB");
		if (node != NULL) {
			_dbmgrInfo.isShareDB = (xml->getValStr(node) == "true");
		}

		node = xml->enterNode(rootNode, "account_system");
		if(node != NULL)
		{
			TiXmlNode* childnode = xml->enterNode(node, "accountDefaultFlags");
			if(childnode)
			{
				_dbmgrInfo.accountDefaultFlags = xml->getValInt(childnode);
			}

			childnode = xml->enterNode(node, "accountDefaultDeadline");	
			if(childnode != NULL)
			{
				_dbmgrInfo.accountDefaultDeadline = xml->getValInt(childnode);
			}

			childnode = xml->enterNode(node, "accountEntityScriptType");	
			if(childnode != NULL)
			{
				strncpy((char*)&_dbmgrInfo.dbAccountEntityScriptType, xml->getValStr(childnode).c_str(), MAX_NAME - 1);
			}

			childnode = xml->enterNode(node, "account_registration");	
			if(childnode != NULL)
			{
				TiXmlNode* childchildnode = xml->enterNode(childnode, "enable");
				if(childchildnode)
				{
					_dbmgrInfo.account_registration_enable = (xml->getValStr(childchildnode) == "true");
				}

				childchildnode = xml->enterNode(childnode, "loginAutoCreate");
				if(childchildnode != NULL){
					_dbmgrInfo.notFoundAccountAutoCreate = (xml->getValStr(childchildnode) == "true");
				}
			} 

			childnode = xml->enterNode(node, "account_resetPassword");
			if (childnode != NULL)
			{
				TiXmlNode* childchildnode = xml->enterNode(childnode, "enable");
				if (childchildnode)
				{
					_dbmgrInfo.account_reset_password_enable = (xml->getValStr(childchildnode) == "true");
				}
			}
		}
	}

	rootNode = xml->getRootNode("loginapp");
	if(rootNode != NULL)
	{
		node = xml->enterNode(rootNode, "entryScriptFile");
		if (node != NULL)
			strncpy((char*)&_loginAppInfo.entryScriptFile, xml->getValStr(node).c_str(), MAX_NAME - 1);
		
		node = xml->enterNode(rootNode, "telnet_service");
		if (node != NULL)
		{
			TiXmlNode* childnode = xml->enterNode(node, "port");
			if (childnode)
			{
				_loginAppInfo.telnet_port = xml->getValInt(childnode);
			}

			childnode = xml->enterNode(node, "password");
			if (childnode)
			{
				_loginAppInfo.telnet_passwd = xml->getValStr(childnode);
			}

			childnode = xml->enterNode(node, "default_layer");
			if (childnode)
			{
				_loginAppInfo.telnet_deflayer = xml->getValStr(childnode);
			}
		}

		node = xml->enterNode(rootNode, "internalInterface");	
		if(node != NULL)
			strncpy((char*)&_loginAppInfo.internalInterface, xml->getValStr(node).c_str(), MAX_NAME - 1);

		node = xml->enterNode(rootNode, "externalInterface");	
		if(node != NULL)
			strncpy((char*)&_loginAppInfo.externalInterface, xml->getValStr(node).c_str(), MAX_NAME - 1);

		node = xml->enterNode(rootNode, "externalAddress");	
		if(node != NULL)
			strncpy((char*)&_loginAppInfo.externalAddress, xml->getValStr(node).c_str(), MAX_NAME - 1);

		node = xml->enterNode(rootNode, "externalTcpPorts_min");
		if(node != NULL)	
			_loginAppInfo.externalTcpPorts_min = xml->getValInt(node);

		node = xml->enterNode(rootNode, "externalTcpPorts_max");
		if(node != NULL)	
			_loginAppInfo.externalTcpPorts_max = xml->getValInt(node);

		if(_loginAppInfo.externalTcpPorts_min < 0)
			_loginAppInfo.externalTcpPorts_min = 0;
		if(_loginAppInfo.externalTcpPorts_max < _loginAppInfo.externalTcpPorts_min)
			_loginAppInfo.externalTcpPorts_max = _loginAppInfo.externalTcpPorts_min;

		node = xml->enterNode(rootNode, "externalUdpPorts_min");
		if (node != NULL)
			_loginAppInfo.externalUdpPorts_min = xml->getValInt(node);

		node = xml->enterNode(rootNode, "externalUdpPorts_max");
		if (node != NULL)
			_loginAppInfo.externalUdpPorts_max = xml->getValInt(node);

		if (_loginAppInfo.externalUdpPorts_min < 0)
			_loginAppInfo.externalUdpPorts_min = -1;
		if (_loginAppInfo.externalUdpPorts_max < _loginAppInfo.externalUdpPorts_min)
			_loginAppInfo.externalUdpPorts_max = _loginAppInfo.externalUdpPorts_min;

		node = xml->enterNode(rootNode, "SOMAXCONN");
		if(node != NULL){
			_loginAppInfo.tcp_SOMAXCONN = xml->getValInt(node);
		}

		node = xml->enterNode(rootNode, "encrypt_login");
		if(node != NULL){
			_loginAppInfo.encrypt_login = xml->getValInt(node);
		}

		node = xml->enterNode(rootNode, "account_type");
		if(node != NULL){
			_loginAppInfo.account_type = xml->getValInt(node);
		}

		node = xml->enterNode(rootNode, "http_cbhost");
		if(node)
			_loginAppInfo.http_cbhost = xml->getValStr(node);

		node = xml->enterNode(rootNode, "http_cbport");
		if(node)
			_loginAppInfo.http_cbport = xml->getValInt(node);
	}
	
	rootNode = xml->getRootNode("cellappmgr");
	if(rootNode != NULL)
	{
		node = xml->enterNode(rootNode, "internalInterface");	
		if(node != NULL)
			strncpy((char*)&_cellAppMgrInfo.internalInterface, xml->getValStr(node).c_str(), MAX_NAME - 1);

		node = xml->enterNode(rootNode, "SOMAXCONN");
		if(node != NULL){
			_cellAppMgrInfo.tcp_SOMAXCONN = xml->getValInt(node);
		}

		node = xml->enterNode(rootNode, "spaceAllocationMaxSkew");
		if (node != NULL)
			_cellAppMgrInfo.cellappmgr_space_assignment_max_skew = xml->getValInt(node);

		node = xml->enterNode(rootNode, "witnessPendingPressureWeight");
		if (node != NULL)
			_cellAppMgrInfo.cellappmgr_witness_pending_pressure_weight =
				std::max(0.f, static_cast<float>(xml->getValFloat(node)));
	}
	
	rootNode = xml->getRootNode("baseappmgr");
	if(rootNode != NULL)
	{
		node = xml->enterNode(rootNode, "internalInterface");	
		if(node != NULL)
			strncpy((char*)&_baseAppMgrInfo.internalInterface, xml->getValStr(node).c_str(), MAX_NAME - 1);

		node = xml->enterNode(rootNode, "SOMAXCONN");
		if(node != NULL){
			_baseAppMgrInfo.tcp_SOMAXCONN = xml->getValInt(node);
		}
	}
	
	rootNode = xml->getRootNode("machine");
	if(rootNode != NULL)
	{
		node = xml->enterNode(rootNode, "internalInterface");	
		if(node != NULL)
			strncpy((char*)&_kbMachineInfo.internalInterface, xml->getValStr(node).c_str(), MAX_NAME - 1);

		node = xml->enterNode(rootNode, "externalInterface");	
		if(node != NULL)
			strncpy((char*)&_kbMachineInfo.externalInterface, xml->getValStr(node).c_str(), MAX_NAME - 1);

		node = xml->enterNode(rootNode, "externalTcpPorts_min");
		if(node != NULL)	
			_kbMachineInfo.externalTcpPorts_min = xml->getValInt(node);

		node = xml->enterNode(rootNode, "externalTcpPorts_max");
		if(node != NULL)	
			_kbMachineInfo.externalTcpPorts_max = xml->getValInt(node);

		if(_kbMachineInfo.externalTcpPorts_min < 0)
			_kbMachineInfo.externalTcpPorts_min = 0;
		if(_kbMachineInfo.externalTcpPorts_max < _kbMachineInfo.externalTcpPorts_min)
			_kbMachineInfo.externalTcpPorts_max = _kbMachineInfo.externalTcpPorts_min;

		node = xml->enterNode(rootNode, "SOMAXCONN");
		if(node != NULL){
			_kbMachineInfo.tcp_SOMAXCONN = xml->getValInt(node);
		}
		
		node = xml->enterNode(rootNode, "addresses");
		if(node)
		{
			do
			{
				if (TiXmlNode::TINYXML_COMMENT == node->Type())
					continue;

				if(node->FirstChild() != NULL)
				{
					std::string c = node->FirstChild()->Value();
					c = strutil::kbe_trim(c);
					if(c.size() > 0)
					{
						_kbMachineInfo.machine_addresses.push_back(c);
					}
				}
			} while((node = node->NextSibling()));
		}
	}
	
	rootNode = xml->getRootNode("bots");
	if(rootNode != NULL)
	{
		node = xml->enterNode(rootNode, "entryScriptFile");	
		if(node != NULL)
			strncpy((char*)&_botsInfo.entryScriptFile, xml->getValStr(node).c_str(), MAX_NAME - 1);

		node = xml->enterNode(rootNode, "internalInterface");	
		if(node != NULL)
			strncpy((char*)&_botsInfo.internalInterface, xml->getValStr(node).c_str(), MAX_NAME - 1);

		node = xml->enterNode(rootNode, "host");	
		if(node != NULL)
			strncpy((char*)&_botsInfo.login_ip, xml->getValStr(node).c_str(), MAX_IP - 1);

		node = xml->enterNode(rootNode, "port_min");	
		if(node != NULL)
			_botsInfo.login_port_min = xml->getValInt(node);

		node = xml->enterNode(rootNode, "port_max");
		if (node != NULL)
			_botsInfo.login_port_max = xml->getValInt(node);

		if (_botsInfo.login_port_min < 0)
			_botsInfo.login_port_min = 0;
			
		if (_botsInfo.login_port_max < _botsInfo.login_port_min)
			_botsInfo.login_port_max = _botsInfo.login_port_min;
		
		_botsInfo.login_port = _botsInfo.login_port_min;

		node = xml->enterNode(rootNode, "isOnInitCallPropertysSetMethods");
		if (node != NULL)
			_botsInfo.isOnInitCallPropertysSetMethods = xml->getValInt(node);

		node = xml->enterNode(rootNode, "defaultAddBots");
		if(node != NULL)
		{
			TiXmlNode* childnode = xml->enterNode(node, "totalCount");
			if(childnode)
			{
				_botsInfo.defaultAddBots_totalCount = xml->getValInt(childnode);
			}

			childnode = xml->enterNode(node, "tickCount");
			if(childnode)
			{
				_botsInfo.defaultAddBots_tickCount = xml->getValInt(childnode);
			}

			childnode = xml->enterNode(node, "tickTime");
			if(childnode)
			{
				_botsInfo.defaultAddBots_tickTime = (float)xml->getValFloat(childnode);
			}
		}

		node = xml->enterNode(rootNode, "account_infos");
		if(node != NULL)
		{
			TiXmlNode* childnode = xml->enterNode(node, "account_name_prefix");
			if(childnode)
			{
				_botsInfo.bots_account_name_prefix = xml->getValStr(childnode);
			}

			childnode = xml->enterNode(node, "account_name_suffix_inc");
			if(childnode)
			{
				_botsInfo.bots_account_name_suffix_inc = xml->getValInt(childnode);
			}

			childnode = xml->enterNode(node, "account_password");
			if (childnode)
			{
				_botsInfo.bots_account_passwd = xml->getValStr(childnode);
			}
		}

		node = xml->enterNode(rootNode, "SOMAXCONN");
		if(node != NULL){
			_botsInfo.tcp_SOMAXCONN = xml->getValInt(node);
		}

		node = xml->enterNode(rootNode, "forceInternalLogin");
		if (node != NULL){
			_botsInfo.forceInternalLogin = (xml->getValStr(node) == "true");
		}

		node = xml->enterNode(rootNode, "transport");
		if (node != NULL)
		{
			const std::string transport = strutil::toLower(strutil::kbe_trim(xml->getValStr(node)));
			if (transport == "kcp" || transport == "tcp")
			{
				_botsInfo.bots_transport = transport;
			}
			else
			{
				ERROR_MSG(fmt::format(
					"ServerConfig::loadConfig: bots/transport '{}' is invalid; expected 'kcp' or 'tcp', keeping '{}'.\n",
					transport, _botsInfo.bots_transport));
			}
		}

		node = xml->enterNode(rootNode, "allowTcpFallback");
		if (node != NULL)
		{
			const std::string value = strutil::toLower(strutil::kbe_trim(xml->getValStr(node)));
			if (value == "true" || value == "false")
			{
				_botsInfo.bots_allow_tcp_fallback = value == "true";
			}
			else
			{
				ERROR_MSG(fmt::format(
					"ServerConfig::loadConfig: bots/allowTcpFallback '{}' is invalid; expected 'true' or 'false'.\n",
					value));
			}
		}

		node = xml->enterNode(rootNode, "telnet_service");
		if(node != NULL)
		{
			TiXmlNode* childnode = xml->enterNode(node, "port");
			if(childnode)
			{
				_botsInfo.telnet_port = xml->getValInt(childnode);
			}

			childnode = xml->enterNode(node, "password");
			if(childnode)
			{
				_botsInfo.telnet_passwd = xml->getValStr(childnode);
			}

			childnode = xml->enterNode(node, "default_layer");
			if(childnode)
			{
				_botsInfo.telnet_deflayer = xml->getValStr(childnode);
			}
		}
	}

	rootNode = xml->getRootNode("logger");
	if(rootNode != NULL)
	{
		node = xml->enterNode(rootNode, "internalInterface");	
		if(node != NULL)
			strncpy((char*)&_loggerInfo.internalInterface, xml->getValStr(node).c_str(), MAX_NAME - 1);

		node = xml->enterNode(rootNode, "entryScriptFile");
		if (node != NULL)
			strncpy((char*)&_loggerInfo.entryScriptFile, xml->getValStr(node).c_str(), MAX_NAME - 1);

		node = xml->enterNode(rootNode, "SOMAXCONN");
		if(node != NULL){
			_loggerInfo.tcp_SOMAXCONN = xml->getValInt(node);
		}

		node = xml->enterNode(rootNode, "tick_max_buffered_logs");
		if(node != NULL){
			tick_max_buffered_logs_ = (uint32)xml->getValInt(node);
		}

		node = xml->enterNode(rootNode, "tick_sync_logs");
		if(node != NULL){
			tick_max_sync_logs_ = (uint32)xml->getValInt(node);
		}
	
		node = xml->enterNode(rootNode, "telnet_service");
		if (node != NULL)
		{
			TiXmlNode* childnode = xml->enterNode(node, "port");
			if (childnode)
			{
				_loggerInfo.telnet_port = xml->getValInt(childnode);
			}

			childnode = xml->enterNode(node, "password");
			if (childnode)
			{
				_loggerInfo.telnet_passwd = xml->getValStr(childnode);
			}

			childnode = xml->enterNode(node, "default_layer");
			if (childnode)
			{
				_loggerInfo.telnet_deflayer = xml->getValStr(childnode);
			}
		}
	}

	if(email_service_config.size() > 0)
	{
		SmartPointer<XML> emailxml(new XML(Resmgr::getSingleton().matchRes(email_service_config).c_str()));

		if(!emailxml->isGood())
		{
			ERROR_MSG(fmt::format("ServerConfig::loadConfig: load {} is failed!\n",
				email_service_config.c_str()));

			return false;
		}

		TiXmlNode* childnode = emailxml->getRootNode("smtp_server");
		if(childnode)
			emailServerInfo_.smtp_server = emailxml->getValStr(childnode);

		childnode = emailxml->getRootNode("smtp_port");
		if(childnode)
			emailServerInfo_.smtp_port = emailxml->getValInt(childnode);

		childnode = emailxml->getRootNode("username");
		if(childnode)
			emailServerInfo_.username = emailxml->getValStr(childnode);

		childnode = emailxml->getRootNode("password");
		if(childnode)
		{
			emailServerInfo_.password = emailxml->getValStr(childnode);
		}

		childnode = emailxml->getRootNode("smtp_auth");
		if(childnode)
			emailServerInfo_.smtp_auth = emailxml->getValInt(childnode);

		TiXmlNode* rootNode1 = emailxml->getRootNode("email_activation");
		if(rootNode1 != NULL)
		{
			TiXmlNode* childnode1 = emailxml->enterNode(rootNode1, "subject");
			if(childnode1)
				emailAtivationInfo_.subject = childnode1->ToText()->Value();

			childnode1 = emailxml->enterNode(rootNode1, "message");
			if(childnode1)
				emailAtivationInfo_.message = childnode1->ToText()->Value();

			childnode1 = emailxml->enterNode(rootNode1, "deadline");
			if(childnode1)
				emailAtivationInfo_.deadline = emailxml->getValInt(childnode1);

			childnode1 = emailxml->enterNode(rootNode1, "backlink_success_message");
			if(childnode1)
				emailAtivationInfo_.backlink_success_message = childnode1->ToText()->Value();

			childnode1 = emailxml->enterNode(rootNode1, "backlink_fail_message");
			if(childnode1)
				emailAtivationInfo_.backlink_fail_message = childnode1->ToText()->Value();

			childnode1 = emailxml->enterNode(rootNode1, "backlink_hello_message");
			if(childnode1)
				emailAtivationInfo_.backlink_hello_message = childnode1->ToText()->Value();
		}

		rootNode1 = emailxml->getRootNode("email_resetpassword");
		if(rootNode1 != NULL)
		{
			TiXmlNode* childnode1 = emailxml->enterNode(rootNode1, "subject");
			if(childnode1)
				emailResetPasswordInfo_.subject = childnode1->ToText()->Value();

			childnode1 = emailxml->enterNode(rootNode1, "message");
			if(childnode1)
				emailResetPasswordInfo_.message = childnode1->ToText()->Value();

			childnode1 = emailxml->enterNode(rootNode1, "deadline");
			if(childnode1)
				emailResetPasswordInfo_.deadline = emailxml->getValInt(childnode1);

			childnode1 = emailxml->enterNode(rootNode1, "backlink_success_message");
			if(childnode1)
				emailResetPasswordInfo_.backlink_success_message = childnode1->ToText()->Value();

			childnode1 = emailxml->enterNode(rootNode1, "backlink_fail_message");
			if(childnode1)
				emailResetPasswordInfo_.backlink_fail_message = childnode1->ToText()->Value();

			childnode1 = emailxml->enterNode(rootNode1, "backlink_hello_message");
			if(childnode1)
				emailResetPasswordInfo_.backlink_hello_message = childnode1->ToText()->Value();
		}

		rootNode1 = emailxml->getRootNode("email_bind");
		if(rootNode1 != NULL)
		{
			TiXmlNode* childnode1 = emailxml->enterNode(rootNode1, "subject");
			if(childnode1)
				emailBindInfo_.subject = childnode1->ToText()->Value();

			childnode1 = emailxml->enterNode(rootNode1, "message");
			if(childnode1)
				emailBindInfo_.message = childnode1->ToText()->Value();

			childnode1 = emailxml->enterNode(rootNode1, "deadline");
			if(childnode1)
				emailBindInfo_.deadline = emailxml->getValInt(childnode1);

			childnode1 = emailxml->enterNode(rootNode1, "backlink_success_message");
			if(childnode1)
				emailBindInfo_.backlink_success_message = childnode1->ToText()->Value();

			childnode1 = emailxml->enterNode(rootNode1, "backlink_fail_message");
			if(childnode1)
				emailBindInfo_.backlink_fail_message = childnode1->ToText()->Value();

			childnode1 = emailxml->enterNode(rootNode1, "backlink_hello_message");
			if(childnode1)
				emailBindInfo_.backlink_hello_message = childnode1->ToText()->Value();
		}
	}

	// 自定义配置只接受统一的 param 结构，使 key 不受 XML 标签命名规则限制。
	// Custom configuration accepts only param elements so keys are independent of XML tag naming rules.
	rootNode = xml->getRootNode("customCfg");
	if(rootNode != NULL)
	{
		TiXmlNode* childnode = rootNode;
		while(childnode)
		{
			if(childnode->Type() == TiXmlNode::TINYXML_ELEMENT)
			{
				TiXmlElement* element = childnode->ToElement();
				if(element == NULL)
				{
					childnode = childnode->NextSibling();
					continue;
				}

				if(std::string(element->Value()) != "param")
				{
					WARNING_MSG(fmt::format(
						"ServerConfig::loadConfig: customCfg only supports <param>, ignore <{}>.\n",
						element->Value()));
					childnode = childnode->NextSibling();
					continue;
				}

				const char* name = element->Attribute("name");
				if(name == NULL || name[0] == '\0')
				{
					WARNING_MSG("ServerConfig::loadConfig: customCfg param missing name, ignored.\n");
					childnode = childnode->NextSibling();
					continue;
				}

				CustomCfgItem item;
				item.name = name;

				// 缺省类型按 string 处理，避免简单文本配置必须重复声明类型。
				// Missing types default to string so simple text values need no redundant declaration.
				const char* type = element->Attribute("type");
				item.type = (type != NULL && type[0] != '\0') ? type : "string";

				TiXmlNode* textNode = childnode->FirstChild();
				item.value = textNode != NULL ? xml->getValStr(textNode) : "";

				// loadConfig 先读取 defaults 再读取项目配置，赋值语义自然提供业务覆盖能力。
				// loadConfig reads defaults before project settings, so assignment provides project overrides.
				customCfg_[item.name] = item;
			}

			childnode = childnode->NextSibling();
		}
	}

	return true;
}

//-------------------------------------------------------------------------------------	
uint32 ServerConfig::tcp_SOMAXCONN(COMPONENT_TYPE componentType)
{
	ENGINE_COMPONENT_INFO& cinfo = getComponent(componentType);
	return cinfo.tcp_SOMAXCONN;
}

//-------------------------------------------------------------------------------------	
void ServerConfig::_updateEmailInfos()
{
	// 如果小于64则表示目前还是明文密码
	if(emailServerInfo_.password.size() < 64)
	{
		WARNING_MSG(fmt::format("ServerConfig::loadConfig: email password(email_service.xml) is not encrypted!\nplease use password(rsa):\n{}\n"
			, KBEKey::getSingleton().encrypt(emailServerInfo_.password)));
	}
	else
	{
		std::string out = KBEKey::getSingleton().decrypt(emailServerInfo_.password);
		if(out.size() == 0)
		{
			ERROR_MSG("ServerConfig::loadConfig: email password(email_service.xml) encrypt error!\n");
		}
		else
		{
			emailServerInfo_.password = out;
		}
	}
}

//-------------------------------------------------------------------------------------	
void ServerConfig::updateExternalAddress(char* buf)
{
	if(strlen(buf) > 0)
	{
		unsigned int inaddr = 0; 
		if((inaddr = inet_addr(buf)) == INADDR_NONE)  
		{
			struct hostent *host;
			host = gethostbyname(buf);
			if(host)
			{
				strncpy(buf, inet_ntoa(*(struct in_addr*)host->h_addr_list[0]), MAX_BUF - 1);
			}	
		}
	}
}

//-------------------------------------------------------------------------------------	
void ServerConfig::updateInfos(bool isPrint, COMPONENT_TYPE componentType, COMPONENT_ID componentID, 
							   const Network::Address& internalAddr, const Network::Address& externalAddr)
{
	std::string infostr = "";

	KBE_ASSERT(_dbmgrInfo.dbInterfaceInfos.size() <= static_cast<size_t>(std::numeric_limits<int>::max()));
	for (size_t i = 0; i < _dbmgrInfo.dbInterfaceInfos.size(); ++i)
		_dbmgrInfo.dbInterfaceInfos[i].index = static_cast<int>(i);

	if (g_dbmgr_addDefaultAddress)
	{
		Network::Address interfacesAddr(interfacesAddress_, interfacesPort_min_);
		interfacesAddrs_.insert(interfacesAddrs_.begin(), interfacesAddr);
	}

	//updateExternalAddress(getBaseApp().externalAddress);
	//updateExternalAddress(getLoginApp().externalAddress);

	if(componentType == CELLAPP_TYPE)
	{
		ENGINE_COMPONENT_INFO info = getCellApp();
		info.internalAddr = &internalAddr;
		info.externalAddr = &externalAddr;
		info.componentID = componentID;

		if (info.ids_criticallyLowSize > getDBMgr().ids_increasing_range / 2)
		{
			info.ids_criticallyLowSize = getDBMgr().ids_increasing_range / 2;
			ERROR_MSG(fmt::format("kbengine[_defs].xml->cellapp->ids->criticallyLowSize > dbmgr->ids->increasing_range / 2, Force adjustment to criticallyLowSize({})\n", 
				info.ids_criticallyLowSize));
		}

		if(isPrint)
		{
			INFO_MSG("server-configs:\n");
			INFO_MSG(fmt::format("\tgameUpdateHertz : {}\n", gameUpdateHertz()));
			INFO_MSG(fmt::format("\tdefaultViewRadius : {}\n", info.defaultViewRadius));
			INFO_MSG(fmt::format("\tdefaultViewHysteresisArea : {}\n", info.defaultViewHysteresisArea));
			INFO_MSG(fmt::format("\tentryScriptFile : {}\n", info.entryScriptFile));
			INFO_MSG(fmt::format("\tinternalAddr : {}\n", internalAddr.c_str()));
			//INFO_MSG(fmt::format("\texternalAddr : {}\n", externalAddr.c_str()));
			INFO_MSG(fmt::format("\tcomponentID : {}\n", info.componentID));

			infostr += "server-configs:\n";
			infostr += (fmt::format("\tgameUpdateHertz : {}\n", gameUpdateHertz()));
			infostr += (fmt::format("\tdefaultViewRadius : {}\n", info.defaultViewRadius));
			infostr += (fmt::format("\tdefaultViewHysteresisArea : {}\n", info.defaultViewHysteresisArea));
			infostr += (fmt::format("\tentryScriptFile : {}\n", info.entryScriptFile));
			infostr += (fmt::format("\tinternalAddr : {}\n", internalAddr.c_str()));
			//infostr += (fmt::format("\texternalAddr : {}\n", externalAddr.c_str()));
			infostr += (fmt::format("\tcomponentID : {}\n", info.componentID));
		}
	}
	else if (componentType == BASEAPP_TYPE)
	{
		ENGINE_COMPONENT_INFO info = getBaseApp();
		info.internalAddr = const_cast<Network::Address*>(&internalAddr);
		info.externalAddr = const_cast<Network::Address*>(&externalAddr);
		info.componentID = componentID;

		if (info.ids_criticallyLowSize > getDBMgr().ids_increasing_range / 2)
		{
			info.ids_criticallyLowSize = getDBMgr().ids_increasing_range / 2;
			ERROR_MSG(fmt::format("kbengine[_defs].xml->baseapp->ids->criticallyLowSize > dbmgr->ids->increasing_range / 2, Force adjustment to criticallyLowSize({})\n",
				info.ids_criticallyLowSize));
		}

		if(isPrint)
		{
			INFO_MSG("server-configs:\n");
			INFO_MSG(fmt::format("\tgameUpdateHertz : {}\n", gameUpdateHertz()));
			INFO_MSG(fmt::format("\tentryScriptFile : {}\n", info.entryScriptFile));
			INFO_MSG(fmt::format("\tinternalAddr : {}\n", internalAddr.c_str()));
			INFO_MSG(fmt::format("\texternalAddr : {}\n", externalAddr.c_str()));

			if(strlen(info.externalAddress) > 0)
			{
				INFO_MSG(fmt::format("\texternalCustomAddr : {}\n", info.externalAddress));
			}

			INFO_MSG(fmt::format("\tcomponentID : {}\n", info.componentID));

			infostr += "server-configs:\n";
			infostr += (fmt::format("\tgameUpdateHertz : {}\n", gameUpdateHertz()));
			infostr += (fmt::format("\tentryScriptFile : {}\n", info.entryScriptFile));
			infostr += (fmt::format("\tinternalAddr : {}\n", internalAddr.c_str()));
			infostr += (fmt::format("\texternalAddr : {}\n", externalAddr.c_str()));

			if(strlen(info.externalAddress) > 0)
			{
				infostr +=  (fmt::format("\texternalCustomAddr : {}\n", info.externalAddress));
			}

			infostr += (fmt::format("\tcomponentID : {}\n", info.componentID));
		}

		_updateEmailInfos();
	}
	else if (componentType == BASEAPPMGR_TYPE)
	{
		ENGINE_COMPONENT_INFO info = getBaseAppMgr();
		info.internalAddr = const_cast<Network::Address*>(&internalAddr);
		info.externalAddr = const_cast<Network::Address*>(&externalAddr);
		info.componentID = componentID;

		if(isPrint)
		{
			INFO_MSG("server-configs:\n");
			INFO_MSG(fmt::format("\tinternalAddr : {}\n", internalAddr.c_str()));
			//INFO_MSG((fmt::format("\texternalAddr : %s\n", externalAddr.c_str())));
			INFO_MSG(fmt::format("\tcomponentID : {}\n", info.componentID));

			infostr += "server-configs:\n";
			infostr += (fmt::format("\tinternalAddr : {}\n", internalAddr.c_str()));
			infostr += (fmt::format("\tcomponentID : {}\n", info.componentID));
		}
	}
	else if (componentType == CELLAPPMGR_TYPE)
	{
		ENGINE_COMPONENT_INFO info = getCellAppMgr();
		info.internalAddr = const_cast<Network::Address*>(&internalAddr);
		info.externalAddr = const_cast<Network::Address*>(&externalAddr);
		info.componentID = componentID;

		if(isPrint)
		{
			INFO_MSG("server-configs:\n");
			INFO_MSG(fmt::format("\tinternalAddr : {}\n", internalAddr.c_str()));
			//INFO_MSG((fmt::format("\texternalAddr : %s\n", externalAddr.c_str())));
			INFO_MSG(fmt::format("\tcomponentID : {}\n", info.componentID));

			infostr += "server-configs:\n";
			infostr += (fmt::format("\tinternalAddr : {}\n", internalAddr.c_str()));
			infostr += (fmt::format("\tcomponentID : {}\n", info.componentID));
		}
	}
	else if (componentType == DBMGR_TYPE)
	{
		ENGINE_COMPONENT_INFO info = getDBMgr();
		info.internalAddr = const_cast<Network::Address*>(&internalAddr);
		info.externalAddr = const_cast<Network::Address*>(&externalAddr);
		info.componentID = componentID;

		if (info.ids_increasing_range < 500)
		{
			info.ids_increasing_range = 500;
			ERROR_MSG(fmt::format("kbengine[_defs].xml-> dbmgr->ids->increasing_range too small, Force adjustment to ids_increasing_range({})\n",
				info.ids_increasing_range));
		}

		if(isPrint)
		{
			INFO_MSG("server-configs:\n");
			INFO_MSG(fmt::format("\tinternalAddr : {}\n", internalAddr.c_str()));
			//INFO_MSG((fmt::format("\texternalAddr : %s\n", externalAddr.c_str())));
			INFO_MSG(fmt::format("\tcomponentID : {}\n", info.componentID));

			infostr += "server-configs:\n";
			infostr += (fmt::format("\tinternalAddr : {}\n", internalAddr.c_str()));
			infostr += (fmt::format("\tcomponentID : {}\n", info.componentID));
		}
	}
	else if (componentType == LOGINAPP_TYPE)
	{
		ENGINE_COMPONENT_INFO info = getLoginApp();
		info.internalAddr = const_cast<Network::Address*>(&internalAddr);
		info.externalAddr = const_cast<Network::Address*>(&externalAddr);
		info.componentID = componentID;

		if(isPrint)
		{
			INFO_MSG("server-configs:\n");
			INFO_MSG(fmt::format("\tinternalAddr : {}\n", internalAddr.c_str()));
			INFO_MSG(fmt::format("\texternalAddr : {}\n", externalAddr.c_str()));
			if(strlen(info.externalAddress) > 0)
			{
				INFO_MSG(fmt::format("\texternalCustomAddr : {}\n", info.externalAddress));
			}

			INFO_MSG(fmt::format("\tcomponentID : {}\n", info.componentID));

			infostr += "server-configs:\n";
			infostr += (fmt::format("\tinternalAddr : {}\n", internalAddr.c_str()));
			infostr += (fmt::format("\texternalAddr : {}\n", externalAddr.c_str()));

			if(strlen(info.externalAddress) > 0)
			{
				infostr +=  (fmt::format("\texternalCustomAddr : {}\n", info.externalAddress));
			}

			infostr += (fmt::format("\tcomponentID : {}\n", info.componentID));
		}

		_updateEmailInfos();
	}
	else if (componentType == MACHINE_TYPE)
	{
		ENGINE_COMPONENT_INFO info = getKBMachine();
		info.internalAddr = const_cast<Network::Address*>(&internalAddr);
		info.externalAddr = const_cast<Network::Address*>(&externalAddr);
		info.componentID = componentID;
		if(isPrint)
		{
			INFO_MSG("server-configs:\n");
			INFO_MSG(fmt::format("\tinternalAddr : {}\n", internalAddr.c_str()));
			//INFO_MSG((fmt::format("\texternalAddr : %s\n", externalAddr.c_str())));
			INFO_MSG(fmt::format("\tcomponentID : {}\n", info.componentID));

			infostr += "server-configs:\n";
			infostr += (fmt::format("\tinternalAddr : {}\n", internalAddr.c_str()));
			infostr += (fmt::format("\tcomponentID : {}\n", info.componentID));
		}
	}
	else if (componentType == INTERFACES_TYPE)
	{
		ENGINE_COMPONENT_INFO info = getInterfaces();
		info.internalAddr = const_cast<Network::Address*>(&internalAddr);
		info.externalAddr = const_cast<Network::Address*>(&externalAddr);
		info.componentID = componentID;
		if (isPrint)
		{
			INFO_MSG("server-configs:\n");
			INFO_MSG(fmt::format("\tinternalAddr : {}\n", internalAddr.c_str()));
			INFO_MSG((fmt::format("\texternalAddr : %s\n", externalAddr.c_str())));
			INFO_MSG(fmt::format("\tcomponentID : {}\n", info.componentID));

			infostr += "server-configs:\n";
			infostr += (fmt::format("\tinternalAddr : {}\n", internalAddr.c_str()));
			infostr += (fmt::format("\tcomponentID : {}\n", info.componentID));
		}
	}

#if KBE_PLATFORM == PLATFORM_WIN32
	if(infostr.size() > 0)
	{
		infostr += "\n";
		printf("%s", infostr.c_str());
	}
#endif
}

//-------------------------------------------------------------------------------------
bool ServerConfig::enableRawDatabaseCommandBlacklist() const
{
	return _dbmgrInfo.enableRawDatabaseCommandBlacklist;
}

//-------------------------------------------------------------------------------------
const std::vector<std::string>& ServerConfig::rawDatabaseCommandBlacklist(const std::string& dbType) const
{
	static const std::vector<std::string> emptyList;
	if (!_dbmgrInfo.enableRawDatabaseCommandBlacklist)
		return emptyList;

	std::string normalizedType = strutil::toLower(strutil::kbe_trim(dbType));
	if (normalizedType == "pgsql")
		normalizedType = "postgresql";

	std::map<std::string, std::vector<std::string> >::const_iterator iter =
		_dbmgrInfo.rawDatabaseCommandBlacklist.find(normalizedType);
	return iter == _dbmgrInfo.rawDatabaseCommandBlacklist.end() ? emptyList : iter->second;
}

//-------------------------------------------------------------------------------------		
}
