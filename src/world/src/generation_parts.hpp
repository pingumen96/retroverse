#pragma once

// Private to rv_world (I3 step 9, I4 step 1): what the fresh generation (generation.cpp) and the incremental update
// (incremental.cpp) share: the room-level generation beneath the building, the systems' entry into the building (I4
// step 6a), the checks of their inputs, the building's input digest and the context the validation judges their plans
// in.

#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <rv/catalog/ids.hpp>
#include <rv/core/expected.hpp>
#include <rv/world/building.hpp>
#include <rv/world/generation.hpp>
#include <rv/world/overrides.hpp>
#include <rv/world/regeneration.hpp>
#include <rv/world/validation.hpp>
#include <rv/world/world_plan.hpp>

#include "allocation.hpp"

namespace rv::world::detail {

/// The prefabs' slot levels as the generator measured them, for the validation (ValidationContext::prefabSlots): the
/// first of `catalogs` that holds a prefab answers. The catalogs must outlive the source.
[[nodiscard]] PrefabSlotSource catalogSlots(std::vector<const PrefabCatalog*> catalogs);

/// The data of `system`'s room among `rooms` (by its platform's system), or null.
[[nodiscard]] const WorldData* dataOf(std::span<const WorldData> rooms, const catalog::SystemName& system) noexcept;

/// The selection of the data's room (unless `anySection`: a shared or arcade room's data is its first section's, and
/// the selection one of its sections'), no game both a title and unplaced and, unless the data carries a stored room's
/// recorded family (`recordedFamily`), the platform's media family. Errors: InvalidArgument naming the problem.
[[nodiscard]] Status checkRoomInput(const RoomSelection& selection, const WorldData& data, bool recordedFamily,
                                    bool anySection = false);

/// The selection of a shared or arcade room `room` over its sections' (ADR-0013 §6): every section's titles in priority
/// order (the room's face-out titles are the first D + B of them), every section's unplaced games naming `room`, the
/// distinct titles summed; `index` gets each title's section, its system's place in `sections`.
[[nodiscard]] RoomSelection sectionsSelection(const RoomId& room, std::span<const catalog::SystemName> sections,
                                              std::span<const RoomSelection* const> selections, SectionIndex& index);

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

/// The overrides' part of a plan's inputDigest (ADR-0015 §2, I4b step 2): the SHA-256 of the canonical form of the
/// pins `honoured` holds, provenance left out ({"fixturePins": [{"fixture", "prefab"?, "transform"?}], "gamePins":
/// [{"game", "slot"}]}, each list sorted), as 64 hexadecimal digits; empty when it holds no pin. Errors: those of
/// core::canonicalJsonDigest().
[[nodiscard]] Expected<std::string> overridesDigestTerm(const WorldOverrides& honoured);
/// `digest` with `term` joined (each of the 32 bytes XORed): `digest` itself for an empty term, so a plan of a world
/// without overrides keeps its digest byte for byte; joining a term twice takes it out again, so an edit replaces the
/// honoured pins' term of a plan's inputDigest without the inputs (I4b step 2). Errors: InvalidArgument for a digest or
/// a term that is not 64 lowercase hexadecimal digits.
[[nodiscard]] Expected<std::string> joinDigestTerm(std::string_view digest, std::string_view term);
/// joinDigestTerm(digest, overridesDigestTerm(honoured)).
[[nodiscard]] Expected<std::string> joinOverridesDigest(std::string_view digest, const WorldOverrides& honoured);

/// What a building generated or updated is judged against: the rooms' templates, the configuration, the selections,
/// the prefabs' slots as measured, the plan's lights per room, the pins given, the building and the rooms kept.
[[nodiscard]] ValidationContext buildingValidationContext(const WorldPlan& plan, const WorldConfig& config,
                                                          std::span<const RoomSelection> selections,
                                                          std::span<const WorldData* const> rooms,
                                                          const BuildingData& building, const WorldOverrides& pins,
                                                          std::vector<KeptRoom> keptRooms);

/// A regeneration's pins of one room (I4b step 5, ADR-0015 §3; regeneration.hpp).
struct RoomPins {
    /// The game pins whose slot is in the room, and the fixture pins of its fixtures.
    std::vector<GamePin> games;
    std::vector<FixturePin> fixtures;
    /// The previous plan: its fixtures give a pinned fixture's role, its prefab (a decoration's) and its lift, its room
    /// record the south wall a hung decoration followed; null: none.
    const WorldPlan* previous = nullptr;
    /// The fixture pins a previous attempt found breaking a hard rule (the regeneration's `invalid`): not applied.
    std::set<FixtureId> excluded;

