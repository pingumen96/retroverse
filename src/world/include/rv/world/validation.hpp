#pragma once

// The WorldPlan's validation (ADR-0010 §11, ADR-0013 §11, specification §7.3 step 13 and §21.3): validatePlan() judges
// a plan against the templates of its rooms, the world's configuration and, when given, the rooms' selections, the
// prefabs' slots, the rooms' lights, the pins, the building's data and the rooms an update kept. A hard finding makes
// the plan unusable (never saved or shown: the app keeps the previous plan, or the empty room shell and the quick
// catalog); a soft one is reported and the plan kept. The report lists the findings rule by rule in the order of
// validationRules(), each rule's findings sorted by subject then message, and the LayoutScore (style §13.5) as a
// diagnostic that chooses nothing in I3 (ADR-0010 §4).
//
// Geometry (millimetres in the room's frame, world_plan.hpp): a fixture is the axis-aligned box around its prefab's
// bounds turned by its yaw about its pivot, the prefab's origin as rv/geometry/prefabs.hpp places it by kind: the
// middle of the footprint at the bottom for most kinds (the box from yMm to yMm + heightMm), the middle of the back
// for a poster frame (hung on a wall: the box reaches depthMm out along its front and is centred on yMm), the centre
// for a cartridge; boxes that touch do not overlap. A turned fixture's box is rounded outwards to half millimetres
// from sines and cosines evaluated by a polynomial of the yaw (basic arithmetic only), so every build computes the
// same boxes. A floor covering (a
// fixture on the floor at most kFloorCoveringMaxHeightMm high: the rug, which has no collider) is walked over: it
// overlaps nothing, blocks no cell and hides nothing, but it may not lie in the doorway's clear area nor leave the
// room. Every other fixture is an obstacle of the navigation grid (nav_grid.hpp), wall-mounted ones included (they
// stand against a wall the walker cannot cross anyway).
//
// The building (ADR-0013 §11): the plan holds the ground floor, every room stands on a floor of the plan (and every
// floor is building.json's, at its order, when the building is given), the rooms of a floor do not overlap (their
// footprints on the floor: the interior grown by the building's wall allowance, building.hpp), every system stands in
// exactly one room (an expansion counted with its chain's head) or one section, and the arcade room stands on the
// ground floor. With the building given, each floor's hall is derived from its rooms (floor_layout.hpp's deriveHall(),
// I4 step 3) and judged with them: no room overlaps the hall (hall-overlap), every room's doorway is in its doorway
// wall, which stands on the hall's edge (doorway-off-hall), and every room can be walked to from the elevator
// (floor-unreachable): the floor's navigation grid (nav_grid.hpp, 0.1 m cells over the hall and the floor's rooms;
// obstacles the hall's walls, the elevator's frame, the hall's benches and plants (I6 step 2), the hub's solid pieces
// (I6 step 5: columns, the landmark, the totem, the showcase, sofas and plants; its walls above its rooms are not), the
// rooms' walls around their doorways and their fixtures, dilated by half the minimum clear width) searched from the
// arrival point in front of the elevator reaches every doorway's clear area's centre within kInteractionRadiusMm, and
// on the ground floor the middle of the totem's zone and the point kHubShowcaseFrontMm before the showcase's front
// (subjects "hall:ground/totem" and "hall:ground/showcase"). The hall's lights
// are not the plan's: only the rooms keep the light limit. A room an update kept as it is (ValidationContext::keptRooms) is reported (soft, room-kept); its
// template's version and its prefabs' slots are not judged, since it keeps what an older template or prefab made.
//
// Reachability: the grid of each room, its obstacles dilated by half the configuration's minimum clear width
// (WorldConfig::minClearWidthMm, 1.2 m), is searched from the cell at the centre of the doorway's clear area. A
// target (every hero-wall and side-wall module, every display fixture and the hero TV, the hero corner's "crt" part)
// is reachable when a reached cell's centre lies within kInteractionRadiusMm of its interaction point, the middle of
// its box's front face moved kInteractionDistanceMm out along its front (style §6.2: 1.1 to 1.5 m kept free in front of
// an interactive shelf). The same search with half the main passage width (1.4 m) gives the soft rule passage-narrow.
// The hero TV is in line of sight when the segment from the doorway's clear area's centre at kDoorwayEyeHeightMm to the
// middle of the CRT's front face crosses no other fixture's box (floor coverings excepted).

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <rv/catalog/ids.hpp>
#include <rv/core/expected.hpp>
#include <rv/world/building.hpp>
#include <rv/world/generation.hpp>
#include <rv/world/ids.hpp>
#include <rv/world/overrides.hpp>
#include <rv/world/room_selection.hpp>
#include <rv/world/room_template.hpp>
#include <rv/world/world_config.hpp>
#include <rv/world/world_plan.hpp>

