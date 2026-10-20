#pragma once

// The building (ADR-0013 §1, §2, §7; I4 step 1): its data and the floor layout's arithmetic, GPU-free and pure.
//
// assets/world/building.json (JSON with comments; every key required unless noted):
//   {"schemaVersion": 1,
//    "floors": [{"id": "ground", "kinds": ["arcade"]}, {"id": "gen2", "kinds": ["home"], "generations": [1, 2]}, ...],
//    "layout": {"hallWidthMm": 3000, "lobbyWidthMm": 6000, "rowStartXMm": 3000, "wallAllowanceMm": 150,
//               "maxRoomsPerRow": 12}}
// The floors in the elevator's order (a floor's order is its index; the first is the ground floor, kGroundFloor), each
// holding the systems of its kinds, and of those generations only when "generations" (optional, 1 to 9) is given. The
// reader requires every kind but "excluded" to land on exactly one floor at every generation from 1 to 9. Layout:
// the hall's and the ground floor's lobby's widths (even), the x where the rooms' rows start east of the hall's west
// end (the elevator's landing), the allowance a room's walls add around its interior on the floor, and the rooms a row
// holds at most.
//
// assets/world/systems.json (JSON with comments):
//   {"schemaVersion": 1,
//    "defaults": {"home": {"themePack": "classic_16bit_warm", "console": <prefab> or null,
//                          "plastic": {"cartridge": "#9A9A96", "console": "#D6CBAE", "accent": "#7D6E9E"}},
//                 "handheld": {...}, "arcade": {...}, "computer": {...}, "other": {...}},
//    "fallback": {"family": "generic_case", "order": 9999, "sign": "MORE GAMES"},
//    "systems": [{"system": "snes", "kind": "home", "generation": 4, "family": "cartridge_home_16bit",
//                 "order": 400, "sign": "16-BIT CORNER", "platforms": ["snes"]}, ...,
//                {"system": "desktop", "kind": "excluded", "platforms": ["pcwindows"]}]}
// A system's kind, its generation (1 to 9, exactly for home and handheld), its media family (a data id; the registry
// is checked where both are read), its entry order on its floor, a generic sign (1 to 32 printable ASCII characters)
// and its ES-DE platform tags (lowercase, at least one). An excluded system names its kind and platform tags only. No
// system twice. classify() reads it with building.json.
//
// The floor's frame (ADR-0013 §7): millimetres, +X east, +Y up, north = -Z, the origin on the hall's centre line at its
// west end, where the elevator stands; the hall (the ground floor's lobby) runs along +X between z = -width / 2 and
// z = +width / 2. Rooms stand in two rows: the north row (yaw 0, its doorway wall, the room's south wall, against the
// hall's north edge) and the south row (yaw half a turn, its doorway wall against the hall's south edge). A room's
// footprint on its floor is its interior (its current bounds) grown by the wall allowance on every side. A room
// entering a floor stands at the current east end of the shorter of the two rows (by the east end's x, the north row on
// a tie), unless that row holds maxRoomsPerRow rooms already; its x never changes after. Its z follows its depth
// (followDepth): a room that grows moves away from the hall as a whole, its contents keeping their place in its own
// frame.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/catalog/ids.hpp>
#include <rv/core/expected.hpp>
#include <rv/world/ids.hpp>
#include <rv/world/room_template.hpp>
#include <rv/world/world_plan.hpp>

namespace rv::world {

/// What a system is (ADR-0013 §2).
enum class SystemKind : std::uint8_t {
    Home,
    Handheld,
    Arcade,
    Computer,
    Other,
    Excluded, ///< no room: its games stay in the quick catalog
};

/// "home", "handheld", "arcade", "computer", "other", "excluded".
[[nodiscard]] constexpr std::string_view toString(SystemKind kind) noexcept {
    switch (kind) {
    case SystemKind::Home:
        return "home";
    case SystemKind::Handheld:
        return "handheld";
    case SystemKind::Arcade:
        return "arcade";
    case SystemKind::Computer:
        return "computer";
    case SystemKind::Other:
        return "other";
    case SystemKind::Excluded:
        return "excluded";
    }
    return "unknown";
}
[[nodiscard]] constexpr std::optional<SystemKind> parseSystemKind(std::string_view text) noexcept {
    return detail::enumFromString(text, SystemKind::Excluded);
}

/// The generations a home console or a handheld may have.
inline constexpr std::uint32_t kMaxGeneration = 9;

// --- building.json --------------------------------------------------------------------------------------------------

struct FloorSpec {
    std::string id;
    std::vector<SystemKind> kinds;
    /// The generations it holds of its kinds; empty: every generation.
    std::vector<std::uint32_t> generations;

