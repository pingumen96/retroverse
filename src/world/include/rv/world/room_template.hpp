#pragma once

// RoomTemplate (ADR-0010 §4): assets/world/templates/<id>.json, the shape of a room and the sockets the generator
// fills. Rectangular rooms only in I3 (L-shaped and lounge rooms are I4's).
//
// The frame is the plan's (world_plan.hpp): millimetres, +X east, +Y up, north = -Z, the origin on the floor at the
// centre of the base room, whose interior is widthMm x depthMm. The room grows south only, in growthStepMm steps up to
// maxDepthMm: each step moves the south wall, and every socket on it (wall "south") and the doorway when it is in the
// south wall, south by one step, and adds the side-wall sockets of that step (growthStep); nothing else moves.
//
// A socket is a place on the floor: its anchor is the middle of its back edge (against the wall for a wall socket),
// its yaw turns the local frame (+Z out of the back edge, +X to the right as one faces the socket, the prefab's front
// facing +Z), and it reaches widthMm along local X and depthMm along local Z. Sockets are axis-aligned (yaws 0, 9000,
// 18000, 27000); a wall socket's yaw faces into the room (north 0, west 9000, south 18000, east 27000) and its back
// edge lies at most kMaxWallGapMm from the wall's interior face.
//
// The file (JSON with comments; every key required unless noted):
//   {"schemaVersion": 1, "id": "console_room_9x10", "version": 1,
//    "room": {"widthMm": 9000, "depthMm": 10000, "heightMm": 3200, "wallThicknessMm": 150},
//    "doorway": {"wall": "south", "offsetMm": 0, "widthMm": 1600, "heightMm": 2300, "clearWidthMm": 1600,
//                "clearDepthMm": 1500},
//    "circulationWidthMm": 1400, "eyeHeightMm": 1300, "growth": {"stepMm": 2000, "maxDepthMm": 16000},
//    "lightLimit": 16,
//    "modules": {"hero": <prefab>, "side": <prefab>},
//    "sockets": [{"id": "w01", "kind": "side-wall", "wall": "west", "xMm": -4470, "zMm": -4000, "yawCdeg": 9000,
//                 "widthMm": 900, "depthMm": 400, "growthStep": 0 (optional, default 0),
//                 "prefab": <prefab> (display sockets only), "parts": [...] (the hero corner only)}, ...],
//    "collectionOrder": ["hw-w", "hw-e", "w01", "e01", ...], "heroBand": ["hw-w", "hw-e"],
//    "displayOrder": ["fs01", "dc01", "dc02"],
//    "decoration": [{"name": "poster", "prefab": <prefab>, "mountHeightMm": 1500}, ...],
//    "shelfDressing": [<prefab>, ...]}
// <prefab> is {"manifest": <a prefab id>, "kind": <a prefab kind>}: prefabs are named by manifest id, never by a model
// path (specification §17.3). A hero corner part is {"role", "prefab" (null for the "console", which is the platform
// manifest's), "xMm", "yMm", "zMm", "yawCdeg"} in the socket's local frame.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/core/expected.hpp>
#include <rv/world/ids.hpp>

