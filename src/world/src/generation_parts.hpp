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
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

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
/// The SHA-256 of the canonical text of the inputs' document (buildingInputDocument()), written straight from the
/// inputs (writeBuildingInputText()) unless they hold what only the document can name (then through it, as before
/// m3/preview-time), or a ScopedDocumentDigests is alive (plan_json.hpp).
[[nodiscard]] Expected<std::string> buildingInputDigest(const BuildingDigestInput& input);
/// The inputs' document. Errors: those of roomSelectionToJson() (the reader's check of a selection) and of the
/// documents of the configuration and the building.
[[nodiscard]] Expected<nlohmann::json> buildingInputDocument(const BuildingDigestInput& input);
/// The canonical text of buildingInputDocument() appended to `out` straight from the inputs: the selections' titles
/// and unplaced games, most of the text, written without their documents, each room's data and the building's as
/// documents (one per room data, however many systems share it). Errors (the caller then takes the document's path,
/// which names the problem or makes the same digest): a selection that checkSelectionForm() refuses, a string that is
/// not UTF-8.
[[nodiscard]] Status writeBuildingInputText(const BuildingDigestInput& input, std::string& out);

/// The form of a selection as the reader's check of its document (roomSelectionToJson()) would find it, without the
/// document: a rule name and a sort-key version, the titles valid and in strict priority order with no game twice,
/// their ratings 0 to 10, at least as many distinct titles as titles, the unplaced games valid, of the room, sorted
/// strictly, a representative exactly for a duplicate (the incremental update's check of its inputs since I3 step 9).
/// It refuses what the reader refuses, and unplaced games out of order (which the document sorts first). Errors:
/// InvalidArgument naming the problem, with `context`.
[[nodiscard]] Status checkSelectionForm(const RoomSelection& selection, std::string_view context);

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
    /// Why the chain's rule 4 (chainShares()) could not make a pinned game a title, by game: the conflict's message
    /// then, measured against the chain's whole selection (an expansion's share lists no unplaced game).
    std::map<catalog::GameId, std::string> chainWhy;

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
    /// A new room's expansions (I4b step 7, ADR-0015 §7), by number: each record at the template's base size, placed
    /// when it entered its floor (placeExpansions()), its id the chain's next and `expands` the room.
    std::vector<Room> expansions;
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

// --- Expansion chains (I4b step 7, ADR-0015 §7; generator 1.5) -------------------------------------------------------

/// The most titles one room of `data` holds with the reserve, at its template's growth limit (countCollection()'s
/// largest count that fits). Errors: those of collectionCapacity().
[[nodiscard]] Expected<std::uint64_t> roomTitleLimit(const WorldData& data, const WorldConfig& config);

/// The fewest rooms that hold `titles` titles, each holding at most `limit`: 1 for none, or when a room holds none.
[[nodiscard]] std::uint32_t chainRoomsFor(std::uint64_t titles, std::uint64_t limit) noexcept;

/// A room record entering a floor at its template's base size (its width, which places the rooms after it, never
/// changes; its z follows its depth once it is generated).
[[nodiscard]] Room enteringRecord(const RoomId& id, RoomKind kind, const std::string& floor,
                                  std::vector<catalog::SystemName> sections, const RoomTemplate& roomTemplate);

/// The record of expansion `index` of `head` (a room of the chain, as it stands), at the template's base size, of the
/// head's kind, floor and sections, `expands` the head, not yet placed. Errors: InvalidArgument for an index past
/// kMaxRoomIndex.
[[nodiscard]] Expected<Room> expansionRecord(const Room& head, std::uint32_t index, const RoomTemplate& roomTemplate);

/// The numbers a chain's expansions take (ADR-0015 §7 as amended on 2026-10-28 and 2026-10-29): first those of the
/// chain's expansions standing in the plan a regeneration replaces, lowest first, then the numbers after the highest
/// of those and of every room of the chain that the overrides name (a pin's room, whatever conflict it records), so
/// that a number the overrides still name never comes back while they name it. The plan's schema holds no record of
/// a dropped room: the overrides are the only memory of one.
struct ChainNumbering {
    /// By head: the numbers of the chain's expansions standing in the previous plan, ascending.
    std::map<RoomId, std::vector<std::uint32_t>> standing;
    /// By head: the highest number of the chain's rooms the overrides name (1: the head only).
    std::map<RoomId, std::uint32_t> named;

    /// The numbers of `count` expansions of the chain `head`: its standing ones first, then fresh ones (next()).
    [[nodiscard]] std::vector<std::uint32_t> numbers(const RoomId& head, std::uint32_t count) const;
    /// The number of a fresh expansion of the chain `head` whose highest room stands at `highest`: after the highest
    /// of `highest`, the chain's standing numbers and its named ones.
    [[nodiscard]] std::uint32_t next(const RoomId& head, std::uint32_t highest) const;
};

/// The numbering of chains with `overrides` (their rooms named) and the expansions standing in `previous` (null: no
/// plan replaced).
[[nodiscard]] ChainNumbering chainNumbering(const WorldOverrides& overrides, const WorldPlan* previous = nullptr);

/// Refuses a plan holding an expansion whose head it lacks (InvalidArgument, `context` the operation): an update or a
/// regeneration works on chains by their heads and could not tell what the expansion is of. Only a plan edited by hand
/// holds one: every generator writes a chain whole.
[[nodiscard]] Status checkChainHeads(const WorldPlan& plan, std::string_view context);

