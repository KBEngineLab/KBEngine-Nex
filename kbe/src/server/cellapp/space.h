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

#ifndef KBE_SPACE_H
#define KBE_SPACE_H

#include "coordinate_system.h"
#include "boundary_ghosts.h"
#include "cell.h"
#include "helper/debug_helper.h"
#include "common/common.h"
#include "common/smartpointer.h"
#include "pyscript/scriptobject.h"
#include "navigation/navigation_handle.h"
#include "network/address.h"
#include "server/cell_recovery.h"
#include "server/space_layout.h"

namespace KBEngine{

class Entity;
typedef SmartPointer<Entity> EntityPtr;
typedef std::vector<EntityPtr> SPACE_ENTITIES;

class Space
{
public:
	enum VIEWER_BOUNDS_SOURCE
	{
		VIEWER_BOUNDS_DEFAULT = 0,
		VIEWER_BOUNDS_EXPLICIT = 1,
		VIEWER_BOUNDS_NAVIGATION = 2,
		VIEWER_BOUNDS_OBSERVED_ENTITIES = 3
	};

	Space(SPACE_ID spaceID, const std::string& scriptModuleName, bool reportToManager = true);
	~Space();

	void unLoadSpaceGeometry();
	void loadSpaceGeometry(const std::map< int, std::string >& params);

	/** 
		更新space中的内容 
	*/
	bool update();

	void addEntity(Entity* pEntity, bool recordMigrationStages = false);
	void addEntityToNode(Entity* pEntity);

	void addEntityAndEnterWorld(Entity* pEntity, bool isRestore = false,
		bool recordMigrationStages = false);
	void removeEntity(Entity* pEntity);

	/**
		一个entity进入了游戏世界
	*/
	void onEnterWorld(Entity* pEntity, bool recordMigrationStages = false);
	void _onEnterWorld(Entity* pEntity, bool recordMigrationStages = false);
	void onLeaveWorld(Entity* pEntity);

	void onEntityAttachWitness(Entity* pEntity);

	SPACE_ID id() const{ return id_; }

	const SPACE_ENTITIES& entities() const{ return entities_; }
	Entity* findEntity(ENTITY_ID entityID);

	/**
		销毁
	*/
	enum class DestroyMode { Requested, LayoutRemoval, Shutdown };
	bool destroy(ENTITY_ID entityID, bool ignoreGhost = true, DestroyMode mode = DestroyMode::Requested);

	// 关服终结必须释放所有ghost引用，避免Space因外部共享引用延迟到实体注册表卸载后才析构。
	// Shutdown finalization must release every ghost reference so external shared owners cannot defer Space destruction past entity-registry teardown.
	void finalise(bool notifyCellappmgr = false);

	/**
		这个space的cell
	*/
	Cell * pCell() const	{ return pCell_; }
	void pCell( Cell * pCell );

