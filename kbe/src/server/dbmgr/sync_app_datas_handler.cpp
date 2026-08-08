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

#include "dbmgr.h"
#include "sync_app_datas_handler.h"
#include "entitydef/scriptdef_module.h"
#include "entitydef/entity_macro.h"
#include "network/fixed_messages.h"
#include "math/math.h"
#include "network/bundle.h"
#include "network/channel.h"
#include "server/components.h"

#include "baseapp/baseapp_interface.h"
#include "cellapp/cellapp_interface.h"
#include "baseappmgr/baseappmgr_interface.h"
#include "cellappmgr/cellappmgr_interface.h"
#include "loginapp/loginapp_interface.h"

namespace KBEngine{	

namespace
{
const uint64 SYNC_APP_DATAS_WAIT_NS = 3ULL * 1000ULL * 1000ULL * 1000ULL;
const uint64 SYNC_APP_DATAS_WAIT_LOG_NS = 5ULL * 1000ULL * 1000ULL * 1000ULL;

uint64 syncAppDatasNow()
{
	// 启动 ready gate 只需要稳定的单调时间，不需要跟随全局 profiling timing method。
	// The readiness gate needs stable monotonic time, not the global profiling timing method.
	// 在 macOS 上 timestamp() 可能受运行时 timing method 切换影响而短暂回退，导致 init completed 一直延后。
	// On macOS timestamp() can move backwards when the runtime timing method changes, which can keep init-completed delayed.
	return timestamp_gettime();
}
}

//-------------------------------------------------------------------------------------
SyncAppDatasHandler::SyncAppDatasHandler():
Task(),
lastRegAppTime_(0),
lastWaitLogTime_(0),
apps_()
{
	// 由 Dbmgr 主 tick 显式驱动，避免 ready gate 依赖 Task 容器的隐式生命周期。
	// Dbmgr drives this explicitly from its main tick so the readiness gate follows the registration data flow.
}

//-------------------------------------------------------------------------------------
SyncAppDatasHandler::~SyncAppDatasHandler()
{
	// networkInterface_.dispatcher().cancelTask(this);
	DEBUG_MSG("SyncAppDatasHandler::~SyncAppDatasHandler()\n");

	Dbmgr::getSingleton().pSyncAppDatasHandler(NULL);
}

//-------------------------------------------------------------------------------------
void SyncAppDatasHandler::pushApp(COMPONENT_ID cid, COMPONENT_ORDER startGroupOrder, COMPONENT_ORDER startGlobalOrder)
{
	std::vector<ComponentInitInfo>::iterator iter = apps_.begin();
	for(; iter != apps_.end(); ++iter)
	{
		if((*iter).cid == cid)
		{
			ERROR_MSG(fmt::format("SyncAppDatasHandler::pushApp: cid({}) is exist!\n", cid));
			return;
		}
	}

	ComponentInitInfo cinfo;
	cinfo.cid = cid;
	cinfo.startGroupOrder = startGroupOrder;
	cinfo.startGlobalOrder = startGlobalOrder;
	apps_.push_back(cinfo);
	lastRegAppTime_ = syncAppDatasNow();

	INFO_MSG(fmt::format(
		"SyncAppDatasHandler::pushApp: queued componentID={}, startGlobalOrder={}, startGroupOrder={}, pending={}.\n",
		cid, startGlobalOrder, startGroupOrder, apps_.size()));
}

//-------------------------------------------------------------------------------------
bool SyncAppDatasHandler::process()
{
	if(lastRegAppTime_ == 0)
		return true;

	const uint64 now = syncAppDatasNow();
	bool hasApp = false;

	std::vector<ComponentInitInfo>::iterator iter = apps_.begin();
	for(; iter != apps_.end(); ++iter)
	{
		ComponentInitInfo cInitInfo = (*iter);
		Components::ComponentInfos* cinfos = Components::getSingleton().findComponent(cInitInfo.cid);

		if(cinfos == NULL)
			continue;

		COMPONENT_TYPE tcomponentType = cinfos->componentType;
		if(tcomponentType == BASEAPP_TYPE || 
			tcomponentType == CELLAPP_TYPE ||
			tcomponentType == LOGINAPP_TYPE)
		{
			hasApp = true;
			break;
		}
	}
	
	if(!hasApp)
	{
		if (lastWaitLogTime_ == 0 || now - lastWaitLogTime_ >= SYNC_APP_DATAS_WAIT_LOG_NS)
		{
			lastWaitLogTime_ = now;
			INFO_MSG(fmt::format(
				"SyncAppDatasHandler::process: waiting for entity apps, pending={}.\n",
				apps_.size()));
		}
		return true;
	}

	if(now < lastRegAppTime_)
	{
		// macOS 上调试/休眠/不同计时后端切换时可能观察到时间戳回退。
		// The startup gate must not become stuck when a monotonic source appears to move backwards.
		// 这里重置等待窗口，比无符号下溢后立即发送更保守。
		// Reset the wait window; this is safer than unsigned underflow causing an immediate send.
		WARNING_MSG(fmt::format(
			"SyncAppDatasHandler::process: timestamp moved backwards, now={}, lastRegAppTime={}, pending={}.\n",
			now, lastRegAppTime_, apps_.size()));
		lastRegAppTime_ = now;
		return true;
	}

	const uint64 elapsed = now - lastRegAppTime_;
	if(elapsed < SYNC_APP_DATAS_WAIT_NS)
		return true;

	std::string digest = EntityDef::md5().getDigestStr();
	bool hasUnavailableChannel = false;

	// 如果是连接到dbmgr则需要等待接收app初始信息
	// 例如：初始会分配entityID段以及这个app启动的顺序信息（是否第一个baseapp启动）
	iter = apps_.begin();
	for(; iter != apps_.end(); ++iter)
	{
		ComponentInitInfo cInitInfo = (*iter);
		Components::ComponentInfos* cinfos = Components::getSingleton().findComponent(cInitInfo.cid);

		if(cinfos == NULL)
			continue;

		COMPONENT_TYPE tcomponentType = cinfos->componentType;

		if(tcomponentType == BASEAPP_TYPE || 
			tcomponentType == CELLAPP_TYPE || 
			tcomponentType == LOGINAPP_TYPE)
		{
			if (cinfos->pChannel == NULL || cinfos->pChannel->isDestroyed())
			{
				// 组件注册和反向连接切换期间可能短暂没有可用 Channel。
				// During registration/reverse-channel handoff a component can briefly have no usable Channel.
				// 初始化完成包是启动链的关键消息，不能静默丢失，否则 BaseApp/CellApp 会永远不进入 ready。
				// The init-completed message gates readiness; silently losing it leaves BaseApp/CellApp stuck forever.
				WARNING_MSG(fmt::format(
					"SyncAppDatasHandler::process: component channel unavailable, componentType={}, componentID={}.\n",
					tcomponentType, cInitInfo.cid));
				hasUnavailableChannel = true;
				continue;
			}

			Network::Bundle* pBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
			
			switch(tcomponentType)
			{
			case BASEAPP_TYPE:
				{
					Dbmgr::getSingleton().onGlobalDataClientLogon(cinfos->pChannel, BASEAPP_TYPE);

					std::pair<ENTITY_ID, ENTITY_ID> idRange = Dbmgr::getSingleton().idServer().allocRange();
					(*pBundle).newMessage(BaseappInterface::onDbmgrInitCompleted);
					BaseappInterface::onDbmgrInitCompletedArgs6::staticAddToBundle((*pBundle), g_kbetime, idRange.first, 
						idRange.second, cInitInfo.startGlobalOrder, cInitInfo.startGroupOrder, 
						digest);
				}
				break;
			case CELLAPP_TYPE:
				{
					Dbmgr::getSingleton().onGlobalDataClientLogon(cinfos->pChannel, CELLAPP_TYPE);

					std::pair<ENTITY_ID, ENTITY_ID> idRange = Dbmgr::getSingleton().idServer().allocRange();
					(*pBundle).newMessage(CellappInterface::onDbmgrInitCompleted);
					CellappInterface::onDbmgrInitCompletedArgs6::staticAddToBundle((*pBundle), g_kbetime, idRange.first, 
						idRange.second, cInitInfo.startGlobalOrder, cInitInfo.startGroupOrder, 
						digest);
				}
				break;
			case LOGINAPP_TYPE:
				(*pBundle).newMessage(LoginappInterface::onDbmgrInitCompleted);
				LoginappInterface::onDbmgrInitCompletedArgs3::staticAddToBundle((*pBundle), 
						cInitInfo.startGlobalOrder, cInitInfo.startGroupOrder, 
						digest);

				break;
			default:
				break;
			}

			INFO_MSG(fmt::format(
				"SyncAppDatasHandler::process: sent init completed to {}, componentID={}, startGlobalOrder={}, startGroupOrder={}.\n",
				COMPONENT_NAME_EX(tcomponentType), cInitInfo.cid, cInitInfo.startGlobalOrder, cInitInfo.startGroupOrder));
			cinfos->pChannel->send(pBundle);
		}
	}

	if(hasUnavailableChannel)
	{
		// 保留队列稍后重试，避免 ready gate 的关键包因为连接切换窗口丢失。
		// Keep the queue for a later retry so the readiness gate message is not lost during channel handoff.
		lastRegAppTime_ = syncAppDatasNow();
		return true;
	}

	apps_.clear();

	delete this;
	return false;
}

//-------------------------------------------------------------------------------------

}
