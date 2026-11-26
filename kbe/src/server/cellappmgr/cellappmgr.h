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


#ifndef KBE_CELLAPPMGR_H
#define KBE_CELLAPPMGR_H
	
#include "cellapp.h"
#include "space_viewer.h"

#include <set>
#include "server/kbemain.h"
#include "server/cell_process_watch.h"
#include "server/serverapp.h"
#include "server/idallocate.h"
#include "server/serverconfig.h"
#include "server/space_layout.h"
#include "server/forward_messagebuffer.h"
#include "common/timer.h"
#include "network/endpoint.h"

namespace KBEngine{


class Cellappmgr :	public ServerApp, 
					public Singleton<Cellappmgr>
{
public:
	enum TimeOutType
	{
		TIMEOUT_GAME_TICK = TIMEOUT_SERVERAPP_MAX + 1
	};
	
	Cellappmgr(Network::EventDispatcher& dispatcher, 
		Network::NetworkInterface& ninterface, 
		COMPONENT_TYPE componentType,
		COMPONENT_ID componentID);

	~Cellappmgr();
	
	bool run();
	
	virtual void onChannelDeregister(Network::Channel * pChannel);
	virtual void onRegisterNewApp(Network::Channel* pChannel,
		int32 uid, std::string& username,
		COMPONENT_TYPE componentType, COMPONENT_ID componentID,
		COMPONENT_ORDER globalorderID, COMPONENT_ORDER grouporderID,
		uint32 intaddr, uint16 intport, uint32 extaddr, uint16 extport,
		std::string& extaddrEx);

	void handleTimeout(TimerHandle handle, void * arg);
	void handleGameTick();

	/* 初始化相关接口 */
	bool initializeBegin();
	bool inInitialize();
	bool initializeEnd();
	bool initializeWatcher();
	void finalise();

	/** 找出一个最空闲的cellapp */
	COMPONENT_ID findFreeCellapp(SPACE_ID spaceID = 0, const SpaceLayout::Transaction* replacement = nullptr);
	void updateBestCellapp();

	/** 网络接口
		baseEntity请求创建在一个新的space中
	*/
	void reqCreateCellEntityInNewSpace(Network::Channel* pChannel, MemoryStream& s);

	/** 网络接口
		baseEntity请求创建在一个新的space中
	*/
	void reqRestoreSpaceInCell(Network::Channel* pChannel, MemoryStream& s);
	
	/** 网络接口
		消息转发， 由某个app想通过本app将消息转发给某个app。
	*/
	void forwardMessage(Network::Channel* pChannel, MemoryStream& s);

	/** 网络接口
		更新cellapp情况。
	*/
	void updateCellapp(Network::Channel* pChannel, COMPONENT_ID componentID, ENTITY_ID numEntities,
		float load, uint32 flags, uint64 activeWitnesses, uint64 pendingWitnesses);

	/** 网络接口
		cellapp同步自己的初始化信息
		startGlobalOrder: 全局启动顺序 包括各种不同组件
		startGroupOrder: 组内启动顺序， 比如在所有baseapp中第几个启动。
	*/
	void onCellappInitProgress(Network::Channel* pChannel, COMPONENT_ID cid, float progress, 
		COMPONENT_ORDER componentGlobalOrder, COMPONENT_ORDER componentGroupOrder);

	bool componentsReady();
	bool componentReady(COMPONENT_ID cid);
	bool spacePartitioningEnabled() const;
	uint32 spacePartitioningConfirmedApps() const;
	void reportSpacePartitioningMode(Network::Channel* pChannel, COMPONENT_ID cid, uint8 mode,
		uint32 pid, uint64 processStart, uint64 pidNamespace, uint64 process);
	void onSpaceLayoutRequest(Network::Channel* pChannel, MemoryStream& stream);
	void manageSpaceLayout(Network::Channel* channel, MemoryStream& stream);
	void reqCellRecoveryScope(Network::Channel* channel, MemoryStream& stream);
	uint64 managedSpaceCount() const { return spaceLayouts_.size(); }

	Cellapp& getCellapp(COMPONENT_ID cid);
	std::map< COMPONENT_ID, Cellapp >& cellapps();
	uint32 readyForLoginApps();
	uint32 totalReadyForLoginApps();
	float minReadyForLoginProgress();
	bool readyForLogin();

	uint32 numLoadBalancingApp();
	uint64 confirmedSpaceCount() const;
	uint64 pendingSpaceCount() const;
	uint64 minAssignedSpaceCount() const;
	uint64 maxAssignedSpaceCount() const;
	uint64 minPendingWitnessCount() const;
	uint64 maxPendingWitnessCount() const;
	uint64 minActiveWitnessCount() const;
	uint64 maxActiveWitnessCount() const;
	float witnessPendingPressureWeight() const;

	/* 以groupOrderID为排序基准，
	   増加一个cellapp component id到cellapp_cids_列表中
	*/
	void addCellappComponentID(COMPONENT_ID cid);

	/** 网络接口
	查询所有相关进程负载信息
	*/
	void queryAppsLoads(Network::Channel* pChannel, MemoryStream& s);

	/** 网络接口
	查询所有相关进程space信息
	*/
	void querySpaces(Network::Channel* pChannel, MemoryStream& s);

	/** 网络接口
	更新相关进程space信息，注意：此spaceData并非API文档中描述的spaceData
	是指space的一些信息
	*/
	void updateSpaceData(Network::Channel* pChannel, MemoryStream& s);

	/** 网络接口
	工具请求改变space查看器（含添加和删除功能）
	如果是请求更新并且服务器上不存在该地址的查看器则自动创建，如果是删除则明确给出删除要求
	*/
	void setSpaceViewer(Network::Channel* pChannel, MemoryStream& s);

protected:
	bool spaceAssigned(SPACE_ID spaceID);
	bool sendSpaceLayout(COMPONENT_ID cid, const SpaceLayout::Packet& packet);
	bool collectCellPeerRoutes(const SpaceLayout::Layout& layout, SpaceLayout::Packet& packet);
	void broadcastSpaceLayout(SPACE_ID spaceID, SpaceLayout::Transaction& transaction,
		SpaceLayout::Operation operation);
	void failSpaceLayout(SPACE_ID spaceID, SpaceLayout::Transaction& transaction,
		const std::string& error, bool suspend);
	void requestBoundaryMove(SpaceLayout::Packet& packet, SpaceLayout::Transaction* transaction);
	void reportBoundaryMove(SPACE_ID spaceID, COMPONENT_ID owner, uint64 version, uint64 revision, const char* error);
	void finishBoundaryMove(SPACE_ID spaceID, SpaceLayout::Transaction& transaction, const char* error);
	const char* requestSpaceSplit(SPACE_ID spaceID, SpaceLayout::Transaction& transaction,
		uint64 version, uint64 boundaryRevision, CELL_ID cell, uint8 axis, float position);
	void handleSpaceSplitAck(const SpaceLayout::Packet& packet, SpaceLayout::Transaction& transaction,
		Network::Channel* channel);
	void stopSpaceSplit(SPACE_ID spaceID, SpaceLayout::Transaction& transaction, const char* reason);
	void sendSplitResource(const SpaceLayout::Packet& packet, SpaceLayout::Transaction& transaction);
	const char* requestSpaceMerge(SPACE_ID spaceID, SpaceLayout::Transaction& transaction,
		uint64 version, uint64 revision, CELL_ID retiring, CELL_ID keeping);
	void handleSpaceMergeAck(SpaceLayout::Packet& packet, SpaceLayout::Transaction& transaction);
	void stopSpaceMerge(SPACE_ID spaceID, SpaceLayout::Transaction& transaction, const char* reason);
	void finishSpaceMerge(SPACE_ID spaceID, SpaceLayout::Transaction& transaction);
	void updateSpaceLayouts();
	bool updateCellReplacement(SPACE_ID spaceID, SpaceLayout::Transaction& transaction, uint64 now);
	void checkCellReplacement(SPACE_ID spaceID, SpaceLayout::Transaction& transaction, uint64 now);
	void cancelCellReplacement(SPACE_ID spaceID, SpaceLayout::Transaction& transaction, const char* reason);
	uint64 preparedCellReplacements() const;
	void removeCellapp(COMPONENT_ID cid, uint64 session);
	void onSpaceLayoutCellLost(COMPONENT_ID cid, uint64 session);
	void watchPartitionCell(Network::Channel& channel, const Components::ComponentInfos& source,
		const CellRecovery::ProcessIdentity& identity, uint64 process);
	void notifyCellProcessExit(COMPONENT_ID cid, uint64 process);
	void checkLostPartitionCells();
	uint64 lostPartitionCellsWaiting() const;
	uint64 lostPartitionCellsExited() const;

	struct PartitionCellProcess
	{
		uint64 session = 0;
		uint64 process = 0;
		std::unique_ptr<CellRecovery::ProcessWatch> watch;
	};

	struct LostPartitionCell
	{
		uint64 session = 0;
		uint64 process = 0;
		std::map<SPACE_ID, uint64> layouts;
		std::unique_ptr<CellRecovery::ProcessWatch> watch;
		bool exited = false;
	};

	TimerHandle							gameTimer_;
	ForwardAnywhere_MessageBuffer		forward_anywhere_cellapp_messagebuffer_;
	ForwardComponent_MessageBuffer		forward_cellapp_messagebuffer_;

	COMPONENT_ID						bestCellappID_;

	std::map< COMPONENT_ID, Cellapp >	cellapps_;
	std::vector<COMPONENT_ID>			cellapp_cids_;
	uint8								spacePartitioningMode_;
	// 尚无可用 CellApp 时仍需保留编号，避免重复恢复请求绕过单区域限制。
	std::set<SPACE_ID>					queuedSpaceIDs_;
	std::map<SPACE_ID, SpaceLayout::Transaction> spaceLayouts_;
	struct CancelledSplit
	{
		uint64 session, version, attempt;
	};
	std::map<COMPONENT_ID, std::map<SPACE_ID, CancelledSplit>> cancelledSplits_;
	// 只保留涉及 Cell 退出的已关闭 Space 编号，供稍后收到的 Base 查询使用。
	std::set<SPACE_ID> closedSpacesWithCellLoss_;
	std::map<COMPONENT_ID, PartitionCellProcess> partitionCellProcesses_;
	std::map<COMPONENT_ID, LostPartitionCell> lostPartitionCells_;
	std::map<std::pair<COMPONENT_ID, uint64>, std::unique_ptr<CellRecovery::ProcessWatch>> disconnectedCellProcesses_;
	uint64 nextSpaceLayoutCheck_ = 0;
	SPACE_ID lastReplacementSpaceChecked_ = 0;

	// 通过工具查看space
	SpaceViewers						spaceViewers_;
};

} 

#endif // KBE_CELLAPPMGR_H