namespace rv::world {

/// The rules, by the name their findings carry (ValidationFinding::rule), in the order they run.
namespace rule {
// Hard rules.
inline constexpr std::string_view kRoomUnknown = "room-unknown";
inline constexpr std::string_view kGroundFloorMissing = "ground-floor";
inline constexpr std::string_view kFloorUnknown = "floor-unknown";
inline constexpr std::string_view kRoomsOverlap = "rooms-overlap";
inline constexpr std::string_view kSystemRooms = "system-rooms";
inline constexpr std::string_view kHallOverlap = "hall-overlap";
inline constexpr std::string_view kDoorwayOffHall = "doorway-off-hall";
inline constexpr std::string_view kFloorUnreachable = "floor-unreachable";
inline constexpr std::string_view kRoomTemplate = "room-template";
inline constexpr std::string_view kRoomSize = "room-size";
inline constexpr std::string_view kFixtureSocket = "fixture-socket";
inline constexpr std::string_view kFixtureLevels = "fixture-levels";
inline constexpr std::string_view kPrefabSlots = "prefab-slots";
inline constexpr std::string_view kFixtureOutsideRoom = "fixture-outside-room";
inline constexpr std::string_view kDecorationOffWall = "decoration-off-wall";
inline constexpr std::string_view kDoorReserved = "door-reserved";
inline constexpr std::string_view kDoorwayClearance = "doorway-clearance";
inline constexpr std::string_view kFootprintsOverlap = "footprints-overlap";
inline constexpr std::string_view kLightsOverLimit = "lights-over-limit";
inline constexpr std::string_view kSlotUnknown = "slot-unknown";
inline constexpr std::string_view kSlotShared = "slot-shared";
inline constexpr std::string_view kDressingUnknown = "dressing-unknown";
inline constexpr std::string_view kDressingOverlap = "dressing-overlap";
inline constexpr std::string_view kSlotDressed = "slot-dressed";
inline constexpr std::string_view kGameTwice = "game-twice";
inline constexpr std::string_view kGameMissing = "game-missing";
inline constexpr std::string_view kRepresentativeUnknown = "representative-unknown";
inline constexpr std::string_view kPinBroken = "pin-broken";
inline constexpr std::string_view kHeroTvMissing = "hero-tv-missing";
inline constexpr std::string_view kDoorwayBlocked = "doorway-blocked";
inline constexpr std::string_view kUnreachable = "unreachable";
// Soft rules.
inline constexpr std::string_view kRoomKept = "room-kept";
inline constexpr std::string_view kPrefabUnresolved = "prefab-unresolved";
inline constexpr std::string_view kPinOrphaned = "pin-orphaned";
inline constexpr std::string_view kTvLineOfSight = "tv-line-of-sight";
inline constexpr std::string_view kPassageNarrow = "passage-narrow";
inline constexpr std::string_view kSocketBare = "socket-bare";
inline constexpr std::string_view kDecorationDensity = "decoration-density";
} // namespace rule

/// A rule of the validation.
struct ValidationRule {
    std::string_view name;
    Severity severity = Severity::Hard;
    /// What it checks, one sentence.
    std::string_view summary;
};

/// Every rule, in the order the validation runs them and reports their findings.
[[nodiscard]] std::span<const ValidationRule> validationRules() noexcept;
/// The rule named `name`, or null.
[[nodiscard]] const ValidationRule* findValidationRule(std::string_view name) noexcept;

/// How far in front of a target's front face its interaction point lies, and how near it a reached cell must be.
inline constexpr std::int64_t kInteractionDistanceMm = 800;
inline constexpr std::int64_t kInteractionRadiusMm = 400;
/// The eye of a walker standing in the doorway (CLAUDE.md: the eye 1.65 m above the feet), for the TV's line of sight.
inline constexpr std::int64_t kDoorwayEyeHeightMm = 1650;
/// A fixture on the floor at most this high is a floor covering, walked over (the rug: 12 mm).
inline constexpr std::int64_t kFloorCoveringMaxHeightMm = 20;
/// The hero corner's part that is the hero TV (room_template.hpp: exactly one per template).
inline constexpr std::string_view kHeroTvPart = "crt";

/// A fixture's box as the validation judges it (see the file comment: its prefab's bounds turned by its yaw about its
/// pivot), in its room's frame, rounded outwards to whole millimetres: what the editor picks and outlines (I4b step 4).
struct FixtureBox {
    std::int64_t minXMm = 0;
    std::int64_t minYMm = 0;
    std::int64_t minZMm = 0;
    std::int64_t maxXMm = 0;
    std::int64_t maxYMm = 0;
    std::int64_t maxZMm = 0;

