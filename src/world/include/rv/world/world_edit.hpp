#pragma once

// Edits of a plan and its overrides (ADR-0015 §1, §4, §8; I4b step 2): the operations the app's editor (I4b steps 3 and
// 4) and `retroverse-cli worldplan edit` apply, each a pure change of a plan and its overrides together, validated at
// once by the hard rules. The plan is the truth: an edit changes it (a placement's slot, a fixture's place); the
// overrides record what the user decided, so that a regeneration honours it (I4b step 5). Every manual change is pinned
// (style §13.4): a game moved or swapped is pinned in its new slot, a fixture moved is pinned where it was put (a fixture
// put back home stands where the layout stands it: its pin is removed).
//
// The operations:
//   - move: a placed game into an empty slot of its room chain (its room and its expansions, ADR-0015 §7), a face-out
//     one included; a dressed span gives way where the game stands (the span split around its slot); the slot it left
//     stays free, dressed with one slot of its level's prop when it is face-out (as an update frees one: ADR-0010 §10).
//     A move onto its own slot pins it there. Its offset is drawn again as a fresh plan draws it in that slot.
//   - swap: two placed games of one room chain exchange their slots, both pinned.
//   - pin, unpin: a placed game pinned in its slot; a game's pin removed (also an orphaned one), the game left where it
//     stands. Nothing to do (pinned there already, no pin) is no change.
//   - move-fixture: a side-wall module, a display fixture or a decoration moved and turned in its room: its pivot on
//     the room's 100 mm grid through its place in its socket (its "home", where a fresh layout stands it), so
//     that a fixture slid along its wall stays flush with it; turned a quarter turn at a time
//     (decoration by 15 degrees); a decoration hung on a wall (a poster) only along the walls, its back on a wall
//     (within kMaxWallGapMm) facing into the room, the coordinate across its wall exempt from the grid. Its games and
//     its dressing move with it (they name it); a lamp's light follows it; a moved side-wall module has the room's
//     wall-washers placed again over the modules where they stand. Pinned at its transform; put back home, its pin is
//     removed (a pinned prefab stays pinned, in its socket), so that a fixture of the south wall put back moves with
//     its wall again (ADR-0015 §3 refuses a growth that would move one pinned in its socket). A move onto where it
//     stands pins it there, or changes nothing at home. The hero corner and the hero-wall modules are fixed.
//   - pin-fixture, unpin-fixture: a movable fixture pinned where it stands (in its socket when at home, else at its
//     transform), or its pin removed (the fixture left where it stands).
// An edit is refused, the world unchanged, when it names what the plan lacks, breaks a rule of its own (edit_rule) or
// leaves a hard finding of the validation (validation.hpp) on the floor it edits: an overlap, a doorway's clearance or
// a door-reserved socket entered, a fixture outside its room, a module, display fixture or the hero TV that cannot be
// walked to through the 1.2 m clear width, a floor whose rooms cannot be walked to from the elevator.
//
// The plan's inputDigest joins the canonical form of the pins it honours (ADR-0015 §2): an edit replaces the honoured
// pins' term of the digest (generation_parts.hpp: the SHA-256 of their canonical form XORed into the inputs' digest),
// so that an update of an unchanged catalog after an edit computes the same digest and writes nothing, and a plan of a
// world without overrides keeps its digest byte for byte.
//
// The signature of applyEdit() is frozen at the end of I4b step 2 (docs/i4b/plan.md): the editor's draft (step 3)
// replays its operations onto the stored plan with applyEdits() when it saves, and the regeneration (step 5) reads
// the overrides edits write.

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
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
#include <rv/world/validation.hpp>
#include <rv/world/world_config.hpp>
#include <rv/world/world_plan.hpp>

