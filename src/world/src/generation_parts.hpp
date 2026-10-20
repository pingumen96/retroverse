#pragma once

// Private to rv_world (I3 step 9, I4 step 1): what the fresh generation (generation.cpp) and the incremental update
// (incremental.cpp) share: the room-level generation beneath the building, the checks of their inputs, the building's
// input digest and the context the validation judges their plans in.

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <rv/core/expected.hpp>
#include <rv/world/building.hpp>
#include <rv/world/generation.hpp>
#include <rv/world/overrides.hpp>
#include <rv/world/validation.hpp>
#include <rv/world/world_plan.hpp>

namespace rv::world::detail {

/// The prefabs' slot levels as the generator measured them, for the validation (ValidationContext::prefabSlots): the
/// first of `catalogs` that holds a prefab answers. The catalogs must outlive the source.
[[nodiscard]] PrefabSlotSource catalogSlots(std::vector<const PrefabCatalog*> catalogs);

/// The data of `system`'s room among `rooms` (by its platform's system), or null.
[[nodiscard]] const WorldData* dataOf(std::span<const WorldData> rooms, const catalog::SystemName& system) noexcept;

/// The selection of the data's room, no game both a title and unplaced and, unless the data carries a stored room's
/// recorded family (`recordedFamily`), the platform's media family. Errors: InvalidArgument naming the problem.
[[nodiscard]] Status checkRoomInput(const RoomSelection& selection, const WorldData& data, bool recordedFamily);

/// What the building's inputDigest hashes (generation.hpp's generationInputDigest()).
struct BuildingDigestInput {
    WorldConfig config;
    catalog::LibraryId library;
    std::uint64_t seed = 0;
    BuildingData building;
    SystemsData systems;
    std::map<catalog::SystemName, std::vector<std::string>> platformTags;
    /// The rooms generated or updated, by room id: each one's selection and data.
    std::vector<std::pair<const RoomSelection*, const WorldData*>> rooms;
    /// False: the selections' documents are taken without the reader's check (the update checks them itself, for its
    /// time budget); the same digest.
    bool checkedSelections = true;
};
[[nodiscard]] Expected<std::string> buildingInputDigest(const BuildingDigestInput& input);

/// What a building generated or updated is judged against: the rooms' templates, the configuration, the selections,
/// the prefabs' slots as measured, the plan's lights per room, the pins given, the building and the rooms kept.
[[nodiscard]] ValidationContext buildingValidationContext(const WorldPlan& plan, const WorldConfig& config,
                                                          std::span<const RoomSelection> selections,
                                                          std::span<const WorldData* const> rooms,
                                                          const BuildingData& building, const WorldOverrides& pins,
                                                          std::vector<KeptRoom> keptRooms);

/// One system's room as I3's room-level rules generate it (ADR-0010 §3 to §8): a plan of that one room (its record
/// with the template's fields, without its building fields) with its fixtures, placements, dressing, lights and
/// unplaced games (the selection's, and the titles over the template's capacity). Errors: those of the layout.
[[nodiscard]] Expected<WorldPlan> generateRoomContent(const RoomSelection& selection, const WorldConfig& config,
                                                      std::uint64_t worldSeed, const WorldData& data);

/// Moves the room and the elements of `room` (a one-room plan) into `plan`.
void appendRoom(WorldPlan& plan, WorldPlan&& room);

/// The plan's floors: the ground floor and every room's, by order.
void setFloors(WorldPlan& plan, const BuildingData& building);

/// The highest of the rooms' generator versions (kGeneratorVersion for a plan without a room).
[[nodiscard]] GeneratorVersion highestGeneratorVersion(const WorldPlan& plan);

} // namespace rv::world::detail
