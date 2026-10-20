#pragma once

// The WorldPlan (ADR-0010 §8): what the generator decided for a library's world, as plain data that the app
// materializes (the scene, the collection's instances) and that is kept as a canonical JSON file (plan_json.hpp). It
// holds integers and identifiers only: lengths in millimetres, angles in hundredths of a degree, counts, seeds,
// GameIds, the world's ids and versions; no floating-point number, no title and no path (the catalog's text is read
// when the room is drawn), so the file carries nothing from the user's library but GameIds.
//
// Schema 2 (ADR-0013 §5) holds a building: its floors (each its own scene, in building.json's order) and, per room, its
// kind (a system's console room, a floor's shared room, the arcade room), its floor, its sections (the systems it
// holds, in order), its placement on its floor and the generator version that last generated or updated it. The halls
// are not stored: they are derived from a floor's rooms when its scene is built (I4 step 3). Schema 1 (I3: one room,
// no floors) is read and lifted (lift.hpp), never written.
//
// A room's contents are in the room's frame (ADR-0010 §4, ADR-0002): millimetres, +X east, +Y up, north = -Z, the
// origin on the floor at the centre of the template's base room. A room that grew keeps that origin (its north wall
// stays where it was) and extends south, so its bounds are not centred on it. Yaws are hundredths of a degree from 0 to
// 35999 about +Y, right-handed (glm::rotate's sense: counter-clockwise seen from above), applied to a prefab whose
// front faces +Z: 0 faces south, 9000 east, 18000 north, 27000 west. A room's placement puts its frame on its floor's
// frame (building.hpp: the hall along +X from the elevator at its west end, the north row's rooms facing south).

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/catalog/ids.hpp>
#include <rv/world/ids.hpp>

namespace rv::world {

/// The plan file's format (ADR-0013 §5): schema 2, the building, is the only one written; schema 1 (I3's one room) is
/// read and lifted (lift.hpp). A plan of a newer schema is refused (plan_json.hpp).
inline constexpr std::int64_t kPlanSchemaVersion = 2;
/// The oldest schema this program reads (and lifts).
inline constexpr std::int64_t kOldestPlanSchemaVersion = 1;

/// The generator's version (ADR-0010 §8): a minor change adds rules that keep every existing placement, a major one
/// may place games differently and needs an explicit regeneration (ADR-0010 §10). Text form "major.minor" ("1.0").
struct GeneratorVersion {
    std::uint32_t major = 1;
    std::uint32_t minor = 0;

    [[nodiscard]] std::string toString() const;
    /// Decimal numbers without leading zeros, each up to 999,999.
    [[nodiscard]] static std::optional<GeneratorVersion> parse(std::string_view text) noexcept;