    [[nodiscard]] bool empty() const noexcept { return games.empty() && fixtures.empty(); }
};

/// What honouring a room's pins found: the pins it could not honour, and the transforms of the pinned hung
/// decorations that followed the south wall (their pins rewritten), by fixture.
struct RoomPinOutcome {
    std::vector<OverrideConflict> conflicts;
    std::map<FixtureId, FixtureTransform> followed;
};

/// One room as I3's room-level rules generate it (ADR-0010 §3 to §8): a plan of that one room (its record with the
/// template's fields, without its building fields) with its fixtures, placements, dressing, lights and unplaced games
/// (the selection's, and the titles over the template's capacity); a shared or arcade room's collection ordered by
/// `sections` first (sectionsSelection()). With `pins` (a regeneration's, not empty) the room honours them
/// (regeneration.hpp's rules 1 to 4: generatePinnedRoom()), what it could not honour in `outcome`. Errors: those of the
/// layout.
[[nodiscard]] Expected<WorldPlan> generateRoomContent(const RoomSelection& selection, const WorldConfig& config,
                                                      std::uint64_t worldSeed, const WorldData& data,
                                                      const SectionIndex* sections = nullptr,
                                                      const RoomPins* pins = nullptr,
                                                      RoomPinOutcome* outcome = nullptr);

/// generateRoomContent() of a room with pins (pinned_room.cpp).
[[nodiscard]] Expected<WorldPlan> generatePinnedRoom(const RoomSelection& selection, const WorldConfig& config,
                                                     std::uint64_t worldSeed, const WorldData& data,
                                                     const SectionIndex* sections, const RoomPins& pins,
                                                     RoomPinOutcome& outcome);

/// A system entering the building (ADR-0013 §3, §7): its selection, its own data (loadWorldData()'s: its console
/// room's, with the data of the shared or arcade room it would take a section of), its classification and the order of
/// its floor.
struct EnteringSystem {
    const RoomSelection* selection = nullptr;
    const WorldData* data = nullptr;
    Classification classification;
    std::uint32_t floorOrder = 0;
};

/// The entering system of `selection` (classify() with its platform tags in `tags`), or why it cannot enter:
/// InvalidArgument for a selection that is not of a system's room, a system without its own data in `rooms`, an
/// excluded system or one on no floor, the inputs checkRoomInput() refuses.
[[nodiscard]] Expected<EnteringSystem> enteringSystem(
    const RoomSelection& selection, std::span<const WorldData> rooms,
    const std::map<catalog::SystemName, std::vector<std::string>>& tags, const SystemsData& systems,
    const BuildingData& building);

/// A room of the building while systems enter it.
struct BuildingRoom {
    RoomId id;
    RoomKind kind = RoomKind::Console;
    std::string floor;
    /// The data it is generated or updated from: a new console room's system's, a new shared or arcade room's first
    /// section's WorldData::sharedRoom, a stored room's own (loadRoomData()).
    const WorldData* data = nullptr;
    /// Its record as it stands on its floor: a stored room's as stored; a new room's at the template's base size, placed
    /// when it entered (its width, which places the rooms after it, never changes; its z follows its depth once it is
    /// generated: generateEnteredRoom()).
    Room record;
    /// It stood in the previous plan.
    bool stored = false;
    /// A stored shared or arcade room that takes no section now: the update holds it (a section without a selection).
    bool closed = false;
    /// The systems that entered it, in the order they entered: a new room's sections, a stored room's new ones.
    std::vector<const EnteringSystem*> entered;
};

/// What the entry of systems into a building decided (enterBuilding()).
struct BuildingEntry {
    /// The stored rooms as given, then the new rooms in the order they entered.
    std::vector<BuildingRoom> rooms;
    /// The games of the systems their floor had no place for, unplaced for capacity, naming the room each would have
    /// had (ADR-0013 §7).
    std::vector<UnplacedGame> capacity;
    /// The systems whose shared or arcade room is closed, with that room: they do not enter.
    std::vector<std::pair<const EnteringSystem*, RoomId>> waiting;
    /// Every system that entered or found no place, with the data of the room it stands in (or would have had), for
    /// the building's input digest.
    std::vector<std::pair<const RoomSelection*, const WorldData*>> digest;
};

/// Enters `entering` (sorted here into the order floor order, systems.json order, system name) into a building whose
/// rooms are `stored`: an arcade system takes a section of the arcade room on the ground floor; a system with fewer
/// than config.sharedRoomMinTitles distinct titles a section of its floor's shared room; any other its console room. A
/// section is appended to its room when the room stands (a closed one makes the system wait), else the room enters its
/// floor with that first section; a new room is placed by placeEnteringRoom() over the rooms standing then (the stored
/// ones and those that entered before it); a console room its floor has no place for joins the floor's shared room
/// when it stands, else its games are unplaced for capacity, as are those of a shared or arcade room that finds no
/// place. `entering` must outlive the result. Errors: InvalidArgument for a shared or arcade room without its data
/// (WorldData::sharedRoom).
[[nodiscard]] Expected<BuildingEntry> enterBuilding(std::vector<BuildingRoom> stored,
                                                    std::vector<EnteringSystem>& entering, const WorldConfig& config,
                                                    const BuildingData& building);

/// A new room of an entry generated by I3's room-level rules (ADR-0010 §3 to §8) over its sections' selections (a
/// shared or arcade room's ordered by section, sectionsSelection()), as a plan of that one room: its record with its
/// kind, floor, sections, this generator's version and its placement, its z following its depth (followDepth()).
/// Errors: those of the layout.
[[nodiscard]] Expected<WorldPlan> generateEnteredRoom(const BuildingRoom& room, const WorldConfig& config,
                                                      std::uint64_t worldSeed, const BuildingData& building,
                                                      const RoomPins* pins = nullptr,
                                                      RoomPinOutcome* outcome = nullptr);

/// Moves the room and the elements of `room` (a one-room plan) into `plan`.
void appendRoom(WorldPlan& plan, WorldPlan&& room);

/// The plan's floors: the ground floor and every room's, by order.
void setFloors(WorldPlan& plan, const BuildingData& building);

/// The highest of the rooms' generator versions (kGeneratorVersion for a plan without a room).
[[nodiscard]] GeneratorVersion highestGeneratorVersion(const WorldPlan& plan);

} // namespace rv::world::detail
