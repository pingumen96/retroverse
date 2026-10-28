#pragma once

// The explicit regeneration (ADR-0015 §3, §5; I4b step 5): a room chain, a floor or the whole building generated afresh
// as a fresh building generates it, honouring the world's overrides, with the impact summary that the preview shows
// ("Rigenera stanza / piano / mondo", `retroverse-cli worldplan regenerate`). A pure function of its inputs (the
// room data it reads through the caller's loader, the asset root's files): two runs give one plan, byte for byte, in
// Debug and Release alike.
//
// The scopes:
//   - a room (its chain: the room and its expansions, ADR-0015 §7; I4b step 7 makes expansions, none stands before
//     it): generated afresh with the data a new room of its kind reads now (the template's current version, the
//     family's last successor, the theme pack its floor resolves), the face-out set and the representatives chosen
//     again by priority; its id, kind, floor, x, yaw, sections and seed kept, its z following its depth. Every other
//     room stands as it is;
//   - a floor: every room of the floor, and the floor's rooms decided again: its systems (the sections of its rooms,
//     and the systems of the selections no room holds whose classification names the floor) enter it afresh in the
//     order (systems.json order, system name): own or shared rooms by sharedRoomMinTitles on today's counts, the
//     arcade room on the ground floor, the rooms placed again from the rows' start (ADR-0013 §7). A system standing on
//     the floor stays on it whatever its classification says now (only the world moves it), and so does every room of
//     the other floors;
//   - the world: the whole building generated afresh (generateBuilding()'s rules), the systems on the floors their
//     classification names. `worldplan generate --replace` is this scope.
// The overrides of the scope (the game pins whose slot, the fixture pins whose fixture, is in a room of the scope, as
// it stood or as it stands after) are honoured (ADR-0015 §3), unless `dropOverrides` drops them:
//   1. pinned fixtures first: a side-wall module, a display fixture or a decoration of a socket of the room's template
//      stands at its pinned transform (else in its socket) with its pinned prefab (else a decoration's prefab as it
//      stood, a module's or display's the template's), before the layout fills the other sockets: a socket whose new
//      fixture would overlap a pinned fixture moved there takes none (a module goes to the next socket); a pinned
//      decoration's socket is decorated whatever the density draws, a pinned module's always takes it; a hung
//      decoration whose back stood on the south wall follows that wall to the room's new depth, its pin's transform
//      rewritten (the result's overrides), as an update's growth moves it (ADR-0015 §3 as amended);
//   2. the side-wall sockets holding a pinned game's slot always take their module, whatever the count needs (the room
//      grows to a socket of a growth step when one holds a pinned slot, module or display fixture, and a pinned
//      decoration is judged against that growth, whatever order the pins' sockets sort in; the room grows further,
//      within its template's limit, while the side-wall sockets the pinned fixtures take or block leave the modules
//      the count needs no socket);
//   3. pinned games are reserved in their slots before the allocation (allocation.hpp); the face-out choice and the
//      alphabetical fill skip those slots and those games, every other title by the rules of a fresh room;
//   4. a pinned game stays its title's representative although another version ranks first: it takes the title's
//      place in the selection, the previous representative unplaced as its duplicate (the update's rule then keeps it,
//      incremental.hpp).
// A pin the regeneration cannot honour is a conflict, with its reason: `room-gone` (its room is not in the new plan: a
// system moved to a shared room), `slot-gone` (the slot's fixture, level or index, or the fixture's socket, is not in
// the new layout), `game-gone` (the game is not a title of the room's selection: not eligible, gone, another pinned
// version represents its title) or `invalid` (a pinned fixture breaks a hard rule of the new layout: the room is then
// generated without it, or its prefab is not one the room measures or does not fit its socket). A conflict is never
// dropped silently: the overrides keep it (an update reports it as orphaned or not honoured, the editor shows it) unless
// `dropOverrides` drops the scope's overrides, every one of them (ADR-0015 §5's consent). Pins outside the scope are
// left as they are.
//
// The regenerated plan's inputDigest is what an update of the same selections computes over it (the rooms given a
// selection, each with the data of the room it stands in now, the honoured pins joined: ADR-0015 §2), so that an update
// of an unchanged catalog after a regeneration writes nothing; a regeneration of a world without overrides of an
// unchanged catalog equals the fresh building byte for byte. The plan is validated as a fresh one is (the rooms
// outside the scope judged as the update judges them: kept rooms as kept, their selections not judged); a plan that
// breaks a hard rule is returned all the same, and its caller never saves it (ADR-0010 §11).
//
// The impact (ADR-0015 §5's preview) is the difference of the two plans: the rooms regenerated, added, removed and
// moved; the games that change slot; the titles entering and leaving face-out; the games newly placed and newly
// unplaced; the fixtures added, removed and moved; the pins kept (honoured), the conflicts and the overrides dropped.
// The caller saves the result behind a snapshot (world_snapshots.hpp's commitWorld(), action `regenerate`, so that
// `worldplan undo` restores it); a dry run (the preview) saves nothing.
//
// The signature of regenerate() is frozen at the end of I4b step 5 (docs/i4b/plan.md): step 6 calls it from the app on
// the pool, step 7 adds expansion chains beneath it.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rv/catalog/ids.hpp>
#include <rv/core/expected.hpp>
#include <rv/world/building.hpp>
#include <rv/world/generation.hpp>
#include <rv/world/ids.hpp>
#include <rv/world/overrides.hpp>
#include <rv/world/room_selection.hpp>
#include <rv/world/world_config.hpp>
#include <rv/world/world_plan.hpp>