    auto operator<=>(const GeneratorVersion&) const = default;
};

/// What a fixture is in its room.
enum class FixtureRole : std::uint8_t {
    HeroModule, ///< a spine-out module of the hero wall, carrying a level of the hero band
    SideModule, ///< a spine-out module in a side-wall socket
    Display,    ///< a display fixture: the featured wall shelf or a display case (face-out, the featured titles)
    HeroCorner, ///< a part of the hero corner (the TV cabinet, the CRT, the console, the rug, the armchair, ...)
    Decoration, ///< decoration in a socket the collection does not need (a poster, a lamp, a sideboard, ...)
};

/// "hero-module", "side-module", "display", "hero-corner", "decoration".
[[nodiscard]] constexpr std::string_view toString(FixtureRole role) noexcept {
    switch (role) {
    case FixtureRole::HeroModule:
        return "hero-module";
    case FixtureRole::SideModule:
        return "side-module";
    case FixtureRole::Display:
        return "display";
    case FixtureRole::HeroCorner:
        return "hero-corner";
    case FixtureRole::Decoration:
        return "decoration";
    }
    return "unknown";
}
[[nodiscard]] constexpr std::optional<FixtureRole> parseFixtureRole(std::string_view text) noexcept {
    return detail::enumFromString(text, FixtureRole::Decoration);
}

/// Which section of the collection a level of slots belongs to (ADR-0010 §3): its titles stand face-out on the display
/// fixtures (Featured) and in the hero band (Band), spine-out everywhere else (Spine).
enum class Section : std::uint8_t {
    Featured,
    Band,
    Spine,
};

/// "featured", "band", "spine".
[[nodiscard]] constexpr std::string_view toString(Section section) noexcept {
    switch (section) {
    case Section::Featured:
        return "featured";
    case Section::Band:
        return "band";
    case Section::Spine:
        return "spine";
    }
    return "unknown";
}
[[nodiscard]] constexpr std::optional<Section> parseSection(std::string_view text) noexcept {
    return detail::enumFromString(text, Section::Spine);
}

/// Face-out sections show the cover, the spine-out one the spine.
[[nodiscard]] constexpr bool isFaceOut(Section section) noexcept {
    return section != Section::Spine;
}

/// Why an eligible or ineligible game of a room's system has no placement (ADR-0010 §3). Every game of the system that
/// the room does not show is listed with one, so the plan accounts for each; the quick catalog lists them all.
enum class UnplacedReason : std::uint8_t {
    DuplicateTitle, ///< another version of its title is the room's representative (UnplacedGame::representative)
    NoCover,        ///< it has no cover while other games of its system have one (WorldConfig's include "covered")
    Unavailable,    ///< its file was missing when it was first considered
    Capacity,       ///< the room at its template's limit holds no more titles, or its floor has no place for the room
    RoomKept,       ///< its room is kept as it is until a regeneration (ADR-0013 §5: an old template version, prefab
                    ///< levels that changed, another generator major), so the room places nothing new
};

/// "duplicate-title", "no-cover", "unavailable", "capacity", "room-kept".
[[nodiscard]] constexpr std::string_view toString(UnplacedReason reason) noexcept {
    switch (reason) {
    case UnplacedReason::DuplicateTitle:
        return "duplicate-title";
    case UnplacedReason::NoCover:
        return "no-cover";
    case UnplacedReason::Unavailable:
        return "unavailable";
    case UnplacedReason::Capacity:
        return "capacity";
    case UnplacedReason::RoomKept:
        return "room-kept";
    }
    return "unknown";
}
[[nodiscard]] constexpr std::optional<UnplacedReason> parseUnplacedReason(std::string_view text) noexcept {
    return detail::enumFromString(text, UnplacedReason::RoomKept);
}

/// A floor of the building (ADR-0013 §1): building.json's id and its place in the elevator's order (0 the ground
/// floor). A floor is in a plan when it holds a room; the ground floor always is.
struct Floor {
    std::string id;
    std::uint32_t order = 0;

    auto operator<=>(const Floor&) const = default;
};

/// The ground floor's id (building.json's first floor: the lobby and the arcade room).
inline constexpr std::string_view kGroundFloor = "ground";

/// What a room of the building is (ADR-0013 §3, §5).
enum class RoomKind : std::uint8_t {
    Console, ///< a system's own room, "room:<system>"
    Shared,  ///< a floor's shared room, "area:shared:<floor>": a section per small system
    Arcade,  ///< the arcade game room on the ground floor, "area:arcade": a section per arcade system
};

/// "console", "shared", "arcade".
[[nodiscard]] constexpr std::string_view toString(RoomKind kind) noexcept {
    switch (kind) {
    case RoomKind::Console:
        return "console";
    case RoomKind::Shared:
        return "shared";
    case RoomKind::Arcade:
        return "arcade";
    }
    return "unknown";
}
[[nodiscard]] constexpr std::optional<RoomKind> parseRoomKind(std::string_view text) noexcept {
    return detail::enumFromString(text, RoomKind::Arcade);
}

/// Where a room stands on its floor (ADR-0013 §7): its frame's origin in the floor's frame, in millimetres, and its
/// yaw in half turns (0: the north row, its doorway facing south onto the hall; 1: the south row, turned by half a
/// turn). x is set when the room enters the plan and never changes; z follows the room's depth, so that its doorway
/// wall always abuts the hall (building.hpp's placement functions).
struct RoomPlacement {
    std::int64_t xMm = 0;
    std::int64_t zMm = 0;
    std::uint32_t yawHalfTurns = 0;

