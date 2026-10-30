#pragma once

// overrides.json (ADR-0010 §9, §10; I3 step 9; schema 2 since I4b step 1, ADR-0015 §2):
// <user data>/worlds/<library id>/overrides.json holds the world's pins, a game held in a slot and a fixture held in
// its socket (with its prefab, when the pin names one) or at the place the user gave it. Since I4b Retroverse writes it
// (writeOverrides(), saved under the world lock by world_store.hpp's saveOverrides() and world_snapshots.hpp's
// commitWorld()); the user may still edit it by hand. A missing file holds no pin.
//
// The file (JSON with comments, every key but schemaVersion optional):
//   {"schemaVersion": 2,
//    "gamePins": [{"game": "g0123456789abcdef", "slot": "room:snes/fx:w01/s:3.12",
//                  "provenance": {"origin": "editor", "time": "2026-10-27T10:00:00.000Z"}}],
//    "fixturePins": [{"fixture": "room:snes/fx:w05", "prefab": {"manifest": "decor.poster.generic",
//                                                                 "kind": "poster_frame"},
//                     "transform": {"xMm": 1200, "zMm": -300, "yawCdeg": 9000},
//                     "provenance": {"origin": "cli"}}]}
// A game is pinned once and a slot holds one pin; a fixture is pinned once. "prefab", "transform" (the fixture's place
// in the room's frame, its height the prefab's; absent for a fixture pinned in its socket) and "provenance" are
// optional. Provenance says who made the pin (`origin`: editor, cli, or file for a schema-1 pin or a hand edit, the
// default) and when (`time`, UTC, absent when unknown); it never decides anything and is left out of every digest.
// "conflict" (schema 2, since I4b step 5's review: ADR-0015 §3 as amended on 2026-10-28) is what the last regeneration
// of the pin's room could not honour about it ("room-gone", "slot-gone", "game-gone", "invalid"): written by the
// regeneration, cleared by the one that honours the pin (or by the editor's next pin of it), never in a digest. A pin
// with a conflict that the plan does not honour is reported as orphaned at every update, never as needing a
// confirmation (a regeneration, which the app's notice would offer, cannot honour it either).
// Schema 1 (I3: no provenance, no transform) is read lifted (origin file); schema 2 is the only one written; a file of
// a newer schemaVersion is refused (written by a newer Retroverse).
//
// Pins are protected and reported, never applied by the generator: the generator and the incremental update never move
// or remove a pinned object (a change that would is not applied and is reported as needing a confirmation), a pin that
// names a game, slot, fixture or socket that no longer exists is reported as orphaned, and a pin the plan does not
// honour is reported (ADR-0010 §10). Since I4b step 2 the editor's operations (world_edit.hpp) make pins as they change
// the plan, an update leaves a fixture pinned at a transform where it stands (never moved by growth, the sockets it
// overlaps skipped), and the pins a plan honours join its inputDigest (ADR-0015 §2). Since I4b step 5 the explicit
// regeneration applies them (regeneration.hpp's regenerate(): pinned fixtures first, pinned games reserved), and a pin
// it cannot honour is a conflict it keeps; a fresh generation without a stored plan still applies none.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/catalog/ids.hpp>
#include <rv/core/expected.hpp>
#include <rv/world/ids.hpp>
#include <rv/world/room_template.hpp>
#include <rv/world/world_plan.hpp>