namespace rv::world {

/// What a regeneration covers (ADR-0015 §5).
enum class RegenerationScopeKind : std::uint8_t {
    Room,  ///< "Rigenera stanza": a room chain
    Floor, ///< "Rigenera piano": a floor's rooms, decided again
    World, ///< "Rigenera mondo": the whole building
};

/// "room", "floor", "world".
[[nodiscard]] constexpr std::string_view toString(RegenerationScopeKind kind) noexcept {
    switch (kind) {
    case RegenerationScopeKind::Room:
        return "room";
    case RegenerationScopeKind::Floor:
        return "floor";
    case RegenerationScopeKind::World:
        return "world";
    }
    return "unknown";
}

struct RegenerationScope {
    RegenerationScopeKind kind = RegenerationScopeKind::World;
    /// Room: a room of the plan (an expansion names its head's chain).
    RoomId room;
    /// Floor: a floor of the building (building.json's id).
    std::string floor;

    [[nodiscard]] static RegenerationScope ofRoom(const RoomId& room);
    [[nodiscard]] static RegenerationScope ofFloor(std::string_view floor);
    [[nodiscard]] static RegenerationScope ofWorld();
    /// What a snapshot's scope records: the room's id, "floor:<id>", "world".
    [[nodiscard]] std::string toString() const;

    bool operator==(const RegenerationScope&) const = default;
};

/// Why a pin cannot be honoured (ADR-0015 §3).
enum class ConflictReason : std::uint8_t {
    RoomGone, ///< its room is not in the new plan
    SlotGone, ///< its slot, or its fixture's socket, is not in the new layout
    GameGone, ///< its game is not a title of the room's selection
    Invalid,  ///< the pinned fixture breaks a hard rule of the new layout
};

/// "room-gone", "slot-gone", "game-gone", "invalid".
[[nodiscard]] constexpr std::string_view toString(ConflictReason reason) noexcept {
    switch (reason) {
    case ConflictReason::RoomGone:
        return "room-gone";
    case ConflictReason::SlotGone:
        return "slot-gone";
    case ConflictReason::GameGone:
        return "game-gone";
    case ConflictReason::Invalid:
        return "invalid";
    }
    return "unknown";
}
[[nodiscard]] constexpr std::optional<ConflictReason> parseConflictReason(std::string_view text) noexcept {
    return detail::enumFromString(text, ConflictReason::Invalid);
}

/// A pin the regeneration does not honour: the pin (pinText()), the game or the fixture it names, the reason and a
/// sentence.
struct OverrideConflict {
    std::string pin;
    catalog::GameId game;
    FixtureId fixture;
    ConflictReason reason = ConflictReason::Invalid;
    std::string message;

    bool operator==(const OverrideConflict&) const = default;
};

/// A game that changes slot.
struct GameMove {
    catalog::GameId game;
    SlotId from;
    SlotId to;

    auto operator<=>(const GameMove&) const = default;
};

/// What a regeneration changes (ADR-0015 §5's preview), each list sorted.
struct RegenerationImpact {
    /// The scope's rooms in the new plan.
    std::vector<RoomId> roomsRegenerated;
    /// The rooms only the new plan holds, only the previous one, and those whose floor or place on it changed.
    std::vector<RoomId> roomsAdded;
    std::vector<RoomId> roomsRemoved;
    std::vector<RoomId> roomsMoved;
    /// The games placed in both plans in other slots.
    std::vector<GameMove> gamesMoved;
    /// The games standing face-out (a featured or band slot) in the new plan and not in the previous one, and those
    /// that did and do not.
    std::vector<catalog::GameId> faceOutEntering;
    std::vector<catalog::GameId> faceOutLeaving;
    /// The games placed in the new plan and not in the previous one, and those that were and are not.
    std::vector<catalog::GameId> newlyPlaced;
    std::vector<catalog::GameId> newlyUnplaced;
    /// The fixtures by id: only in the new plan (or of another role or prefab there), only in the previous one (or of
    /// another role or prefab), and standing elsewhere (another place, yaw, size or levels).
    std::vector<FixtureId> fixturesAdded;
    std::vector<FixtureId> fixturesRemoved;
    std::vector<FixtureId> fixturesMoved;
    /// The scope's pins the new plan honours, those it cannot (by pin), and those dropped (dropOverrides), as
    /// pinText().
    std::vector<std::string> pinsKept;
    std::vector<OverrideConflict> conflicts;
    std::vector<std::string> overridesDropped;