namespace rv::world {

/// The farthest a wall socket's back edge stands from its wall's interior face (the wainscot, a skirting).
inline constexpr std::int64_t kMaxWallGapMm = 100;
/// The renderer's point and spot lights per scene (gl::packLights), the most a template may allow.
inline constexpr std::uint32_t kMaxSceneLights = 16;

/// A wall of the room, by its direction from the centre (north = -Z); None for a free-standing socket.
enum class Wall : std::uint8_t {
    North,
    South,
    East,
    West,
    None,
};

/// "north", "south", "east", "west", "none".
[[nodiscard]] constexpr std::string_view toString(Wall wall) noexcept {
    switch (wall) {
    case Wall::North:
        return "north";
    case Wall::South:
        return "south";
    case Wall::East:
        return "east";
    case Wall::West:
        return "west";
    case Wall::None:
        return "none";
    }
    return "unknown";
}
[[nodiscard]] constexpr std::optional<Wall> parseWall(std::string_view text) noexcept {
    return detail::enumFromString(text, Wall::None);
}

/// What a socket is for (ADR-0010 §4).
enum class SocketKind : std::uint8_t {
    HeroModule,   ///< a hero-wall module, always placed (spine-out, carrying the hero band)
    SideWall,     ///< a side-wall socket: a spine-out module when the collection needs it, decoration otherwise
    DoorReserved, ///< never occupied: M3 opens a doorway there without moving anything
    Display,      ///< a display fixture (the featured wall shelf, a display case): face-out, the featured titles
    HeroCorner,   ///< the hero corner: the TV on its cabinet, the console, the sign, the rug, the armchair, a lamp
    Decoration,   ///< decoration only (posters, lamps), within the decoration density
};

/// "hero-module", "side-wall", "door-reserved", "display", "hero-corner", "decoration".
[[nodiscard]] constexpr std::string_view toString(SocketKind kind) noexcept {
    switch (kind) {
    case SocketKind::HeroModule:
        return "hero-module";
    case SocketKind::SideWall:
        return "side-wall";
    case SocketKind::DoorReserved:
        return "door-reserved";
    case SocketKind::Display:
        return "display";
    case SocketKind::HeroCorner:
        return "hero-corner";
    case SocketKind::Decoration:
        return "decoration";
    }
    return "unknown";
}
[[nodiscard]] constexpr std::optional<SocketKind> parseSocketKind(std::string_view text) noexcept {
    return detail::enumFromString(text, SocketKind::Decoration);
}

/// An axis-aligned rectangle on the floor, in millimetres; edges that touch do not overlap.
struct Footprint {
    std::int64_t minXMm = 0;
    std::int64_t minZMm = 0;
    std::int64_t maxXMm = 0;
    std::int64_t maxZMm = 0;

    [[nodiscard]] bool overlaps(const Footprint& other) const noexcept {
        return minXMm < other.maxXMm && other.minXMm < maxXMm && minZMm < other.maxZMm && other.minZMm < maxZMm;
    }
    [[nodiscard]] bool contains(const Footprint& other) const noexcept {
        return minXMm <= other.minXMm && other.maxXMm <= maxXMm && minZMm <= other.minZMm && other.maxZMm <= maxZMm;
    }

    bool operator==(const Footprint&) const = default;
};

/// A prefab of the hero corner, placed in its socket's local frame.
struct HeroPart {
    /// A socket name ("crt", "tv-cabinet"); the fixture's part (FixtureId "fx:hero.crt").
    std::string role;
    /// Nothing for the "console": the platform manifest's console stands there.
    std::optional<PrefabRef> prefab;
    std::int64_t xMm = 0;
    std::int64_t yMm = 0;
    std::int64_t zMm = 0;
    std::int32_t yawCdeg = 0;

    bool operator==(const HeroPart&) const = default;
};

struct TemplateSocket {
    std::string id;
    SocketKind kind = SocketKind::Decoration;
    Wall wall = Wall::None;
    std::int64_t xMm = 0;
    std::int64_t zMm = 0;
    std::int32_t yawCdeg = 0;
    std::int64_t widthMm = 0;
    std::int64_t depthMm = 0;
    /// 0 for the base room, k for a side-wall socket that the k-th growth step adds.
    std::uint32_t growthStep = 0;
    /// A display socket's fixture.
    std::optional<PrefabRef> prefab;
    /// The hero corner's prefabs.
    std::vector<HeroPart> parts;

    bool operator==(const TemplateSocket&) const = default;
};

/// The entrance: a doorway in one wall, its centre `offsetMm` from the wall's middle (along +X for the north and south
/// walls, +Z for the east and west ones), and the clear area kept free inside it.
struct Doorway {
    Wall wall = Wall::South;
    std::int64_t offsetMm = 0;
    std::int64_t widthMm = 0;
    std::int64_t heightMm = 0;
    std::int64_t clearWidthMm = 0;
    std::int64_t clearDepthMm = 0;