	/**
		添加space的几何映射
	*/
	static PyObject* __py_AddSpaceGeometryMapping(PyObject* self, PyObject* args);
	bool addSpaceGeometryMapping(std::string respath, bool shouldLoadOnServer, const std::map< int, std::string >& params);
	static PyObject* __py_GetSpaceGeometryMapping(PyObject* self, PyObject* args);
	const std::string& getGeometryPath();
	const std::string& getScriptModuleName() const { return scriptModuleName_; }
	bool getViewerBounds(float& minimumX, float& minimumZ, float& maximumX, float& maximumZ,
		VIEWER_BOUNDS_SOURCE& source) const;
	void setGeometryPath(const std::string& path);
	void onLoadedSpaceGeometryMapping(const std::string& resPath, uint64 loadGeneration, NavigationHandlePtr pNavHandle);
	void onAllSpaceGeometryLoaded();
	bool isGeometryLoadCurrent(const std::string& resPath, uint64 loadGeneration);
	bool isGeometryLoading() const { return isGeometryLoading_; }
	const SpaceLayout::Layout& layout() const { return layout_; }
	const SpaceLayout::Bounds& localCellBounds() const { return localCellBounds_; }
	BoundaryGhosts& boundaryGhosts() { return boundaryGhosts_; }
	uint64 layoutVersion() const { return layoutVersion_; }
	uint64 resourceRevision() const { return resourceRevision_; }
	uint64 boundaryRevision() const { return boundaryRevision_; }
	bool boundaryMovePending() const { return pendingBoundaryRevision_ != 0; }
	const std::string& boundaryMoveError() const { return boundaryMoveError_; }
	void beginBoundaryMove(uint64 revision);
	bool applyBoundaryMove(const SpaceLayout::Packet& packet);
	void finishBoundaryMove(uint64 revision, const std::string& error);
	bool beginCellSplit(const SpaceLayout::Packet& packet, const Network::Channel& manager, bool candidate);
	bool applyCellSplit(const SpaceLayout::Packet& packet, const Network::Channel& manager);
	bool finishCellSplit(const SpaceLayout::Packet& packet, const Network::Channel& manager, bool cancel);
	void checkSplitMigrationTarget(const SpaceLayout::Packet& packet);
	bool isPendingSplitPeer(COMPONENT_ID cid) const;
	bool needsSplitPeer(COMPONENT_ID cid) const;
	void handleCellMerge(SpaceLayout::Packet& packet, Network::Channel& manager);
	bool isDrainingCell() const;
	bool isCellMergeHandoffStopped() const;
	void reportCellMergeBlock(ENTITY_ID entity, const char* error);
	const SpaceLayout::Node* handoffDestination(float x, float z, COMPONENT_ID source) const;
	bool isStagedSplitCell() const { return split_ && split_->candidate && !split_->applied; }
	bool canReceiveSplitGhosts() const
	{
		return split_ && split_->candidate && split_->applied &&
			layoutState_ == SpaceLayout::COMMITTING && isGood() && !layoutRemovalPending_;
	}
	SpaceLayout::State layoutState() const { return layoutState_; }
	const std::string& layoutError() const { return layoutError_; }
	bool hasManagedCell() const { return layoutPinned_; }
	bool isLayoutReplica() const { return layoutReplica_; }
	bool isPreparingReplacement() const { return replacementTemporary_; }
	bool isRecoveryTarget() const { return replacedCellApp_ != 0; }
	bool isCheckingReplacement() const { return replacementCheckVersion_ != 0; }
	Network::Channel* replacementManagerChannel() const;
	void bindReplacementManager(const Network::Channel& channel, const SpaceLayout::Packet& packet);
	bool canStageRecovery(const CellRecovery::StageKey& key) const;
	bool matchesRecovery(const CellRecovery::StageKey& key) const;
	enum class RecoveryInstallResult { Rejected, Installed, Busy };
	RecoveryInstallResult installRecoveryEntity(const CellRecovery::StageKey& key, COMPONENT_ID base, uint64 baseProcess,
		ENTITY_SCRIPT_UID type, uint64 routingEpoch, bool ownsSpace, const std::string& data);
	bool finishRecoveryEntity(const CellRecovery::StageKey& key, COMPONENT_ID base, uint64 baseProcess,
		bool activate, bool hasClient);
	bool checkReplacement(const SpaceLayout::Packet& packet);
	bool commitReplacement(const SpaceLayout::Packet& packet, Network::Channel* manager);
	void cancelReplacementCheck(uint64 version);
	bool layoutShutdownRequested() const { return layoutShutdownRequested_; }
	void markLayoutShutdownRequested() { layoutShutdownRequested_ = true; }
	bool canChangeSpaceResources() const;
	SpaceLayout::Resources layoutResources() const;
	bool prepareLayout(const SpaceLayout::Packet& packet, bool replica, std::string& error);
	void applyLayoutResources(const SpaceLayout::Resources& resources, uint64 revision, bool retryGeometry = false);
	void applyLayoutData(const std::string& key, const std::string& value,
		bool deleted, uint64 revision);
	void setLayoutState(SpaceLayout::State state, const std::string& error = "");
	void abortLayout(const std::string& error);
	void removeLayoutSpace() { layoutRemovalPending_ = true; }
	void updateLayoutPreparation();
	
	NavigationHandlePtr pNavHandle() const{ return pNavHandle_; }

	/**
		spaceData相关操作接口
	*/
	void setSpaceData(const std::string& key, const std::string& value);
	void delSpaceData(const std::string& key);
	bool hasSpaceData(const std::string& key);
	const std::string& getSpaceData(const std::string& key);
	void onSpaceDataChanged(const std::string& key, const std::string& value, bool isdel);
	static PyObject* __py_SetSpaceData(PyObject* self, PyObject* args);
	static PyObject* __py_GetSpaceData(PyObject* self, PyObject* args);
	static PyObject* __py_DelSpaceData(PyObject* self, PyObject* args);

	CoordinateSystem* pCoordinateSystem(){ return &coordinateSystem_; }

	bool isDestroyed() const{ return state_ == STATE_DESTROYED; }
	bool isGood() const{ return state_ == STATE_NORMAL; }
	bool canAcceptEntities() const
	{
		return isGood() && !layoutShutdownRequested_ && !layoutRemovalPending_ &&
			!isPreparingReplacement() && !replacementCheckVersion_ && !(split_ && split_->candidate);
	}
	bool canAcceptNewEntities() const { return canAcceptEntities() && !isDrainingCell(); }
	// 未提交迁移实体持有 Space 生命周期租约，脚本清理必须等待 BaseApp 完成路由切换。
	// Uncommitted migration Targets lease the Space lifetime until BaseApp completes the route switch.
	bool hasPendingMigrationEntities(bool includeSources = false) const;

protected:
	void _addSpaceDatasToEntityClient(const Entity* pEntity);
	void updateCellSplitPreparation();
	void updateCellSplitMigration();
	bool hasSplitSourceEntityToMigrate(Entity& entity, COMPONENT_ID target) const;
	void updateCellMerge();
	bool hasMergePendingEntity(const Entity& entity) const;