    bool operator==(const RegenerationImpact&) const = default;
};

/// The plans' difference: every list of RegenerationImpact but roomsRegenerated, pinsKept, conflicts and
/// overridesDropped (which only the regeneration knows).
[[nodiscard]] RegenerationImpact planDifference(const WorldPlan& before, const WorldPlan& after);

/// Reads the data of a room (generation.hpp's loadWorldData(), whose request it takes): the caller's, so that the app
/// and the CLI read the asset root as they do (cached), and tests give theirs.
using RoomDataLoader = std::function<Expected<WorldData>(const RoomDataRequest& request)>;

/// A loader reading `assetRoot` with loadWorldData(), each request read once.
[[nodiscard]] RoomDataLoader assetRootLoader(const std::filesystem::path& assetRoot);

/// What a regeneration reads.
struct RegenerationInput {
    RegenerationScope scope;
    /// Drop the scope's overrides (ADR-0015 §5's "Rigenera e scarta le personalizzazioni", the CLI's
    /// --drop-overrides): nothing is honoured, and the result's overrides lack them.
    bool dropOverrides = false;
    catalog::LibraryId library;
    /// The rooms' selections now (selectRoom()), one per system, each of its system's room:<system>: the scope's
    /// systems must have one (a room's every section's; a floor's every standing system's), and those of the
    /// building's other rooms join the inputDigest as an update's (an update reads them all).
    std::vector<RoomSelection> selections;
    std::map<catalog::SystemName, std::vector<std::string>> platformTags;
    WorldConfig config;
    WorldOverrides overrides;
    /// The plan to regenerate, of schema 2 (readPlan()'s, lifted), and the SHA-256 of its file (empty: computed).
    WorldPlan previous;
    std::string previousDigest;
};

/// A regeneration's answer.
struct Regeneration {
    /// The plan (sorted), its change report against the previous plan (Generated, Full; the games placed anew, the
    /// placements kept, the fixtures' difference, the rooms added; orphaned and unhonoured pins; the digests) and its
    /// validation.
    GenerationResult result;
    /// The overrides to save with the plan: the input's, the scope's dropped with dropOverrides, the transforms of the
    /// pins that followed the south wall rewritten.
    WorldOverrides overrides;
    RegenerationImpact impact;
    /// The plan or the overrides differ from the input's (false: a commit writes nothing).
    bool changed = false;
};

/// Regenerates `input.scope` of `input.previous` by the rules above; `building` and `systems` are the asset root's
/// (loadBuildingData(), loadSystemsData()), `load` reads the rooms' data: a new room's (RoomDataRequest without a
/// family), a standing room's as an update reads it (loadRoomData()'s request: its kind, floor, recorded family and
/// template). Errors (nothing decided): InvalidArgument for a previous plan of another library or not of schema 2, a
/// scope's room the plan lacks or a floor the building lacks, the inputs generateBuilding() refuses (a selection twice,
/// not of a system's room); NotFound for a system of a room or floor scope without a selection; Unsupported for a room
/// or floor scope of a plan of another seed, preset, random algorithm, title-key rule or generator major (only the world
/// regenerates it), and for a room with expansions (I4b step 7); those of `load` and of the layout.
[[nodiscard]] Expected<Regeneration> regenerate(const RegenerationInput& input, const BuildingData& building,
                                                const SystemsData& systems, const RoomDataLoader& load);

namespace detail {

/// The pinned fixtures among `candidates` that the hard findings of `report` break (the regeneration then generates
/// the scope again without them, each an `invalid` conflict), by fixture, each with why: those a hard finding names (as
/// its subject or in its message); when the findings name none, the pins of `applied` (those the plan stands as
/// pinned) in a room that a hard finding's subject is or is in (a fixture, a slot), each with that room's first such
/// finding. Empty: the hard findings are not the pins' (the plan is refused as any). Public for its tests.
[[nodiscard]] std::map<FixtureId, std::string> breakingPins(const ValidationReport& report,
                                                            std::span<const FixturePin> candidates,
                                                            const std::set<FixtureId>& applied);

} // namespace detail

} // namespace rv::world