/// Whether a pin's recorded conflict says its room was gone (room-gone): a stale pin, which an update and a
/// regeneration never apply (it refuses no growth, blocks no module and pulls no title into a room of that id that
/// stands again), though it stays in the overrides, and names its room for the numbering, until the user drops it.
[[nodiscard]] bool isStalePin(const GamePin& pin) noexcept;
[[nodiscard]] bool isStalePin(const FixturePin& pin) noexcept;

/// The expansions the new rooms of `entry` need (each its chainRoomsFor() over its sections' titles), numbered by
/// `numbering`, placed once every system entered, room after room in the entry's order and by number, each at the east
/// end of the shorter row of its floor among the rooms standing then (placeEnteringRoom(); `others`: rooms that stand
/// besides the entry's). On a full floor a chain takes the expansions that found a place and no more. Errors: those of
/// roomTitleLimit().
[[nodiscard]] Status placeExpansions(BuildingEntry& entry, const WorldConfig& config, const BuildingData& building,
                                     const ChainNumbering& numbering, std::span<const Room> others = {});

/// The pins of a regeneration's chains (regeneration.hpp's rules): `pins` the scope's to honour, `previous` the plan
/// regenerated (the pinned fixtures' roles and prefabs), `excluded` the fixture pins an earlier attempt found invalid.
/// A stale pin (isStalePin()) is never among them.
struct ChainPins {
    const WorldOverrides* pins = nullptr;
    const WorldPlan* previous = nullptr;
    std::set<FixtureId> excluded;

    /// The pins of `room` among them.
    [[nodiscard]] RoomPins of(const RoomId& room) const;
};

/// The titles of a chain's merged selection split among its rooms (ADR-0015 §7): each room's quota an equal share of
/// the titles (the first shares one title more when they do not divide), a room whose pinned games outnumber its share
/// taking them all and the other rooms sharing the rest equally; a pinned game a title of the room its pin names (the
/// pin wins, rule 4 of regeneration.hpp applied over the chain first, `why` getting why it could not, by game), and
/// the other titles filling the quotas left in the collection's order (section, catalog sort key, title key, GameId),
/// room after room. The quotas never ask more of a room past its pins than an equal share, so no room's share passes
/// the limit the chain's room count was computed with because of a pin elsewhere. Each share is in priority order, of
/// its room (`rooms`, head first); only the head's lists the selection's unplaced games.
[[nodiscard]] std::vector<RoomSelection> chainShares(const RoomSelection& merged, SectionIndex& index,
                                                     std::span<const RoomId> rooms, const ChainPins* pins,
                                                     std::map<catalog::GameId, std::string>* why = nullptr);

/// A chain generated by I3's room-level rules over `merged` (a console room's selection; a shared or arcade room's
/// over its sections, sectionsSelection(), `index` its titles' sections; null for a console room): `records` its rooms
/// as they stand or entered (the head first, then its expansions by number, each placed), as one plan of the chain's
/// rooms. A room alone is generateRoomContent()'s room; a chain is shared out by chainShares(), each room sized to its
/// share and choosing its own face-out titles from it by priority. Each record takes the head's kind and floor,
/// `sections`, this generator's version, its placement (its z following its depth) and `expands` (the head, for an
/// expansion); every unplaced entry names the head. With `pins` each room honours its own (generatePinnedRoom()).
/// Errors: InvalidArgument for no record; those of the layout.
[[nodiscard]] Expected<WorldPlan> generateChain(const RoomSelection& merged, const SectionIndex* index,
                                                std::span<const Room> records,
                                                const std::vector<catalog::SystemName>& sections,
                                                const WorldData& data, const WorldConfig& config,
                                                std::uint64_t worldSeed, const BuildingData& building,
                                                const ChainPins* pins = nullptr, RoomPinOutcome* outcome = nullptr);

/// A new room and its expansions (`room.expansions`) generated by I3's room-level rules over its sections' selections,
/// as one plan of the chain's rooms (generateChain(); a room alone as generateEnteredRoom() generates it). Errors:
/// those of the layout.
[[nodiscard]] Expected<WorldPlan> generateEnteredChain(const BuildingRoom& room, const WorldConfig& config,
                                                       std::uint64_t worldSeed, const BuildingData& building,
                                                       const ChainPins* pins = nullptr,
                                                       RoomPinOutcome* outcome = nullptr);

/// Rule 4 of regeneration.hpp over `selection`: the pinned `game`, a duplicate of its title, takes the title's place
/// (the representative unplaced as its duplicate); false with `why` when it cannot (pinned_room.cpp).
[[nodiscard]] bool takeRepresentation(RoomSelection& selection, SectionIndex& index, const catalog::GameId& game,
                                      const std::set<catalog::GameId>& pinnedGames,
                                      std::set<catalog::GameId>& titleGames, std::string& why);

/// Moves the room and the elements of `room` (a one-room plan) into `plan`.
void appendRoom(WorldPlan& plan, WorldPlan&& room);

/// The plan's floors: the ground floor and every room's, by order.
void setFloors(WorldPlan& plan, const BuildingData& building);

/// The highest of the rooms' generator versions (kGeneratorVersion for a plan without a room).
[[nodiscard]] GeneratorVersion highestGeneratorVersion(const WorldPlan& plan);

} // namespace rv::world::detail
