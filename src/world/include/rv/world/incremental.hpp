#pragma once

// The incremental update (ADR-0010 §10, specification §7.6, style §13.6, NFR-08; I3 step 9): a previous plan brought
// to the room's selection now, moving nothing that stands. world::update is a pure function of its inputs, like
// world::generate, and returns a GenerationResult (the plan, its change report, its validation).
//
// When the previous plan is of another seed, preset, template (id or version), generator major version, random
// algorithm, title-key rule, media family or theme pack, or its collection fixtures' levels are no longer their
// prefabs', the update changes nothing: RequiresExplicitRegeneration, the previous plan returned as it is with the
// reason (`worldplan generate --replace` regenerates after a backup). Otherwise:
//   - every placement is kept as it is, also a game's that is now unavailable, no longer eligible (its cover gone: it
//     keeps its slot with a generated label), hidden, or gone from the catalog's snapshot: its slot is never given to
//     another game (free slots are never compacted);
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
//     sockets are decorated as a fresh room's are); at the template's limit the title is unplaced for capacity;
//   - a new placement's offset comes from its seed as in a fresh plan; added modules, decoration and dressing are what
//     a fresh layout of the room would place there;
//   - pins (overrides.hpp) are protected: a side-wall socket whose fixture is pinned never takes a module (the module
//     goes to the next socket), a growth that would move a pinned fixture of the south wall is not taken (the titles
//     needing it are unplaced for capacity); both are reported in needsConfirmation, as are the pins the plan does not
//     honour (which the update never applies), and the orphaned pins in orphanedPins.
// The selection's unplaced games are listed with their reasons (a duplicate's representative being the placed game
// that represents its title), every placed game keeps its placement and no entry besides. The plan records the
// generator's version, the selection's sort-key version and the inputDigest of the inputs read
// (generationInputDigest()), so that an update of an unchanged catalog gives the previous plan byte for byte. The
// change report lists the games added, the placements kept, the games newly unplaced, the fixtures added, removed and
// moved, the dressing removed, the slots freed, the pins, the rebuild level (NoRebuild when no placement, fixture or
// dressing changed; a slot handed to an alias's identity is a texture swap) and the digests before and after; the
// validation is validatePlan()'s as for a fresh plan, the pins it judges being the honoured and orphaned ones.

#include <string>
#include <vector>

#include <rv/catalog/ids.hpp>
#include <rv/core/expected.hpp>
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

/// What an incremental update reads.
struct UpdateInput {
    catalog::LibraryId library;
    /// The room's selection now (world::selectRoom of the catalog's snapshot); while the media are unknown there is
    /// none, and no update runs (ADR-0010 §3: unknown media change nothing).
    RoomSelection selection;
    WorldConfig config;
    WorldOverrides overrides;
    /// The plan to update, as readPlan() returned it, and the SHA-256 of the file it was read from (its digest; empty:
    /// computed with planDigest()).
    WorldPlan previous;
    std::string previousDigest;
    /// The placed GameIds that became aliases (the caller asks the catalog for the placed games its snapshot lacks).
    std::vector<GameAlias> aliases;
};

/// The previous plan updated by ADR-0010 §10's rules (the file's header). Errors: InvalidArgument for a selection of
/// another room than the data's, a previous plan of another library or room or not holding exactly one room, and the
/// inputs generate() refuses; Unsupported for a previous plan of a newer schema or generator version (written by a
/// newer Retroverse); those of the layout (a side-wall module that does not fit its socket: ValidationFailed).
[[nodiscard]] Expected<GenerationResult> update(const UpdateInput& input, const WorldData& data);

} // namespace rv::world
