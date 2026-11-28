#ifndef KBE_CELL_HANDOFF_SNAPSHOT_H
#define KBE_CELL_HANDOFF_SNAPSHOT_H

#include "entitydef/common.h"
#include "math/math.h"
#include "server/cell_handoff.h"
#include "server/space_layout.h"

namespace KBEngine::CellHandoff
{
// CellApp 可能退役后重新加入同一布局，用 Cell 编号区分这两次成员关系。
using CopyPeers = std::vector<std::pair<COMPONENT_ID, CELL_ID>>;

class CopyReservation;

// 所有 Space 共用 CellApp 间的连接。普通刷新有独立水位，每条连接另外允许
// 四份在途交接首包。预留按最大快照计算，因为脚本在目标安装期间仍可能改变属性。
class CopyBudget
{
public:
	static constexpr size_t MAX_STATE = 256 * 1024;
	static constexpr size_t HEADER = 64;
	static constexpr size_t ORDINARY_BYTES = 512 * 1024;
	static constexpr size_t SLOT = MAX_STATE + HEADER;
	static constexpr size_t SLOTS = 4;

	template<class QueuedBytes> std::unique_ptr<CopyReservation> tryReserve(CopyPeers peers, QueuedBytes queued);

private:
	friend class CopyReservation;
	std::map<COMPONENT_ID, size_t> reserved_;
};

class CopyReservation
{
public:
	CopyReservation(const CopyReservation&) = delete;
	CopyReservation& operator=(const CopyReservation&) = delete;
	const CopyPeers peers;
	~CopyReservation()
	{
		for (const auto& peer : peers)
		{
			const auto it = budget_.reserved_.find(peer.first);
			if (--it->second == 0)
				budget_.reserved_.erase(it);
		}
	}
private:
	friend class CopyBudget;
	CopyReservation(CopyBudget& budget, CopyPeers cells) : peers(std::move(cells)), budget_(budget)
	{
		for (const auto& peer : peers)
			++budget_.reserved_[peer.first];
	}
	CopyBudget& budget_;
};

template<class QueuedBytes>
std::unique_ptr<CopyReservation> CopyBudget::tryReserve(CopyPeers peers, QueuedBytes queued)
{
	for (const auto& peer : peers)
	{
		const auto it = reserved_.find(peer.first);
		const size_t held = it == reserved_.end() ? 0 : it->second;
		if (held >= SLOTS || queued(peer.first) > ORDINARY_BYTES + (SLOTS - held - 1) * SLOT)
			return nullptr;
	}
	return std::unique_ptr<CopyReservation>(new CopyReservation(*this, std::move(peers)));
}

inline bool readCopyPeers(BoundedStreamReader& reader, const Route& route, CopyPeers& peers)
{
	uint8 count = 0;
	if (!reader.read(count) || count > SpaceLayout::MAX_CELLS - 2)
		return false;
	COMPONENT_ID previous = 0;
	for (uint8 i = 0; i < count; ++i)
	{
		COMPONENT_ID app = 0;
		CELL_ID cell = 0;
		if (!reader.read(app) || !reader.read(cell) || !cell || app <= previous ||
			app == route.source || app == route.target)
			return false;
		peers.emplace_back(app, cell);
		previous = app;
	}
	return true;
}

// 接收端按整个消息体重组。快照必须写进已有消息头的流，写入成功后源端才可降级。
template <class WriteSnapshot>
size_t writeSnapshotBody(MemoryStream& body, ENTITY_ID id, ENTITY_SCRIPT_UID type,
	const Position3D& pos, const Direction3D& dir, COMPONENT_ID base, uint64 generation,
	const Route& route, const PeerProcesses& processes, const CopyPeers& peers, WriteSnapshot writeSnapshot)
{
	body << id << type << pos.x << pos.y << pos.z << dir.roll() << dir.pitch() << dir.yaw()
		<< base << generation;
	route.write(body);
	if (!base)
		processes.write(body);
	body << static_cast<uint8>(peers.size());
	for (const auto& peer : peers)
		body << peer.first << peer.second;
	const size_t offset = body.wpos();
	writeSnapshot(body);
	return offset;
}
}
#endif