    bool operator==(const FloorSpec&) const = default;
};

struct BuildingLayout {
    std::int64_t hallWidthMm = 3000;
    std::int64_t lobbyWidthMm = 6000;
    std::int64_t rowStartXMm = 3000;
    std::int64_t wallAllowanceMm = 150;
    std::uint32_t maxRoomsPerRow = 12;

    bool operator==(const BuildingLayout&) const = default;
};

struct BuildingData {
    /// In the elevator's order: a floor's order is its index, the first the ground floor.
    std::vector<FloorSpec> floors;
    BuildingLayout layout;

    /// The floor `id`, or null.
    [[nodiscard]] const FloorSpec* floor(std::string_view id) const noexcept;
    /// The order of the floor `id`, or nothing.
    [[nodiscard]] std::optional<std::uint32_t> order(std::string_view id) const noexcept;
    /// The floor holding `kind` at `generation` (0 for a kind without generations), or null (Excluded).
    [[nodiscard]] const FloorSpec* floorFor(SystemKind kind, std::uint32_t generation) const noexcept;
    /// The plan's record of the floor `id` (its order), or nothing.
    [[nodiscard]] std::optional<Floor> planFloor(std::string_view id) const;

    bool operator==(const BuildingData&) const = default;
};

/// Reads building.json. Errors (context and line): ParseError, ValidationFailed: a key missing, unknown, of another
/// type or out of range; a floor id that is not a data id or twice; the first floor not kGroundFloor; a kind twice on
/// a floor, "excluded" on one, generations on a floor of no generational kind; a kind and generation on no floor or on
/// two; a hall or lobby width that is not even and positive.
[[nodiscard]] Expected<BuildingData> parseBuildingData(std::string_view text, std::string_view context);
/// Reads <assetRoot>/world/building.json.
[[nodiscard]] Expected<BuildingData> loadBuildingData(const std::filesystem::path& assetRoot);
/// The canonical document of the building's data (the plans' inputDigest).
[[nodiscard]] nlohmann::json buildingDataToJson(const BuildingData& building);

// --- systems.json ---------------------------------------------------------------------------------------------------

/// A system's row (or the classification of a system the table does not name: classify()).
struct SystemEntry {
    catalog::SystemName system;
    SystemKind kind = SystemKind::Other;
    /// 1 to kMaxGeneration for home and handheld, 0 otherwise.
    std::uint32_t generation = 0;
    std::string family;
    std::uint32_t order = 0;
    std::string sign;
    std::vector<std::string> platforms;

    bool operator==(const SystemEntry&) const = default;
};

/// A kind's defaults: the theme pack, the console prefab (none: the hero corner stands without one) and the plastics.
struct KindDefaults {
    std::string themePack;
    std::optional<PrefabRef> console;
    std::string cartridgePlastic;
    std::string consolePlastic;
    std::string accentPlastic;

    bool operator==(const KindDefaults&) const = default;
};

struct SystemsData {
    /// In the file's order (the platform-tag fallback's).
    std::vector<SystemEntry> systems;
    /// By kind, every kind but Excluded.
    KindDefaults home;
    KindDefaults handheld;
    KindDefaults arcade;
    KindDefaults computer;
    KindDefaults other;
    /// What a system nobody's tags match is: kind Other, this family, order and sign.
    SystemEntry fallback;

    /// The row of `system`, or null.
    [[nodiscard]] const SystemEntry* find(const catalog::SystemName& system) const noexcept;
    /// The defaults of `kind` (Excluded: Other's).
    [[nodiscard]] const KindDefaults& defaultsOf(SystemKind kind) const noexcept;

    bool operator==(const SystemsData&) const = default;
};

/// Reads systems.json. Errors (context and line): ParseError, ValidationFailed: a key missing, unknown, of another type
/// or form; a system twice; a generation missing for home or handheld, or given for another kind; an excluded system
/// with anything but its kind and platform tags; a platform tag that is not lowercase.
[[nodiscard]] Expected<SystemsData> parseSystemsData(std::string_view text, std::string_view context);
/// Reads <assetRoot>/world/systems.json.
[[nodiscard]] Expected<SystemsData> loadSystemsData(const std::filesystem::path& assetRoot);
/// The canonical document of a row (the plans' inputDigest).
[[nodiscard]] nlohmann::json systemEntryToJson(const SystemEntry& entry);

/// How a system was classified.
enum class ClassificationSource : std::uint8_t {
    Table,       ///< its own row
    PlatformTag, ///< the row of the first system of the table sharing one of its platform tags
    Fallback,    ///< no row matched: kind Other, the fallback's family, order and sign
};

/// "table", "platform-tag", "fallback".
[[nodiscard]] constexpr std::string_view toString(ClassificationSource source) noexcept {
    switch (source) {
    case ClassificationSource::Table:
        return "table";
    case ClassificationSource::PlatformTag:
        return "platform-tag";
    case ClassificationSource::Fallback:
        return "fallback";
    }
    return "unknown";
}

/// What a system is in the building (ADR-0013 §2).
struct Classification {
    /// The row it takes, its `system` the classified system's own name (its platforms the classified system's tags
    /// for a tag match or the fallback).
    SystemEntry entry;
    ClassificationSource source = ClassificationSource::Table;
    /// The system whose row a tag matched (PlatformTag only).
    catalog::SystemName matched;
    /// Its floor (building.json's), empty for an excluded system.
    std::string floor;