    bool operator==(const Doorway&) const = default;
};

/// A decoration the generator may place in a socket the collection does not need: hung on the wall at
/// `mountHeightMm` (a poster) or standing on the floor (0).
struct DecorationChoice {
    std::string name;
    PrefabRef prefab;
    std::int64_t mountHeightMm = 0;

    bool operator==(const DecorationChoice&) const = default;
};

struct RoomTemplate {
    std::string id;
    std::uint32_t version = 1;
    std::int64_t widthMm = 0;
    std::int64_t depthMm = 0;
    std::int64_t heightMm = 0;
    std::int64_t wallThicknessMm = 0;
    Doorway doorway;
    /// The central circulation band's width (ADR-0010 §4) and the eye height the shelves' bands centre on (§5).
    std::int64_t circulationWidthMm = 0;
    std::int64_t eyeHeightMm = 0;
    /// 0 when the room never grows (then maxDepthMm is depthMm).
    std::int64_t growthStepMm = 0;
    std::int64_t maxDepthMm = 0;
    /// The point and spot lights a room of the template may hold (at most kMaxSceneLights).
    std::uint32_t lightLimit = kMaxSceneLights;
    /// The prefabs of the hero-wall modules and of the side-wall modules.
    PrefabRef heroModule;
    PrefabRef sideModule;
    /// In the file's order.
    std::vector<TemplateSocket> sockets;
    /// The collection's fill order (ADR-0010 §4): every hero-module and side-wall socket once, most visible first, the
    /// growth steps' sockets after the base room's.
    std::vector<std::string> collectionOrder;
    /// The hero-module sockets that carry the hero band, in its fill order (the west module first).
    std::vector<std::string> heroBand;
    /// Every display socket once, in the featured titles' fill order.
    std::vector<std::string> displayOrder;
    std::vector<DecorationChoice> decoration;
    /// The shelf dressing props (ADR-0010 §5), one chosen per dressed level.
    std::vector<PrefabRef> shelfDressing;

    /// The socket of `socketId`, or nullptr.
    [[nodiscard]] const TemplateSocket* socket(std::string_view socketId) const noexcept;
    /// How many growth steps the room can take: (maxDepthMm - depthMm) / growthStepMm.
    [[nodiscard]] std::uint32_t maxGrowthSteps() const noexcept;

    bool operator==(const RoomTemplate&) const = default;
};

/// The interior after `growthSteps` steps.
[[nodiscard]] Footprint interiorFootprint(const RoomTemplate& roomTemplate, std::uint32_t growthSteps) noexcept;
/// The floor a socket covers after `growthSteps` steps (a socket on the south wall moved with it).
[[nodiscard]] Footprint socketFootprint(const RoomTemplate& roomTemplate, const TemplateSocket& socket,
                                        std::uint32_t growthSteps) noexcept;
/// The clear area inside the doorway after `growthSteps` steps.
[[nodiscard]] Footprint doorwayClearance(const RoomTemplate& roomTemplate, std::uint32_t growthSteps) noexcept;

/// Reads a template. Errors (context and line): ParseError, ValidationFailed: a key missing, unknown, of another type
/// or out of range; a socket id twice, a yaw that is not axis-aligned or does not face into the room from its wall, a
/// back edge away from its wall; a socket outside the room, overlapping another or the doorway's clearance at any
/// growth step; a growth socket that is not a side-wall socket or a step beyond the limit; a prefab or parts where its
/// kind takes none (or none where it needs them); not exactly one hero corner, holding a "crt" and the "console"; a
/// part outside its socket; the orders not naming exactly their sockets once each, the collection's growth steps out of
/// order; decoration or dressing empty or twice.
[[nodiscard]] Expected<RoomTemplate> parseRoomTemplate(std::string_view text, std::string_view context);
/// Reads <assetRoot>/world/templates/<id>.json, whose "id" must be `id`. Errors: those of parseRoomTemplate() and of
/// reading the file, InvalidArgument for an id that is not a data id.
[[nodiscard]] Expected<RoomTemplate> loadRoomTemplate(const std::filesystem::path& assetRoot, std::string_view id);

} // namespace rv::world