    auto operator<=>(const RoomPlacement&) const = default;
};

/// A room of the world.
struct Room {
    RoomId id;
    /// Its kind, its floor (one of the plan's floors), the systems it holds in their order (exactly its own for a
    /// console room) and its place on the floor (ADR-0013 §5). Schema 1's room, read and not yet lifted, has the
    /// defaults (lift.hpp sets them).
    RoomKind kind = RoomKind::Console;
    std::string floor;
    std::vector<catalog::SystemName> sections;
    RoomPlacement placement;
    /// The generator that last generated or updated the room (a room kept as it is keeps its version); the plan's
    /// generatorVersion is the highest of its rooms'.
    GeneratorVersion generatorVersion;
    /// The room an expansion extends, the head of its chain (ADR-0013 §8: reserved, empty for every room I4 makes).
    RoomId expands;
    /// The template it was generated from and its version (ADR-0010 §8's templateVersion, kept with its room so that
    /// I4's rooms of several templates each record theirs).
    std::string templateId;
    std::uint32_t templateVersion = 1;
    /// The interior on the floor, in the room's frame (a room that grew extends south: maxZ grows, minZ stays).
    std::int64_t minXMm = 0;
    std::int64_t maxXMm = 0;
    std::int64_t minZMm = 0;
    std::int64_t maxZMm = 0;
    std::int64_t heightMm = 0;
    /// How many of the template's growth steps the room took (0 for the base size).
    std::uint32_t growthSteps = 0;
    /// The room's seed (ADR-0010 §7: deriveSeed(worldSeed, "room", id)).
    std::uint64_t seed = 0;
    /// The media family its games stand as (media_family.hpp) and the theme pack its decoration comes from, both from
    /// the system's platform manifest.
    std::string mediaFamily;
    std::string themePack;

    bool operator==(const Room&) const = default;
};

/// A level of a fixture's slots: how many slots it has and which section its titles belong to. Levels and slot counts
/// are the prefab's (its "Slot_<level>_<index>" sockets), recorded so that the plan can be checked and indexed without
/// building the prefab.
struct FixtureLevel {
    std::uint32_t level = 0;
    std::uint32_t slots = 0;
    Section section = Section::Spine;

    auto operator<=>(const FixtureLevel&) const = default;
};

/// A fixture: one prefab standing in the room.
struct Fixture {
    FixtureId id;
    FixtureRole role = FixtureRole::Decoration;
    PrefabRef prefab;
    /// The prefab's origin (its pivot: the middle of its footprint on the floor for floor-standing prefabs) in the
    /// room's frame, and its yaw.
    std::int64_t xMm = 0;
    std::int64_t yMm = 0;
    std::int64_t zMm = 0;
    std::int32_t yawCdeg = 0;
    /// The prefab's bounds, rounded to whole millimetres once (ADR-0010 §5): width along its local X, height, depth
    /// along its local Z. With the pivot and the yaw they give the footprint the validation checks.
    std::int64_t widthMm = 0;
    std::int64_t heightMm = 0;
    std::int64_t depthMm = 0;
    /// The levels of its slots, by level; empty for a fixture without slots.
    std::vector<FixtureLevel> levels;

    bool operator==(const Fixture&) const = default;
};

/// A game standing in a slot.
struct Placement {
    SlotId slot;
    catalog::GameId game;
    /// The small deterministic offset that keeps a row from looking machined (ADR-0010 §6), drawn from the placement's
    /// seed: a spine-out item pushed in by 0 to 4 mm, a face-out item turned by at most 1.5 degrees; 0 when it does not
    /// apply.
    std::int64_t insetMm = 0;
    std::int32_t yawCdeg = 0;

    auto operator<=>(const Placement&) const = default;
};

/// Shelf dressing on slots no title needs (ADR-0010 §5): `count` slots of one level from `firstSlot`, given to one
/// prop chosen from the level's prop seed, so that no board stands bare. Dressing is decoration and gives way when a
/// title needs the level (ADR-0010 §10).
struct DressedLevel {
    LevelId level;
    std::uint32_t firstSlot = 0;
    std::uint32_t count = 0;
    PrefabRef prop;