    bool operator==(const FixtureBox&) const = default;
};
[[nodiscard]] FixtureBox fixtureBox(const Fixture& fixture) noexcept;

/// A level of a prefab's slots: its "Slot_<level>_<index>" sockets with that level, `slots` of them.
struct PrefabSlotLevel {
    std::uint32_t level = 0;
    std::uint32_t slots = 0;

    auto operator<=>(const PrefabSlotLevel&) const = default;
};

/// The slot levels of a prefab, by level (empty for a prefab without slots). Errors: the prefab cannot be resolved
/// (its manifest missing or broken, a kind it does not build): the rule prefab-unresolved, soft, reports them.
using PrefabSlotSource = std::function<Expected<std::vector<PrefabSlotLevel>>(const PrefabRef& prefab)>;

/// The prefabs of an asset root: <assetRoot>/manifests/prefabs/<manifest>.json read with assets::loadPrefabManifest,
/// its runtime.generator required to be the kind the plan names, the procedural prefab built with the manifest's
/// runtime.params (geometry::buildPrefab) and its ShelfSlot sockets counted per level. A model prefab (runtime.model)
/// is not built: its slots are not known (an error).
[[nodiscard]] PrefabSlotSource manifestPrefabSlots(std::filesystem::path assetRoot);

/// The point and spot lights a room holds: the caller counts the plan's (WorldPlan::lights, I3 step 3) by room, and a
/// room without a count is not checked.
struct RoomLights {
    RoomId room;
    std::uint32_t lights = 0;

    bool operator==(const RoomLights&) const = default;
};

// Pins (ADR-0010 §9): GamePin and FixturePin, overrides.json's (rv/world/overrides.hpp, I3 step 9).

/// What a plan is judged against.
struct ValidationContext {
    /// The templates of the plan's rooms (by RoomTemplate::id); a room whose template is not here fails room-template
    /// and its geometry is not checked.
    std::vector<RoomTemplate> templates;
    /// The passage widths (WorldConfig::minClearWidthMm, mainPassageWidthMm) and the decoration density.
    WorldConfig config;
    /// The rooms' selections: every game of a selection (its titles and its unplaced games) needs exactly one
    /// placement or unplaced entry in the plan (game-missing). A room without one is not checked for missing games.
    std::vector<RoomSelection> selections;
    /// The prefabs' slots, compared with the fixtures' levels (prefab-slots, prefab-unresolved); nothing: not compared.
    PrefabSlotSource prefabSlots;
    /// The rooms' light counts (lights-over-limit); a room without one is not checked.
    std::vector<RoomLights> lights;
    /// Pins honoured (pin-broken) or orphaned (pin-orphaned).
    std::vector<GamePin> gamePins;
    std::vector<FixturePin> fixturePins;
    /// The building's data: its floors (floor-unknown), the wall allowance of the rooms' footprints (rooms-overlap)
    /// and the halls' widths (hall-overlap, doorway-off-hall, floor-unreachable); nothing: the plan's floors are taken
    /// as they are, rooms' interiors compared and the halls not judged.
    std::optional<BuildingData> building;
    /// The rooms an update kept as they are (room-kept), with the reason.
    std::vector<KeptRoom> keptRooms;
};

/// Judges `plan` (as readPlan() returns it, or any plan in memory: the lists need not be sorted) against `context`.
/// Never fails: what it cannot judge is a finding.
[[nodiscard]] ValidationReport validatePlan(const WorldPlan& plan, const ValidationContext& context);

} // namespace rv::world
