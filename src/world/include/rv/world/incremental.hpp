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
//     the same selection changes nothing), and the report lists it with the reason (ChangeReport::keptRooms). The room's recorded media family and theme pack are authoritative: its data is read
//     with them (loadRoomData()), so a data change never freezes a room;
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
//     goes to the next socket), a growth that would move a pinned fixture of the south wall is not taken (the titles
//     needing it are unplaced for capacity); both are reported in needsConfirmation, as are the pins the plan does not
//     honour (which the update never applies), and the orphaned pins in orphanedPins.
// An updated room's unplaced games are its selection's with their reasons (a duplicate's representative being the
// placed game that represents its title), every placed game keeps its placement and no entry besides; it records this
// generator's version. A selection for a system that has no room yet is refused (new systems enter the building with I4
// step 6a). A system's selection goes to the room that holds its section (sticky: whatever its count now); a shared or
// arcade room is updated with the selections of all its sections (ADR-0013 §6), and one given only some of them is held
// as it is, as a room without a selection, and listed in the report's heldRooms with the sections it lacks (the plan
// does not say which section an unplaced game was of). The plan records the highest of its rooms' generator versions,
// the selections' sort-key version and the inputDigest of the inputs read (generationInputDigest(), over the rooms
// given a selection; an update that gives some room no selection and changes nothing keeps the previous inputDigest,
// which named more), so that an update of an unchanged catalog gives the previous plan byte for byte, whichever of its
// rooms it is given. The change report lists the games added, the placements kept, the games newly unplaced, the
// fixtures added, removed and moved, the dressing removed, the slots freed, the pins, the rooms kept and held, the
// rebuild level (NoRebuild when no placement, fixture, light or dressing changed; a slot handed to an alias's identity
// is a texture swap) and the digests before (the stored file's SHA-256: previousDigest) and after; the validation is
// validatePlan()'s as for a fresh plan, the pins it judges being the honoured and orphaned ones, the rooms kept judged
// as kept.

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

/// What an incremental update of a building reads: UpdateInput's with a selection per system to update (each of its
/// system's room:<system>) and the systems' platform tags.
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
/// previous plan (loadRoomData(): its recorded family, theme pack and template), by its system. Errors:
/// InvalidArgument for a previous plan of another library, a selection of a system without a room in it, two
/// selections of one system, a room without data in `rooms` or with data of another family than its recorded one, a
/// schema-1 plan without its file's digest or that cannot be lifted, the inputs generateBuilding() refuses; Unsupported
/// for a previous plan of a newer schema or generator version (written by a newer Retroverse); those of the layout (a
/// side-wall module that does not fit its socket: ValidationFailed).
[[nodiscard]] Expected<GenerationResult> updateBuilding(const BuildingUpdateInput& input, const BuildingData& building,
                                                        const SystemsData& systems, std::span<const WorldData> rooms);

/// The update of the selection's one system (updateBuilding() with `data`'s building, systems and room data: the
/// previous plan holds that room only, or its other rooms lack data).
[[nodiscard]] Expected<GenerationResult> update(const UpdateInput& input, const WorldData& data);

} // namespace rv::world