    [[nodiscard]] SystemKind kind() const noexcept { return entry.kind; }
    /// It gets a room: not excluded, on a floor.
    [[nodiscard]] bool hasRoom() const noexcept { return entry.kind != SystemKind::Excluded && !floor.empty(); }
};

/// The classification of `system`, whose ES-DE platform tags are `platformTags` (lowercase; may be empty): its row in
/// `systems`, else the row of the first system of the table (in its order) that shares one of its tags, else kind Other
/// with the fallback's family, order and sign; then its floor in `building` (none for Excluded, or when no floor holds
/// its kind and generation).
[[nodiscard]] Classification classify(const catalog::SystemName& system, std::span<const std::string> platformTags,
                                      const SystemsData& systems, const BuildingData& building);

// --- The floor's layout ---------------------------------------------------------------------------------------------

/// The rows of a floor (ADR-0013 §7).
enum class Row : std::uint8_t {
    North, ///< yaw 0: the room's doorway (its south wall) facing south onto the hall
    South, ///< yaw half a turn: its doorway facing north onto the hall
};

/// The row of a placement (its yaw: 0 North, 1 South).
[[nodiscard]] constexpr Row rowOf(const RoomPlacement& placement) noexcept {
    return placement.yawHalfTurns == 0 ? Row::North : Row::South;
}

/// The width of a floor's hall: the lobby's on the ground floor, the hall's elsewhere.
[[nodiscard]] std::int64_t hallWidthOf(std::string_view floor, const BuildingLayout& layout) noexcept;

/// A point of a room's frame in its floor's frame (x, z in millimetres).
struct FloorPoint {
    std::int64_t xMm = 0;
    std::int64_t zMm = 0;

    auto operator<=>(const FloorPoint&) const = default;
};

/// The room's frame to its floor's: turned by its yaw (half a turn maps (x, z) to (-x, -z)), then moved by its
/// placement.
[[nodiscard]] FloorPoint roomToFloor(const RoomPlacement& placement, std::int64_t xMm, std::int64_t zMm) noexcept;
[[nodiscard]] Footprint roomToFloor(const RoomPlacement& placement, const Footprint& footprint) noexcept;
/// A yaw of the room's frame (hundredths of a degree) in the floor's, from 0 to 35999.
[[nodiscard]] std::int32_t roomYawToFloor(const RoomPlacement& placement, std::int32_t yawCdeg) noexcept;

/// A room's footprint in its own frame: its interior grown by the wall allowance.
[[nodiscard]] Footprint roomFootprint(const Room& room, const BuildingLayout& layout) noexcept;
/// A room's footprint on its floor, as it stands.
[[nodiscard]] Footprint floorFootprint(const Room& room, const BuildingLayout& layout) noexcept;

/// The z of a room's origin whose doorway wall (its south wall, maxZMm plus the wall allowance) abuts the hall of
/// `floor` from `row`, at the room's current depth.
[[nodiscard]] std::int64_t abuttingZ(const Room& room, Row row, std::string_view floor,
                                     const BuildingLayout& layout) noexcept;
/// The room's placement with its z following its current depth (x and yaw kept).
[[nodiscard]] RoomPlacement followDepth(const Room& room, const BuildingLayout& layout) noexcept;

/// The two rows of a floor as its rooms stand.
struct FloorRows {
    /// The x of each row's east end: the rows' start, or the easternmost edge of the row's rooms' footprints.
    std::int64_t northEndMm = 0;
    std::int64_t southEndMm = 0;
    std::uint32_t northRooms = 0;
    std::uint32_t southRooms = 0;

    bool operator==(const FloorRows&) const = default;
};

/// The rows of the rooms of `rooms` that stand on `floor`.
[[nodiscard]] FloorRows floorRows(std::span<const Room> rooms, std::string_view floor, const BuildingLayout& layout);

/// Where `room` (its bounds; its floor the one `rows` describe) stands when it enters its floor: at the east end of the
/// shorter row that holds fewer than maxRoomsPerRow rooms, the north row on a tie, its west footprint edge on that end,
/// its z abutting the hall. Nothing when both rows are full.
[[nodiscard]] std::optional<RoomPlacement> placeEnteringRoom(const Room& room, const FloorRows& rows,
                                                             const BuildingLayout& layout);

} // namespace rv::world