namespace rv::world {

/// overrides.json's format, the one written (schema 1 is read lifted).
inline constexpr std::int64_t kOverridesSchemaVersion = 2;

/// Who made a pin (ADR-0015 §2).
enum class PinOrigin : std::uint8_t {
    File,   ///< a schema-1 pin, or a pin written by hand (the default)
    Editor, ///< the app's editor
    Cli,    ///< retroverse-cli
};

[[nodiscard]] constexpr std::string_view toString(PinOrigin origin) noexcept {
    switch (origin) {
    case PinOrigin::File:
        return "file";
    case PinOrigin::Editor:
        return "editor";
    case PinOrigin::Cli:
        return "cli";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::optional<PinOrigin> parsePinOrigin(std::string_view text) noexcept {
    return detail::enumFromString(text, PinOrigin::Cli);
}

/// Where a pin came from: informative, never deciding anything, never in a digest.
struct PinProvenance {
    PinOrigin origin = PinOrigin::File;
    /// When the pin was made, UTC as core::formatUtcIso8601() writes it; empty when unknown (a lifted pin).
    std::string time;

    bool operator==(const PinProvenance&) const = default;
};

/// A fixture's place given by the user, in its room's frame: its pivot on the floor and its yaw (the height stays the
/// prefab's).
struct FixtureTransform {
    std::int64_t xMm = 0;
    std::int64_t zMm = 0;
    /// 0 to 35999.
    std::int32_t yawCdeg = 0;

    bool operator==(const FixtureTransform&) const = default;
};

/// The bounds of a transform's coordinates: a room is a few metres wide, its frame never reaches 1 km.
inline constexpr std::int64_t kMaxFixtureOffsetMm = 1'000'000;

/// Why a regeneration cannot honour a pin (ADR-0015 §3): a conflict, recorded on the pin (regeneration.hpp).
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

/// A game held in a slot.
struct GamePin {
    catalog::GameId game;
    SlotId slot;
    PinProvenance provenance;
    /// The conflict the last regeneration of its room found (ADR-0015 §3 as amended on 2026-10-28): while the pin is
    /// not honoured, every update reports it as orphaned, not as needing a confirmation. Nothing: none (honoured, or
    /// never regenerated).
    std::optional<ConflictReason> conflict;

    bool operator==(const GamePin&) const = default;
};

/// A fixture held in its socket, or at the transform the user gave it, with its prefab when the pin names one.
struct FixturePin {
    FixtureId fixture;
    std::optional<PrefabRef> prefab;
    /// Absent: the fixture is held in its socket.
    std::optional<FixtureTransform> transform;
    PinProvenance provenance;
    /// As GamePin::conflict.
    std::optional<ConflictReason> conflict;

    bool operator==(const FixturePin&) const = default;
};

/// The world's overrides: its pins, games by GameId and fixtures by id.
struct WorldOverrides {
    std::vector<GamePin> gamePins;
    std::vector<FixturePin> fixturePins;

    [[nodiscard]] bool empty() const noexcept { return gamePins.empty() && fixturePins.empty(); }

    bool operator==(const WorldOverrides&) const = default;
};

/// Reads overrides.json's text, schema 1 lifted (every pin's origin `file`, no time, no transform), the pins sorted by
/// game and by fixture. Errors (context and line): ParseError, ValidationFailed (a key unknown, missing or of another
/// type, a malformed GameId, slot or fixture id, an origin or a time not as written, a transform out of range, a key of
/// schema 2 in a schema-1 file, a game, slot or fixture pinned twice), Unsupported (a newer schemaVersion).
[[nodiscard]] Expected<WorldOverrides> parseOverrides(std::string_view text, std::string_view context);
/// Reads the file; a missing file holds no pin. Errors: those of reading the file (other than NotFound) and of
/// parseOverrides().
[[nodiscard]] Expected<WorldOverrides> loadOverrides(const std::filesystem::path& file);

/// The canonical text of schema 2: formatted JSON (two-space indents, keys sorted, a final line feed), the pins sorted
/// by game and by fixture, a provenance's time and a pin's prefab and transform only when set. parseOverrides() reads
/// it back equal (the pins sorted), and the text of what it reads is this text again. Errors: InvalidArgument for
/// overrides that could not be read back (a game, slot or fixture pinned twice, an invalid id, a transform out of
/// range, a time that core::parseUtcIso8601() refuses).
[[nodiscard]] Expected<std::string> writeOverrides(const WorldOverrides& overrides);

/// The pins measured against a plan.
struct PinCheck {
    /// The pins that name a game, slot, fixture or socket the plan or its room's template no longer holds, and the
    /// pins the plan does not honour that a regeneration recorded as conflicts (their `conflict`): never moved,
    /// reported (ChangeReport::orphanedPins), each as pinText().
    std::vector<std::string> orphaned;
    /// The pins the plan does not honour although what they name is there: applying them would move or replace
    /// something, which I3 never does (ChangeReport::needsConfirmation), each as pinText().
    std::vector<std::string> unhonoured;
    /// The room of each unhonoured pin (its slot's or its fixture's), in `unhonoured`'s order.
    std::vector<RoomId> unhonouredRooms;
    /// The pins the plan honours and every pin whose subject is gone (PinMeasure::gone), with a recorded conflict or
    /// not: the validation's pin-orphaned and pin-broken rules see these. A recorded conflict whose subject is still
    /// there is reported in `orphaned` but not judged.
    WorldOverrides honouredOrOrphaned;
    /// The pins the plan honours (I4b step 2: their canonical form joins the plan's inputDigest, ADR-0015 §2).
    WorldOverrides honoured;
};

/// Measures `overrides` against `plan` (its rooms' templates in `templates`, by id): a game pin is honoured when its
/// game stands in its slot, orphaned when the slot is not one of the plan's (fixture, level and index) or the game has
/// no entry in the plan; a fixture pin is honoured when the plan holds the fixture (with the pin's prefab, when it
/// names one, at the pin's transform, when it has one), orphaned when its room is not the plan's or its socket is not
/// in the room's template; those are reported in `orphaned` and judged (`honouredOrOrphaned`), whether or not they
/// record a regeneration's conflict. Any other pin not honoured is reported in `orphaned`, not judged, when it records
/// a regeneration's conflict, else unhonoured (measurePins() says why of each, and shows a recorded conflict as
/// conflicting whether its subject is gone or not). Lists sorted.
[[nodiscard]] PinCheck checkPins(const WorldPlan& plan, const std::vector<RoomTemplate>& templates,
                                 const WorldOverrides& overrides);

/// What `worldplan check` says of a pin (ADR-0015 §8; I4b step 9): honoured, orphaned, conflicting with its recorded
/// reason, or not honoured although what it names is there.
enum class PinStatus : std::uint8_t {
    Honoured, ///< the plan holds it as pinned
    Orphaned, ///< it names a game, slot, room, fixture or socket that the plan or its room's template no longer holds
    Conflicting, ///< not honoured, with the conflict the last regeneration of its room recorded on it
    Unhonoured,  ///< not honoured although what it names is there, no conflict recorded (an update's confirmation)
};

/// "honoured", "orphaned", "conflicting", "unhonoured".
[[nodiscard]] constexpr std::string_view toString(PinStatus status) noexcept {
    switch (status) {
    case PinStatus::Honoured:
        return "honoured";
    case PinStatus::Orphaned:
        return "orphaned";
    case PinStatus::Conflicting:
        return "conflicting";
    case PinStatus::Unhonoured:
        return "unhonoured";
    }
    return "unknown";
}

/// One pin measured against a plan.
struct PinMeasure {
    /// A game pin (else a fixture pin).
    bool game = true;
    /// The pinned game's GameId or the pinned fixture's id.
    std::string subject;
    /// The room the pin names (its slot's or its fixture's).
    RoomId room;
    /// pinText() of the pin.
    std::string pin;
    PinStatus status = PinStatus::Honoured;
    /// The conflict recorded on the pin (GamePin::conflict, FixturePin::conflict), whatever its status: a honoured pin
    /// may still carry one until its room's next regeneration clears it.
    std::optional<ConflictReason> recordedConflict;
    /// Why it is not honoured, in words ("its slot is not one of the plan's", "the game stands in <slot>", ...);
    /// empty when honoured.
    std::string why;
    /// What it names is gone (its slot, game, room, room's template or socket), whatever its status: a conflicting
    /// pin may be gone too, and checkPins() then judges it as an orphaned one.
    bool gone = false;
    PinOrigin origin = PinOrigin::File;

    bool operator==(const PinMeasure&) const = default;
};

/// Every pin of `overrides` measured against `plan` (its rooms' templates in `templates`, by id), game pins first, each
/// list in the overrides' order (sorted by game and by fixture when read). A pin is honoured, else conflicting when it
/// records a conflict, else orphaned when what it names is gone, else unhonoured; the orphaned and conflicting pins
/// are checkPins()'s `orphaned`, the unhonoured its `unhonoured`; the orphaned ones and the conflicting ones whose
/// subject is gone are those it judges.
[[nodiscard]] std::vector<PinMeasure> measurePins(const WorldPlan& plan, const std::vector<RoomTemplate>& templates,
                                                  const WorldOverrides& overrides);

/// "game g0123456789abcdef pinned to room:snes/fx:w01/s:3.12", "fixture room:snes/fx:w05 pinned" (with " as
/// <manifest>" when the pin names a prefab, " at x <x> z <z> mm, yaw <yaw> cdeg" when it has a transform).
[[nodiscard]] std::string pinText(const GamePin& pin);
[[nodiscard]] std::string pinText(const FixturePin& pin);

} // namespace rv::world