namespace rv::world {

/// What an edit does (ADR-0015 §8's command line names).
enum class EditKind : std::uint8_t {
    Move,         ///< a game into an empty slot of its room chain
    Swap,         ///< two games exchange their slots
    Pin,          ///< a game pinned in its slot
    Unpin,        ///< a game's pin removed
    MoveFixture,  ///< a fixture moved and turned in its room
    PinFixture,   ///< a fixture pinned where it stands
    UnpinFixture, ///< a fixture's pin removed
};

/// "move", "swap", "pin", "unpin", "move-fixture", "pin-fixture", "unpin-fixture".
[[nodiscard]] constexpr std::string_view toString(EditKind kind) noexcept {
    switch (kind) {
    case EditKind::Move:
        return "move";
    case EditKind::Swap:
        return "swap";
    case EditKind::Pin:
        return "pin";
    case EditKind::Unpin:
        return "unpin";
    case EditKind::MoveFixture:
        return "move-fixture";
    case EditKind::PinFixture:
        return "pin-fixture";
    case EditKind::UnpinFixture:
        return "unpin-fixture";
    }
    return "unknown";
}
[[nodiscard]] constexpr std::optional<EditKind> parseEditKind(std::string_view text) noexcept {
    return detail::enumFromString(text, EditKind::UnpinFixture);
}

/// One edit. The fields an operation does not use stay empty.
struct EditOperation {
    EditKind kind = EditKind::Pin;
    /// Move, Swap, Pin, Unpin: the game.
    catalog::GameId game;
    /// Move: the slot it goes to.
    SlotId slot;
    /// Swap: the other game.
    catalog::GameId with;
    /// MoveFixture, PinFixture, UnpinFixture: the fixture.
    FixtureId fixture;
    /// MoveFixture: where its pivot goes in its room's frame, and its yaw (nothing: it keeps its yaw).
    std::int64_t xMm = 0;
    std::int64_t zMm = 0;
    std::optional<std::int32_t> yawCdeg;

    [[nodiscard]] static EditOperation move(const catalog::GameId& game, const SlotId& slot);
    [[nodiscard]] static EditOperation swap(const catalog::GameId& game, const catalog::GameId& with);
    [[nodiscard]] static EditOperation pin(const catalog::GameId& game);
    [[nodiscard]] static EditOperation unpin(const catalog::GameId& game);
    [[nodiscard]] static EditOperation moveFixture(const FixtureId& fixture, std::int64_t xMm, std::int64_t zMm,
                                                   std::optional<std::int32_t> yawCdeg = std::nullopt);
    [[nodiscard]] static EditOperation pinFixture(const FixtureId& fixture);
    [[nodiscard]] static EditOperation unpinFixture(const FixtureId& fixture);

    bool operator==(const EditOperation&) const = default;
};

/// "move g0123456789abcdef to room:snes/fx:dc01/s:0.1", "move-fixture room:snes/fx:w03 to x 0 z 1200 mm, yaw 9000
/// cdeg", ...
[[nodiscard]] std::string editText(const EditOperation& operation);

/// The rules of the edits themselves, by the name a refusal carries (EditRefusal::rule); a refusal by the validation
/// carries the validation's rule (validation.hpp's rule::).
namespace edit_rule {
inline constexpr std::string_view kGameUnknown = "game-unknown";
inline constexpr std::string_view kSlotTaken = "slot-taken";
inline constexpr std::string_view kSlotPinned = "slot-pinned";
inline constexpr std::string_view kOtherRoom = "other-room";
inline constexpr std::string_view kSameGame = "same-game";
inline constexpr std::string_view kFixtureUnknown = "fixture-unknown";
inline constexpr std::string_view kFixtureFixed = "fixture-fixed";
inline constexpr std::string_view kRoomKept = "room-kept";
inline constexpr std::string_view kOffGrid = "off-grid";
inline constexpr std::string_view kTurnStep = "turn-step";
inline constexpr std::string_view kWallMount = "wall-mount";
} // namespace edit_rule

/// The edits' own rules (all hard), with what each checks, in a stable order; a target slot that is not the plan's is
/// refused with the validation's rule::kSlotUnknown.
[[nodiscard]] std::span<const ValidationRule> editRules() noexcept;

/// The grid a fixture moves on and the steps it turns by (ADR-0015 §4).
inline constexpr std::int64_t kEditGridMm = 100;
inline constexpr std::int32_t kFixtureTurnCdeg = 9000;
inline constexpr std::int32_t kDecorationTurnCdeg = 1500;

/// A plan and its overrides, edited together.
struct EditedWorld {
    WorldPlan plan;
    WorldOverrides overrides;

    bool operator==(const EditedWorld&) const = default;
};

/// What an edit reads besides the world: the configuration (the clear widths), the building (the floors' halls) and the
/// data of the plan's rooms (loadRoomData(): the template, the prefabs as measured), by room. An edit needs the data of
/// every room of each floor it edits (editRooms() names them).
struct EditData {
    WorldConfig config;
    BuildingData building;
    std::map<RoomId, std::shared_ptr<const WorldData>> rooms;
};

/// Why an edit was refused: the rule (an edit_rule:: or a validation rule), what it is about and a sentence.
struct EditRefusal {
    std::string rule;
    std::string subject;
    std::string message;

