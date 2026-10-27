#pragma once

// The incremental update (ADR-0010 §10, specification §7.6, style §13.6, NFR-08; I3 step 9; per room since I4 step 1,
// ADR-0013 §5): a previous building brought to its systems' selections now, moving nothing that stands.
// world::updateBuilding is a pure function of its inputs, like world::generateBuilding, and returns a GenerationResult
// (the plan, its change report, its validation).
//
// The previous plan is of a schema this program reads: schema 2 as it is, schema 1 lifted first (lift.hpp). When it is
// of another seed, preset, generator major version, random algorithm or title-key rule, or a room's seed is not the
// world seed's, the update changes nothing: RequiresExplicitRegeneration, the previous plan (lifted) returned as it is
// with the reason (`worldplan generate --replace` regenerates after a backup). Otherwise each room whose system has a
// selection is updated with it, and every other room stays as it is:
//   - a room is KEPT, its contents untouched, when its template version is no longer the asset root's current one, its
//     collection fixtures' levels are no longer their prefabs', or its generator major is another: its placements and
//     unplaced entries stand as they were (a duplicate keeps its representative), the games of its selection that it
//     neither places nor lists are unplaced with reason room-kept (entries that follow the selection: an update with
//     the same selection changes nothing), and the report lists it with the reason (ChangeReport::keptRooms). The
//     room's recorded media family is authoritative: its data is read with it (loadRoomData()), so a data change never
//     freezes a room; its theme pack is a look, resolved on its floor and written into the plan by every update, for
//     every stored room (kept, held, updated or given no selection), never a reason to keep or regenerate it and no
//     change of the inputs for the provenance (ADR-0014 §1);
//   - in any other room every placement is kept as it is, also a game's that is now unavailable, no longer eligible
//     (its cover gone: it keeps its slot with a generated label), hidden, or gone from the catalog's snapshot: its slot
//     is never given to another game (free slots are never compacted);
//   - an alias (the catalog's identities, ADR-0008 §4: `aliases`, each placed GameId that now stands for another
//     identity) hands its slot to the identity it stands for when that one has none, else frees it; a face-out slot
//     freed so stays free, dressed;
//   - a title's representative is chosen once: a title of the selection that a placed game already represents (one of
//     its versions is placed, or the previous plan listed it as a duplicate of a placed game that is no title of the
//     selection now) is unplaced as duplicate-title of that game, and so are its other versions; a title the previous
//     plan left unplaced for capacity stays so until a regeneration;
//   - every other title of the selection is new. New titles are placed in the order (catalog sort key, title key,
//     GameId), each always in the spine-out section, never on a display fixture or in the hero band (the face-out set
//     changes only with a regeneration): in the first free slot after its alphabetical predecessor's (the spine-out
//     title just before it in that order) in the section's fill order (modules in the collection's order, each
//     module's spine-out levels in band order, slots from 0), else the first free slot of the section; with none free,
//     the first dressed spine-out level in that order (its dressing removed: LocalReflow); with none, a side-wall
//     module in the next side-wall socket of the collection's order that has none (the decoration there removed,
//     every level of the module dressed until titles take them; the wall-washers placed again); with no socket left,
//     the room grows by one step (RoomExpansion: the south wall and what stands on it move, the step's side-wall
//     sockets are decorated as a fresh room's are; the room moves away from its floor's hall, its x kept:
//     building.hpp's followDepth()); at the template's limit the title is unplaced for capacity;
//   - a new placement's offset comes from its seed as in a fresh plan; added modules, decoration and dressing are what
//     a fresh layout of the room would place there;
//   - pins (overrides.hpp) are protected: a side-wall socket whose fixture is pinned never takes a module (the module
//     goes to the next socket), a growth that would move a fixture pinned in its socket of the south wall is not taken
//     (the titles needing it are unplaced for capacity); both are reported in needsConfirmation, as are the pins the
//     plan does not honour (which the update never applies), and the orphaned pins in orphanedPins. Since I4b step 2
//     (ADR-0015 §3) a fixture pinned at a transform (moved by the user) stays where it stands when the room grows, its
//     light too, the wall-washers then placed again over the modules where they stand; a socket whose new module or
//     growth decoration would overlap a fixture standing there (one the user moved) takes none (a module goes to the
//     next socket, reported in needsConfirmation).
// An updated room's unplaced games are its selection's with their reasons (a duplicate's representative being the
// placed game that represents its title), every placed game keeps its placement and no entry besides; it records this
// generator's version. A system's selection goes to the room that holds its section (sticky: whatever its count now); a
// shared or arcade room is updated with the selections of all its sections (ADR-0013 §6), and one given only some of
// them is held as it is, as a room without a selection, and listed in the report's heldRooms with the sections it lacks
// (the plan does not say which section an unplaced game was of). A room whose system (or one of whose sections) has no
// selection stands as it is (SC-06: unknown is not empty).
// A system no room holds enters the building (ADR-0013 §3, §7; I4 step 6a) as it enters a fresh one, among the rooms
// that stand, in the order (floor order, systems.json order, system name):
//   - classified (classify(), with its platform tags), an arcade system takes a section of the arcade room, a system
//     with fewer than sharedRoomMinTitles distinct titles a section of its floor's shared room, any other its console
//     room;
//   - a section is appended at the end of its room when the room stands (its titles new titles of that room, placed
//     after the previous section's by the rules above), unless the room is given no selection of some section (held,
//     or not updated): the system then waits, its selection not read, and the report lists it (systemsWaiting); a room
//     kept as it is takes the section, its games room-kept;
//   - a new room (a console room, or the shared or arcade room its first section opens) is generated as a fresh
//     building generates it, and placed at the east end of the shorter row of its floor (a floor that held no room
//     appears), its x fixed, its z following its depth;
//   - a console room its floor has no place for joins the floor's shared room when that stands, else the system's
//     games are unplaced for capacity, naming the room it would have had (expansion rooms are I4b's), as are those of a
//     shared or arcade room that finds no place; when such a system enters again, the entries naming its console room
//     give way to its entry now (all of them: a game that left its selection has none), those naming a shared or arcade
//     room that found no place only for the games of its selection (the plan does not say which system such an entry
//     was of: an entry of a game that left the system stands until a regeneration).
// Nothing that stands moves: the stored rooms' floors, kinds, x, yaws and sections (new ones only appended), their
// placements, and the hall's lights (derived, only appended as a floor's rooms reach further east).
// The one exception (I6 step 4, ADR-0014 §2): a stored room whose recorded media family has a successor in rooms of
// its kind (media_family.hpp's "succeeds" and "succeedsIn"), given a selection of every section and kept by none of
// the rules above, is MIGRATED: regenerated as a fresh building generates it, with the data of its family's last
// successor there (its data's WorldData::successor: loadRoomData() reads it), its id, kind, floor, x, yaw, sections
// and seed kept, its face-out titles chosen again by the same priority, its z following its new depth either way (the
// room deeper or shallower than before, its doorway wall still on the hall), its record naming the successor and this
// generator's version; the report lists it (migratedRooms, {room, from, to, the titles its new room holds no slot for:
// unplaced for capacity}) and its games not placed before among the added. A room a pin names (a game pinned to one of
// its slots, one of its fixtures pinned) is not migrated: it is updated as it stands and the refusal is in
// needsConfirmation. Each room migrates at most once per succession: its new family has no successor in rooms of its
// kind, so the next update of an unchanged catalog writes nothing (the arcade room keeps generic_case, which I6 step
// 6's arcade_cabinet succeeds there: its own migration, once).
// The second exception (I6 step 5, ADR-0014 §3): the ground floor's rooms that stand at I4's lobby (their doorway wall
// not on the hub's edge) are re-placed onto the hub once, before any system enters, by building.hpp's placeOnHub(): new
// x and z, their yaw and contents unchanged in their own frames; the report lists them (replacedRooms, {room, from,
// to}), the rebuild level is full, the inputDigest is computed again (never kept) and the plan is written. The next
// update finds them on the hub and moves nothing. A room whose own row on the hub is full is never put in the other
// row: the update then refuses (RequiresExplicitRegeneration, its reason naming the rooms) and writes nothing.
// The plan records the highest of its rooms' generator versions, the selections' sort-key version and the inputDigest
// of the inputs read, the pins it honours joined (generationInputDigest(), over the rooms given a selection and the
// systems that entered, joined by the honoured pins' term as world_edit.hpp says; an update
// that gives some room no selection and changes nothing keeps the previous inputDigest, which named more), so that an
// update of an unchanged catalog gives the previous plan byte for byte, whichever of its rooms it is given, and an
// update of the building of no room equals the fresh building of its selections. The change report lists the games
// added, the placements kept, the games newly unplaced, the fixtures added, removed and moved, the dressing removed,
// the slots freed, the pins, the rooms kept and held, the rooms added, the systems that entered and those that wait,
// the rebuild level (NoRebuild when no placement, fixture, light or dressing changed; a slot handed to an alias's
// identity is a texture swap) and the digests before (the stored file's SHA-256: previousDigest) and after; the
// validation is validatePlan()'s as for a fresh plan, the pins it judges being the honoured and orphaned ones, the
// rooms kept judged as kept.

