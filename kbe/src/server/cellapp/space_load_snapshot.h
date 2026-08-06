#ifndef KBE_CELLAPP_SPACE_LOAD_SNAPSHOT_H
#define KBE_CELLAPP_SPACE_LOAD_SNAPSHOT_H

#include <cstdint>

namespace KBEngine
{

struct SpaceLoadSnapshot
{
	void observe(std::uint32_t spaceID, std::uint64_t entities, std::uint64_t witnesses,
		std::uint64_t pendingWitnesses, std::uint64_t aoiRelations)
	{
		++spaceCount;
		totalEntities += entities;
		totalWitnesses += witnesses;
		totalPendingWitnesses += pendingWitnesses;
		totalAoiRelations += aoiRelations;
		if (entities > maxEntities)
		{
			maxEntities = entities;
			maxEntitiesSpaceID = spaceID;
		}
		if (witnesses > maxWitnesses)
		{
			maxWitnesses = witnesses;
			maxWitnessesSpaceID = spaceID;
		}
		if (pendingWitnesses > maxPendingWitnesses)
		{
			maxPendingWitnesses = pendingWitnesses;
			maxPendingWitnessesSpaceID = spaceID;
		}
		if (aoiRelations > maxAoiRelations)
		{
			maxAoiRelations = aoiRelations;
			maxAoiRelationsSpaceID = spaceID;
		}
	}

	std::uint64_t sampledTick = 0;
	std::uint64_t spaceCount = 0;
	std::uint64_t totalEntities = 0;
	std::uint64_t totalWitnesses = 0;
	std::uint64_t totalPendingWitnesses = 0;
	std::uint64_t totalAoiRelations = 0;
	std::uint64_t maxEntities = 0;
	std::uint64_t maxWitnesses = 0;
	std::uint64_t maxPendingWitnesses = 0;
	std::uint64_t maxAoiRelations = 0;
	std::uint32_t maxEntitiesSpaceID = 0;
	std::uint32_t maxWitnessesSpaceID = 0;
	std::uint32_t maxPendingWitnessesSpaceID = 0;
	std::uint32_t maxAoiRelationsSpaceID = 0;
};

}

#endif