    bool operator==(const EditRefusal&) const = default;
};

/// What one edit did.
struct EditResult {
    /// Set when the edit was refused: the world is as it was.
    std::optional<EditRefusal> refused;
    /// The plan or the overrides changed (false for a pin that was there already, an unpin of nothing, a refusal).
    bool changed = false;
    /// What the edited rooms rebuild (the app's live update, ADR-0015 §4): NoRebuild when only placements moved onto
    /// bare slots (a texture swap) or only pins changed, LocalReflow when dressing gave way or a fixture moved (the
    /// room's static part).
    RebuildLevel rebuild = RebuildLevel::NoRebuild;
    /// The rooms whose elements changed, and the fixtures moved, by id.
    std::vector<RoomId> rooms;
    std::vector<FixtureId> fixturesMoved;
    /// The floor whose validation judged the edit (empty when the plan did not change) and that validation.
    std::string floor;
    ValidationReport validation;
    /// What it did, one line ("moved g... from <slot> to <slot>, pinned there").
    std::string summary;
};

/// Applies `operation` to `world` (its plan of schema 2, sorted or not; the result sorted when it changed), the pins it
/// makes with `provenance`: on success the edited plan and overrides, on a refusal the world untouched (the result's
/// `refused`). Errors (nothing changed): InvalidArgument for data that lacks a room of the floor the edit touches, a
/// plan without its digest's form; those of the digest.
[[nodiscard]] Expected<EditResult> applyEdit(EditedWorld& world, const EditOperation& operation, const EditData& data,
                                             const PinProvenance& provenance);

/// A session of edits applied in order, all or nothing (a CLI script, the editor's draft replayed on save).
struct EditSession {
    /// One result per operation applied, the refused one last.
    std::vector<EditResult> results;
    /// The index of the refused operation (the world then as it was before the session).
    std::optional<std::size_t> refusedAt;
};

/// applyEdit() of each operation in turn; on a refusal the world is restored as it was before the first. Errors: those
/// of applyEdit() (the world as it was).
[[nodiscard]] Expected<EditSession> applyEdits(EditedWorld& world, std::span<const EditOperation> operations,
                                               const EditData& data, const PinProvenance& provenance);

/// The rooms whose data an edit session needs (EditData::rooms): every room of each floor whose rooms the operations
/// name (a game's room where it stands, a slot's, a fixture's), by id. A name the plan does not hold adds nothing.
[[nodiscard]] std::vector<RoomId> editRooms(const WorldPlan& plan, std::span<const EditOperation> operations);

/// What commitWorld() saves for a session (world_snapshots.hpp): `edited`'s plan sorted, a change report (Updated; the
/// session's highest rebuild level; the placements kept as they were in `stored`; the fixtures moved; the digests,
/// before `storedDigest`, after the plan's) and the validation of the floors the session edited (each one's last).
/// Errors: those of the plan's digest.
[[nodiscard]] Expected<GenerationResult> editedResult(const WorldPlan& stored, std::string_view storedDigest,
                                                      const EditedWorld& edited, std::span<const EditResult> results);

/// The edit script's format (`worldplan edit --script`; formatted JSON with comments):
///   {"schemaVersion": 1, "operations": [
///     {"op": "move", "game": "g...", "slot": "room:snes/fx:dc01/s:0.1"},
///     {"op": "swap", "game": "g...", "with": "g..."},
///     {"op": "pin" | "unpin", "game": "g..."},
///     {"op": "move-fixture", "fixture": "room:snes/fx:w03", "xMm": 0, "zMm": 1200, "yawCdeg": 9000 (optional)},
///     {"op": "pin-fixture" | "unpin-fixture", "fixture": "room:snes/fx:w03"}]}
inline constexpr std::int64_t kEditScriptSchemaVersion = 1;

/// Reads an edit script. Errors (context and line): ParseError, ValidationFailed (an unknown op or key, a key the op
/// does not take or lacks, a malformed id, a coordinate beyond kMaxFixtureOffsetMm, a yaw outside 0 to 35999),
/// Unsupported (a newer schemaVersion).
[[nodiscard]] Expected<std::vector<EditOperation>> parseEditScript(std::string_view text, std::string_view context);
/// The script's text of `operations`: formatted JSON, keys sorted, a final line feed; parseEditScript() reads it back
/// equal.
[[nodiscard]] std::string writeEditScript(std::span<const EditOperation> operations);

} // namespace rv::world