#include <map>
#include <span>
#include <string>
#include <vector>

#include <rv/catalog/ids.hpp>
#include <rv/core/expected.hpp>
#include <rv/world/building.hpp>
#include <rv/world/generation.hpp>
#include <rv/world/overrides.hpp>
#include <rv/world/room_selection.hpp>
#include <rv/world/world_config.hpp>
#include <rv/world/world_plan.hpp>

namespace rv::world {

/// A GameId that now stands for another identity (catalog::StoredAlias: `alias` became an alias of `target`).
struct GameAlias {
    catalog::GameId alias;
    catalog::GameId target;

    auto operator<=>(const GameAlias&) const = default;
};

/// What an incremental update of one system's room reads.
struct UpdateInput {
    catalog::LibraryId library;
    /// The room's selection now (world::selectRoom of the catalog's snapshot); while the media are unknown there is
    /// none, and no update runs (ADR-0010 §3: unknown media change nothing).
    RoomSelection selection;
    WorldConfig config;
    WorldOverrides overrides;
    /// The plan to update, as readPlan() returned it (schema 1 or 2), and the SHA-256 of the file it was read from (its
    /// digest; empty: computed with planDigest(), which a schema-1 plan cannot have).
    WorldPlan previous;
    std::string previousDigest;
    /// The placed GameIds that became aliases (the caller asks the catalog for the placed games its snapshot lacks).
    std::vector<GameAlias> aliases;
};

/// What an incremental update of a building reads: UpdateInput's with a selection per system to update or to enter
/// (each of its system's room:<system>) and the systems' platform tags.
struct BuildingUpdateInput {
    catalog::LibraryId library;
    std::vector<RoomSelection> selections;
    std::map<catalog::SystemName, std::vector<std::string>> platformTags;
    WorldConfig config;
    WorldOverrides overrides;
    WorldPlan previous;
    std::string previousDigest;
    std::vector<GameAlias> aliases;
};

/// Why a stored room is kept as it is (ADR-0013 §5), or nothing: its template (by id and version) is not `data`'s, a
/// collection fixture's levels are not its prefab's as `data` measured them, or its generator major is not this one's.
[[nodiscard]] std::string roomKeepReason(const WorldPlan& plan, const Room& room, const WorldData& data);

/// The previous building updated by the rules of the file's header; `rooms` holds the data of every room of the
/// previous plan (loadRoomData(): its recorded family and template, with the data of its migration when that family
/// has a successor: WorldData::successor), by its system, and of every system
/// entering the building (loadWorldData() of the system with its platform tags: its console room's with the room its
/// section would stand in). Errors: InvalidArgument for a previous plan of another library, a selection that is not of
/// a system's room, two selections of one system, a room without data in `rooms` or with data of another family than
/// its recorded one, a system entering without its own data or excluded, a schema-1 plan without its file's digest or
/// that cannot be lifted, the inputs generateBuilding() refuses; Unsupported for a previous plan of a newer schema or
/// generator version (written by a newer Retroverse); those of the layout (a side-wall module that does not fit its
/// socket: ValidationFailed).
[[nodiscard]] Expected<GenerationResult> updateBuilding(const BuildingUpdateInput& input, const BuildingData& building,
                                                        const SystemsData& systems, std::span<const WorldData> rooms);

/// The update of the selection's one system (updateBuilding() with `data`'s building, systems and room data: the
/// previous plan holds that room only, or its other rooms lack data).
[[nodiscard]] Expected<GenerationResult> update(const UpdateInput& input, const WorldData& data);

} // namespace rv::world