	void _clearGhosts();
	
	enum STATE
	{
		STATE_NORMAL = 0,
		STATE_DESTROYING = 1,
		STATE_DESTROYED = 2
	};

protected:
	// 这个space的ID
	SPACE_ID					id_;														

	// 创建这个space时用的实体脚本模块名称
	std::string					scriptModuleName_;

	// 这个space上的entity
	SPACE_ENTITIES				entities_;							

	// 是否加载过地形数据
	bool						hasGeometry_;

	// 每个space最多只有一个cell
	Cell*						pCell_;

	CoordinateSystem			coordinateSystem_;

	NavigationHandlePtr			pNavHandle_;

	// 代次用于拒绝路径切换后晚到的异步结果，加载状态用于阻止重复任务并让移动控制器等待。
	// The generation rejects late asynchronous results after path changes; the loading flag prevents duplicates and lets movement wait.
	uint64						geometryLoadGeneration_;
	bool						isGeometryLoading_;

	bool					hasNavigationBounds_;
	float					navigationMinimumX_;
	float					navigationMinimumZ_;
	float					navigationMaximumX_;
	float					navigationMaximumZ_;

	// spaceData, 只能存储字符串资源， 这样能比较好的兼容客户端。
	// 开发者可以将其他类型转换成字符串进行传输
	SPACE_DATA					datas_;

	int8						state_;
	
	uint64						destroyTime_;	
	SpaceLayout::Layout			layout_;
	SpaceLayout::Bounds			localCellBounds_;
	BoundaryGhosts				boundaryGhosts_;
	uint64						layoutVersion_ = 0;
	uint64						layoutPreparedVersion_ = 0;
	uint64						resourceRevision_ = 0;
	uint64						boundaryRevision_ = 0;
	uint64						pendingBoundaryRevision_ = 0;
	std::string					boundaryMoveError_;
	struct SplitPreparation
	{
		SpaceLayout::Layout layout;
		Network::Address manager;
		uint64 managerSession;
		uint64 attempt;
		COMPONENT_ID source;
		COMPONENT_ID target;
		uint64 readyRevision = 0;
		bool candidate = false;
		bool applied = false;
	};
	std::optional<SplitPreparation> split_;
	struct SplitMigrationScan
	{
		enum Stage { CheckingSource, WaitingForSource, CheckingTarget };
		uint64 attempt;
		COMPONENT_ID peer;
		Stage stage;
		size_t cursor = 0;
		uint64 nextCheck = 0;
	};
	std::optional<SplitMigrationScan> splitMigration_;
	struct CellMerge
	{
		enum Stage { Preparing, Ready, Draining, Receiving, Checking, Drained, Cancelling, Stopped, Removing };
		SpaceLayout::Layout nextLayout;
		COMPONENT_ID source;
		uint64 managerSession;
		uint64 attempt;
		Stage stage = Preparing;
		bool sourceLocal = false;
		bool warnedAboutController = false;
		size_t cursor = 0;
		uint64 nextCheck = 0;
		std::string error;
		bool errorSent = false;
	};
	std::optional<CellMerge> merge_;
	SpaceLayout::State			layoutState_ = SpaceLayout::NONE;
	std::string					layoutError_;
	bool						layoutPinned_ = false;
	bool						layoutReplica_ = false;
	bool						applyingLayoutResources_ = false;
	bool						layoutReadySent_ = false;
	bool						layoutPrepareReceived_ = false;
	bool						layoutShutdownRequested_ = false;
	bool						layoutRemovalPending_ = false;
	uint64						layoutStarted_ = 0;
	Network::Address			replacementManagerAddress_;
	uint64						replacementManagerSession_ = 0;
	COMPONENT_ID				replacedCellApp_ = 0;
	uint64						replacedLayoutVersion_ = 0;
	uint64						replacementCheckVersion_ = 0;
	bool						replacementTemporary_ = false;
	struct RecoveryInstallation
	{
		COMPONENT_ID base;
		uint64 baseProcess;
		EntityPtr entity;
		bool activated = false;
	};
	std::map<ENTITY_ID, RecoveryInstallation> recoveryInstallations_;
	size_t pendingRecoveryActivations_ = 0;
	bool						geometryShouldLoad_ = false;
	std::map<int, std::string>	geometryParams_;
};


}
#endif