    auto operator<=>(const DressedLevel&) const = default;
};

/// What a light of a room is for (ADR-0010 §4; I3 step 3). The plan places the lights; their colour, intensity, range
/// and cones come from the lighting profile when the room is drawn (I3 steps 10a and 16), so that switching profiles
/// moves nothing. Each plan light is exactly one point or spot light of the room's scene (lightKind()), so the
/// renderer's limit counts plan lights: at most the template's lightLimit (kMaxSceneLights, 16).
enum class LightRole : std::uint8_t {
    Key,        ///< the ceiling spot on the hero wall: the room's only shadowed light
    Pendant,    ///< a point light over the circulation band
    Lamp,       ///< a lamp's bulb: the hero corner's floor lamp, a lamp standing as decoration
    Picture,    ///< a spot on the sign above the hero corner
    WallWasher, ///< a spot over a run of collection modules on one wall
};

/// "key", "pendant", "lamp", "picture", "wall-washer".
[[nodiscard]] constexpr std::string_view toString(LightRole role) noexcept {
    switch (role) {
    case LightRole::Key:
        return "key";
    case LightRole::Pendant:
        return "pendant";
    case LightRole::Lamp:
        return "lamp";
    case LightRole::Picture:
        return "picture";
    case LightRole::WallWasher:
        return "wall-washer";
    }
    return "unknown";
}
[[nodiscard]] constexpr std::optional<LightRole> parseLightRole(std::string_view text) noexcept {
    return detail::enumFromString(text, LightRole::WallWasher);
}

/// Whether a light shines from a point in every direction or as a cone towards its target.
enum class LightKind : std::uint8_t {
    Point,
    Spot,
};

/// The pendant and the lamps are point lights, the key, the picture light and the wall-washers spots.
[[nodiscard]] constexpr LightKind lightKind(LightRole role) noexcept {
    return role == LightRole::Pendant || role == LightRole::Lamp ? LightKind::Point : LightKind::Spot;
}

/// A light of a room. Its id is a FixtureId naming what it serves: "<room>/fx:hero.key-light" (the key),
/// "fx:hero.sign-light" (the picture light), "fx:hero.lamp-light" (the hero corner's lamp), "fx:<socket>.light" (a lamp
/// standing as decoration in that socket), "fx:<socket>.washer" (a wall-washer, by the first socket of its run in fill
/// order), "fx:room.pendant" (the pendant: "room" is no socket, the room itself).
struct PlanLight {
    FixtureId id;
    LightRole role = LightRole::Key;
    /// Where it shines from, in the room's frame.
    std::int64_t xMm = 0;
    std::int64_t yMm = 0;
    std::int64_t zMm = 0;
    /// Where a spot is aimed; a point light's own position.
    std::int64_t targetXMm = 0;
    std::int64_t targetYMm = 0;
    std::int64_t targetZMm = 0;
    /// The width a spot has to light at its target (the run of modules a wall-washer covers, the sign, the hero wall's
    /// collection for the key); 0 for a point light.
    std::int64_t spanMm = 0;

    auto operator<=>(const PlanLight&) const = default;
};

/// A game of a room's system that the room does not show, with the reason.
struct UnplacedGame {
    catalog::GameId game;
    RoomId room;
    UnplacedReason reason = UnplacedReason::NoCover;
    /// The title's representative, for DuplicateTitle only ("no game" otherwise).
    catalog::GameId representative;

    auto operator<=>(const UnplacedGame&) const = default;
};

/// The plan of a library's world. Its canonical text (plan_json.hpp) sorts every list; the plan's digest is the SHA-256
/// of that text, so it is not a field.
struct WorldPlan {
    std::int64_t schemaVersion = kPlanSchemaVersion;
    /// The highest of the rooms' versions (the generator's own when the plan has no room).
    GeneratorVersion generatorVersion;
    /// core::kRandomAlgorithm ("splitmix64/1").
    std::string randomAlgorithm;
    /// The rule of the title keys that chose the representatives ("title-key/1", rv/world/title_key.hpp).
    std::string titleKeyRule;
    /// catalog::kSortKeyVersion of the sort keys the allocation ordered by.
    std::uint32_t catalogSortKeyVersion = 0;
    catalog::LibraryId library;
    /// The world seed (WorldConfig) and the preset the plan was generated with.
    std::uint64_t seed = 0;
    std::string preset;
    /// The SHA-256 of the canonical form of the inputs the generator read, 64 lowercase hexadecimal digits.
    std::string inputDigest;

    /// The floors holding a room, and the ground floor, by order.
    std::vector<Floor> floors;
    std::vector<Room> rooms;
    std::vector<Fixture> fixtures;
    std::vector<Placement> placements;
    std::vector<DressedLevel> dressing;
    std::vector<PlanLight> lights;
    std::vector<UnplacedGame> unplaced;

    bool operator==(const WorldPlan&) const = default;
};

/// The order of each list in the canonical text (ADR-0010 §8): floors by order then id, rooms by id, fixtures by id
/// (their levels by level), placements by slot then game, dressing by level then first slot, lights by id, unplaced by
/// game then room, reason and representative. Ids order by their parts (ids.hpp).
void sortPlan(WorldPlan& plan);

/// A lowercase hexadecimal SHA-256 digest: exactly 64 of 0-9 and a-f.
[[nodiscard]] bool isSha256Hex(std::string_view text) noexcept;

} // namespace rv::world
