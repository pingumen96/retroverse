#include <rv/world/room_template.hpp>

#include <algorithm>
#include <map>
#include <set>
#include <utility>

#include <rv/assets/prefab_manifest.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>

#include "data_reader.hpp"

namespace rv::world {
namespace {

using detail::DocumentReader;
using detail::ObjectReader;
using detail::pointerChild;
using Json = nlohmann::json;

constexpr std::int64_t kMaxRoomMm = 50'000;
constexpr std::int64_t kMaxCoordinateMm = 100'000;
constexpr std::int64_t kMaxVersion = 999'999;
/// A doorway stays this far from the room's corners (its casing and the shell's margin, RoomShellParams).
constexpr std::int64_t kDoorCornerMarginMm = 100;

/// The integer directions of an axis-aligned yaw: the socket's front (local +Z) and right (local +X) in the room.
struct Axes {
    std::int64_t frontX = 0;
    std::int64_t frontZ = 1;
    std::int64_t rightX = 1;
    std::int64_t rightZ = 0;
};

bool isAxisAligned(std::int32_t yawCdeg) noexcept {
    return yawCdeg == 0 || yawCdeg == 9000 || yawCdeg == 18000 || yawCdeg == 27000;
}

/// Rotating +Z by the yaw (right-handed about +Y) gives the front (sin, cos), rotating +X the right (cos, -sin).
Axes axesOf(std::int32_t yawCdeg) noexcept {
    switch (yawCdeg) {
    case 9000:
        return {1, 0, 0, -1};
    case 18000:
        return {0, -1, -1, 0};
    case 27000:
        return {-1, 0, 0, 1};
    default:
        return {0, 1, 1, 0};
    }
}

/// The yaw that faces into the room from `wall`.
std::optional<std::int32_t> inwardYaw(Wall wall) noexcept {
    switch (wall) {
    case Wall::North:
        return 0;
    case Wall::West:
        return 9000;
    case Wall::South:
        return 18000;
    case Wall::East:
        return 27000;
    case Wall::None:
        break;
    }
    return std::nullopt;
}

/// A length split in two integer halves, the extra millimetre of an odd length on the far side.
std::int64_t nearHalf(std::int64_t length) noexcept {
    return length / 2;
}
std::int64_t farHalf(std::int64_t length) noexcept {
    return length - length / 2;
}

/// The rectangle from an anchor (the middle of the back edge) along an axis-aligned yaw.
Footprint rectangle(std::int64_t x, std::int64_t z, std::int32_t yawCdeg, std::int64_t width, std::int64_t depth) {
    const Axes axes = axesOf(yawCdeg);
    const std::int64_t xs[] = {x - axes.rightX * nearHalf(width), x + axes.rightX * farHalf(width)};
    const std::int64_t zs[] = {z - axes.rightZ * nearHalf(width), z + axes.rightZ * farHalf(width)};
    Footprint footprint{std::min(xs[0], xs[1]), std::min(zs[0], zs[1]), std::max(xs[0], xs[1]), std::max(zs[0], zs[1])};
    const std::int64_t frontX = axes.frontX * depth;
    const std::int64_t frontZ = axes.frontZ * depth;
    footprint.minXMm += std::min<std::int64_t>(0, frontX);
    footprint.maxXMm += std::max<std::int64_t>(0, frontX);
    footprint.minZMm += std::min<std::int64_t>(0, frontZ);
    footprint.maxZMm += std::max<std::int64_t>(0, frontZ);
    return footprint;
}

std::int64_t southShift(const RoomTemplate& roomTemplate, std::uint32_t growthSteps) noexcept {
    return roomTemplate.growthStepMm * static_cast<std::int64_t>(growthSteps);
}

std::string describe(const Footprint& footprint) {
    return "x " + std::to_string(footprint.minXMm) + " to " + std::to_string(footprint.maxXMm) + ", z " +
           std::to_string(footprint.minZMm) + " to " + std::to_string(footprint.maxZMm);
}

/// The names of `names` (an array member) checked against the sockets: each names a socket of `accepted` kind, once;
/// with `all`, every socket of those kinds is named.
std::vector<std::string> readOrder(const ObjectReader& reader, std::string_view key,
                                   const std::map<std::string, const TemplateSocket*>& sockets,
                                   std::initializer_list<SocketKind> accepted, bool all, std::string_view what) {
    std::vector<std::string> names;
    DocumentReader& document = reader.document();
    const std::string arrayPointer = reader.pointer(key);
    detail::forEachElement(reader.array(key), arrayPointer, [&](const Json& element, const std::string& pointer) {
        std::string name = detail::readString(document, element, pointer);
        if (document.failed()) {
            return;
        }
        const auto found = sockets.find(name);
        if (found == sockets.end()) {
            document.fail(pointer, "names no socket: \"" + name + "\"");
        } else if (std::find(accepted.begin(), accepted.end(), found->second->kind) == accepted.end()) {
            document.fail(pointer, "\"" + name + "\" is a " + std::string(toString(found->second->kind)) +
                                       " socket, not " + std::string(what));
        } else if (std::find(names.begin(), names.end(), name) != names.end()) {
            document.fail(pointer, "names \"" + name + "\" twice");
        }
        names.push_back(std::move(name));
    });
    if (all && !document.failed()) {
        for (const auto& [id, socket] : sockets) {
            if (std::find(accepted.begin(), accepted.end(), socket->kind) != accepted.end() &&
                std::find(names.begin(), names.end(), id) == names.end()) {
                document.fail(arrayPointer, "does not name the socket \"" + id + "\"");
                break;
            }
        }
    }
    return names;
}

HeroPart readPart(const ObjectReader& reader) {
    HeroPart part;
    part.role = reader.checkedString("role", isValidSocketName, "a part name of a-z, 0-9 and '-'");
    if (!reader.value("prefab").is_null()) {
        part.prefab = reader.prefab("prefab");
    }
    part.xMm = reader.integer("xMm", -kMaxCoordinateMm, kMaxCoordinateMm);
    part.yMm = reader.integer("yMm", 0, kMaxRoomMm);
    part.zMm = reader.integer("zMm", -kMaxCoordinateMm, kMaxCoordinateMm);
    part.yawCdeg = static_cast<std::int32_t>(reader.integer("yawCdeg", 0, 35'999));
    return part;
}

TemplateSocket readSocket(const ObjectReader& reader) {
    TemplateSocket socket;
    socket.id = reader.checkedString("id", isValidSocketName, "a socket name of a-z, 0-9 and '-'");
    socket.kind = reader.enumeration<SocketKind>(
        "kind", parseSocketKind, "one of hero-module, side-wall, door-reserved, display, hero-corner, decoration");
    socket.wall = reader.enumeration<Wall>("wall", parseWall, "one of north, south, east, west, none");
    socket.xMm = reader.integer("xMm", -kMaxCoordinateMm, kMaxCoordinateMm);
    socket.zMm = reader.integer("zMm", -kMaxCoordinateMm, kMaxCoordinateMm);
    socket.yawCdeg = static_cast<std::int32_t>(reader.integer("yawCdeg", 0, 35'999));
    socket.widthMm = reader.integer("widthMm", 1, kMaxRoomMm);
    socket.depthMm = reader.integer("depthMm", 1, kMaxRoomMm);
    socket.growthStep = static_cast<std::uint32_t>(reader.optionalInteger("growthStep", 0, 1'000).value_or(0));
    DocumentReader& document = reader.document();
    if (document.failed()) {
        return socket;
    }
    if (!isAxisAligned(socket.yawCdeg)) {
        document.fail(reader.pointer("yawCdeg"), "a socket's yaw is 0, 9000, 18000 or 27000");
    } else if (const auto inward = inwardYaw(socket.wall); inward && *inward != socket.yawCdeg) {
        document.fail(reader.pointer("yawCdeg"), "a socket on the " + std::string(toString(socket.wall)) +
                                                     " wall faces into the room: yaw " + std::to_string(*inward));
    }
    const bool takesPrefab = socket.kind == SocketKind::Display;
    const bool takesParts = socket.kind == SocketKind::HeroCorner;
    if (takesPrefab) {
        socket.prefab = reader.prefab("prefab");
    } else if (!document.failed() && reader.has("prefab")) {
        document.fail(reader.pointer("prefab"), "only a display socket names its prefab");
    }
    if (takesParts) {
        const Json& parts = reader.array("parts");
        detail::forEachElement(parts, reader.pointer("parts"), [&](const Json& element, const std::string& pointer) {
            socket.parts.push_back(
                readPart(ObjectReader(document, element, pointer, {"prefab", "role", "xMm", "yMm", "yawCdeg", "zMm"})));
        });
        if (!document.failed() && socket.parts.empty()) {
            document.fail(reader.pointer("parts"), "the hero corner needs its parts");
        }
    } else if (!document.failed() && reader.has("parts")) {
        document.fail(reader.pointer("parts"), "only the hero corner has parts");
    }
    if (!document.failed() && socket.kind != SocketKind::SideWall && socket.growthStep != 0) {
        document.fail(reader.pointer("growthStep"), "only side-wall sockets come with a growth step");
    }
    if (!document.failed() && socket.wall == Wall::None &&
        (socket.kind == SocketKind::HeroModule || socket.kind == SocketKind::SideWall ||
         socket.kind == SocketKind::DoorReserved)) {
        document.fail(reader.pointer("wall"), "a " + std::string(toString(socket.kind)) + " socket stands on a wall");
    }
    return socket;
}

/// The hero corner's parts: unique roles, the "console" without a prefab and every other with one, each inside the
/// socket; a "crt" and the "console" present.
void checkParts(DocumentReader& document, const TemplateSocket& socket, const std::string& socketPointer,
                std::int64_t roomHeight) {
    std::set<std::string> roles;
    const std::string partsPointer = pointerChild(socketPointer, "parts");
    for (std::size_t i = 0; i < socket.parts.size() && !document.failed(); ++i) {
        const HeroPart& part = socket.parts[i];
        const std::string pointer = pointerChild(partsPointer, i);
        if (!roles.insert(part.role).second) {
            document.fail(pointerChild(pointer, "role"), "the role appears twice");
        } else if ((part.role == "console") == part.prefab.has_value()) {
            document.fail(pointerChild(pointer, "prefab"), part.role == "console"
                                                               ? "the console is the platform manifest's: null"
                                                               : "a part other than the console names its prefab");
        } else if (part.xMm < -nearHalf(socket.widthMm) || part.xMm > farHalf(socket.widthMm) || part.zMm < 0 ||
                   part.zMm > socket.depthMm || part.yMm > roomHeight) {
            document.fail(pointer, "the part stands outside its socket");
        }
    }
    if (!document.failed() && (!roles.contains("crt") || !roles.contains("console"))) {
        document.fail(partsPointer, "the hero corner holds a \"crt\" (the hero TV) and the \"console\"");
    }
}

void checkGeometry(DocumentReader& document, const RoomTemplate& roomTemplate) {
    const Footprint base = interiorFootprint(roomTemplate, 0);
    // The doorway in its wall, clear of the corners and under the ceiling.
    const bool northSouth = roomTemplate.doorway.wall == Wall::North || roomTemplate.doorway.wall == Wall::South;
    const std::int64_t wallLength = northSouth ? roomTemplate.widthMm : roomTemplate.depthMm;
    const Doorway& door = roomTemplate.doorway;
    if (door.wall == Wall::None) {
        document.fail("/doorway/wall", "the doorway is in a wall");
    } else if (roomTemplate.growthStepMm > 0 && (door.wall == Wall::East || door.wall == Wall::West)) {
        // An east or west doorway would stand in a wall that grows: kept out until a template needs one.
        document.fail("/doorway/wall", "a room that grows has its doorway in the north or south wall");
    } else if (door.offsetMm - nearHalf(door.widthMm) < -nearHalf(wallLength) + kDoorCornerMarginMm ||
               door.offsetMm + farHalf(door.widthMm) > farHalf(wallLength) - kDoorCornerMarginMm) {
        document.fail("/doorway/offsetMm", "the doorway leaves its wall (or stands within " +
                                               std::to_string(kDoorCornerMarginMm) + " mm of a corner)");
    } else if (door.heightMm > roomTemplate.heightMm - kDoorCornerMarginMm) {
        document.fail("/doorway/heightMm", "the doorway reaches the ceiling");
    }
    if (document.failed()) {
        return;
    }
    if (!base.contains(doorwayClearance(roomTemplate, 0))) {
        document.fail("/doorway", "the doorway's clearance leaves the room");
        return;
    }

    std::map<std::string, std::size_t> indexOf;
    for (std::size_t i = 0; i < roomTemplate.sockets.size(); ++i) {
        indexOf.emplace(roomTemplate.sockets[i].id, i);
    }
    const std::uint32_t maxSteps = roomTemplate.maxGrowthSteps();
    for (std::size_t i = 0; i < roomTemplate.sockets.size() && !document.failed(); ++i) {
        const TemplateSocket& socket = roomTemplate.sockets[i];
        const std::string pointer = pointerChild("/sockets", i);
        if (socket.growthStep > maxSteps) {
            document.fail(pointerChild(pointer, "growthStep"),
                          "the room takes at most " + std::to_string(maxSteps) + " growth steps");
            break;
        }
        // The back edge against its wall (the south wall's position in the base room: both move together).
        std::optional<std::int64_t> gap;
        switch (socket.wall) {
        case Wall::North:
            gap = socket.zMm - base.minZMm;
            break;
        case Wall::South:
            gap = base.maxZMm - socket.zMm;
            break;
        case Wall::West:
            gap = socket.xMm - base.minXMm;
            break;
        case Wall::East:
            gap = base.maxXMm - socket.xMm;
            break;
        case Wall::None:
            break;
        }
        if (gap && (*gap < 0 || *gap > kMaxWallGapMm)) {
            document.fail(pointer, "the socket's back edge stands " + std::to_string(*gap) + " mm from the " +
                                       std::string(toString(socket.wall)) + " wall (0 to " +
                                       std::to_string(kMaxWallGapMm) + ")");
            break;
        }
        const Footprint footprint = socketFootprint(roomTemplate, socket, socket.growthStep);
        if (!interiorFootprint(roomTemplate, socket.growthStep).contains(footprint)) {
            document.fail(pointer, "the socket (" + describe(footprint) + ") leaves the room");
            break;
        }
        if (socket.kind == SocketKind::HeroCorner) {
            checkParts(document, socket, pointer, roomTemplate.heightMm);
        }
    }
    // No two sockets overlap, nor one the doorway's clearance, in the base room or after any growth step.
    for (std::uint32_t steps = 0; steps <= maxSteps && !document.failed(); ++steps) {
        const Footprint clearance = doorwayClearance(roomTemplate, steps);
        for (std::size_t i = 0; i < roomTemplate.sockets.size() && !document.failed(); ++i) {
            const TemplateSocket& socket = roomTemplate.sockets[i];
            if (socket.growthStep > steps) {
                continue;
            }
            const Footprint footprint = socketFootprint(roomTemplate, socket, steps);
            if (footprint.overlaps(clearance)) {
                document.fail(pointerChild("/sockets", i), "the socket enters the doorway's clearance after " +
                                                               std::to_string(steps) + " growth step(s)");
                break;
            }
            for (std::size_t j = 0; j < i; ++j) {
                const TemplateSocket& other = roomTemplate.sockets[j];
                if (other.growthStep <= steps && footprint.overlaps(socketFootprint(roomTemplate, other, steps))) {
                    document.fail(pointerChild("/sockets", i), "the socket overlaps \"" + other.id + "\" after " +
                                                                   std::to_string(steps) + " growth step(s)");
                    break;
                }
            }
        }
    }
}

RoomTemplate readDocument(DocumentReader& document, const Json& value) {
    const ObjectReader reader(document, value, "",
                              {"circulationWidthMm", "collectionOrder", "decoration", "displayOrder", "doorway",
                               "eyeHeightMm", "growth", "heroBand", "id", "lightLimit", "modules", "room",
                               "schemaVersion", "shelfDressing", "sockets", "version"});
    RoomTemplate roomTemplate;
    (void)reader.integer("schemaVersion", 1, 1);
    roomTemplate.id = reader.dataId("id");
    roomTemplate.version = static_cast<std::uint32_t>(reader.integer("version", 1, kMaxVersion));

    const ObjectReader room = reader.object("room", {"depthMm", "heightMm", "wallThicknessMm", "widthMm"});
    roomTemplate.widthMm = room.integer("widthMm", 2'000, kMaxRoomMm);
    roomTemplate.depthMm = room.integer("depthMm", 2'000, kMaxRoomMm);
    roomTemplate.heightMm = room.integer("heightMm", 2'000, 10'000);
    roomTemplate.wallThicknessMm = room.integer("wallThicknessMm", 20, 2'000);

    const ObjectReader doorway =
        reader.object("doorway", {"clearDepthMm", "clearWidthMm", "heightMm", "offsetMm", "wall", "widthMm"});
    roomTemplate.doorway.wall = doorway.enumeration<Wall>("wall", parseWall, "one of north, south, east, west");
    roomTemplate.doorway.offsetMm = doorway.integer("offsetMm", -kMaxRoomMm, kMaxRoomMm);
    roomTemplate.doorway.widthMm = doorway.integer("widthMm", 600, kMaxRoomMm);
    roomTemplate.doorway.heightMm = doorway.integer("heightMm", 1'800, 10'000);
    roomTemplate.doorway.clearWidthMm = doorway.integer("clearWidthMm", 600, kMaxRoomMm);
    roomTemplate.doorway.clearDepthMm = doorway.integer("clearDepthMm", 600, kMaxRoomMm);

    roomTemplate.circulationWidthMm = reader.integer("circulationWidthMm", 0, kMaxRoomMm);
    roomTemplate.eyeHeightMm = reader.integer("eyeHeightMm", 500, 2'500);
    const ObjectReader growth = reader.object("growth", {"maxDepthMm", "stepMm"});
    roomTemplate.growthStepMm = growth.integer("stepMm", 0, kMaxRoomMm);
    roomTemplate.maxDepthMm = growth.integer("maxDepthMm", 2'000, kMaxRoomMm);
    roomTemplate.lightLimit = static_cast<std::uint32_t>(reader.integer("lightLimit", 1, kMaxSceneLights));
    if (!document.failed()) {
        const std::int64_t extra = roomTemplate.maxDepthMm - roomTemplate.depthMm;
        if (extra < 0) {
            document.fail("/growth/maxDepthMm", "smaller than the room's depth");
        } else if (roomTemplate.growthStepMm == 0 ? extra != 0 : extra % roomTemplate.growthStepMm != 0) {
            document.fail("/growth/maxDepthMm", "the room's depth plus a whole number of growth steps");
        }
    }

    const ObjectReader modules = reader.object("modules", {"hero", "side"});
    roomTemplate.heroModule = modules.prefab("hero");
    roomTemplate.sideModule = modules.prefab("side");

    std::map<std::string, const TemplateSocket*> byId;
    const Json& sockets = reader.array("sockets");
    roomTemplate.sockets.reserve(sockets.size());
    detail::forEachElement(sockets, "/sockets", [&](const Json& element, const std::string& pointer) {
        roomTemplate.sockets.push_back(readSocket(ObjectReader(
            document, element, pointer,
            {"depthMm", "growthStep", "id", "kind", "parts", "prefab", "wall", "widthMm", "xMm", "yawCdeg", "zMm"})));
    });
    std::size_t heroCorners = 0;
    for (std::size_t i = 0; i < roomTemplate.sockets.size() && !document.failed(); ++i) {
        const TemplateSocket& socket = roomTemplate.sockets[i];
        if (!byId.emplace(socket.id, &socket).second) {
            document.fail(pointerChild(pointerChild("/sockets", i), "id"), "the socket appears twice");
        }
        heroCorners += socket.kind == SocketKind::HeroCorner ? 1 : 0;
    }
    if (!document.failed() && heroCorners != 1) {
        document.fail("/sockets", "a template has exactly one hero-corner socket, not " + std::to_string(heroCorners));
    }

    roomTemplate.collectionOrder = readOrder(reader, "collectionOrder", byId,
                                             {SocketKind::HeroModule, SocketKind::SideWall}, true, "a collection one");
    for (std::size_t i = 1; i < roomTemplate.collectionOrder.size() && !document.failed(); ++i) {
        if (byId.at(roomTemplate.collectionOrder[i])->growthStep <
            byId.at(roomTemplate.collectionOrder[i - 1])->growthStep) {
            document.fail(pointerChild("/collectionOrder", i),
                          "a growth step's sockets come after those of the steps before it");
        }
    }
    roomTemplate.heroBand = readOrder(reader, "heroBand", byId, {SocketKind::HeroModule}, false, "a hero-module one");
    roomTemplate.displayOrder = readOrder(reader, "displayOrder", byId, {SocketKind::Display}, true, "a display one");

    std::set<std::string> names;
    detail::forEachElement(
        reader.array("decoration"), "/decoration", [&](const Json& element, const std::string& pointer) {
            const ObjectReader choice(document, element, pointer, {"mountHeightMm", "name", "prefab"});
            DecorationChoice decoration;
            decoration.name = choice.checkedString("name", isValidSocketName, "a decoration name of a-z, 0-9 and '-'");
            decoration.prefab = choice.prefab("prefab");
            decoration.mountHeightMm = choice.integer("mountHeightMm", 0, 10'000);
            if (!document.failed() && !names.insert(decoration.name).second) {
                document.fail(choice.pointer("name"), "the decoration appears twice");
            }
            roomTemplate.decoration.push_back(std::move(decoration));
        });
    if (!document.failed() && roomTemplate.decoration.empty()) {
        document.fail("/decoration", "must name at least one decoration");
    }
    std::set<PrefabRef> props;
    detail::forEachElement(
        reader.array("shelfDressing"), "/shelfDressing", [&](const Json& element, const std::string& pointer) {
            const ObjectReader prop(document, element, pointer, {"kind", "manifest"});
            PrefabRef prefab;
            prefab.manifest = prop.checkedString("manifest", assets::isValidPrefabId, "a prefab manifest id");
            prefab.kind = prop.checkedString("kind", isValidPrefabKind, "a prefab kind of a-z, 0-9 and '_'");
            if (!document.failed() && !props.insert(prefab).second) {
                document.fail(pointer, "the prop appears twice");
            }
            roomTemplate.shelfDressing.push_back(std::move(prefab));
        });
    if (!document.failed() && roomTemplate.shelfDressing.empty()) {
        document.fail("/shelfDressing", "must name at least one prop");
    }

    if (!document.failed()) {
        checkGeometry(document, roomTemplate);
    }
    return roomTemplate;
}

} // namespace

const TemplateSocket* RoomTemplate::socket(std::string_view socketId) const noexcept {
    for (const TemplateSocket& candidate : sockets) {
        if (candidate.id == socketId) {
            return &candidate;
        }
    }
    return nullptr;
}

std::uint32_t RoomTemplate::maxGrowthSteps() const noexcept {
    if (growthStepMm <= 0 || maxDepthMm <= depthMm) {
        return 0;
    }
    return static_cast<std::uint32_t>((maxDepthMm - depthMm) / growthStepMm);
}

Footprint interiorFootprint(const RoomTemplate& roomTemplate, std::uint32_t growthSteps) noexcept {
    return Footprint{-nearHalf(roomTemplate.widthMm), -nearHalf(roomTemplate.depthMm), farHalf(roomTemplate.widthMm),
                     farHalf(roomTemplate.depthMm) + southShift(roomTemplate, growthSteps)};
}

Footprint socketFootprint(const RoomTemplate& roomTemplate, const TemplateSocket& socket,
                          std::uint32_t growthSteps) noexcept {
    const std::int64_t shift = socket.wall == Wall::South ? southShift(roomTemplate, growthSteps) : 0;
    return rectangle(socket.xMm, socket.zMm + shift, socket.yawCdeg, socket.widthMm, socket.depthMm);
}

Footprint doorwayClearance(const RoomTemplate& roomTemplate, std::uint32_t growthSteps) noexcept {
    const Footprint interior = interiorFootprint(roomTemplate, growthSteps);
    const Doorway& door = roomTemplate.doorway;
    const std::int64_t from = door.offsetMm - nearHalf(door.clearWidthMm);
    const std::int64_t to = door.offsetMm + farHalf(door.clearWidthMm);
    switch (door.wall) {
    case Wall::North:
        return {from, interior.minZMm, to, interior.minZMm + door.clearDepthMm};
    case Wall::South:
        return {from, interior.maxZMm - door.clearDepthMm, to, interior.maxZMm};
    case Wall::West:
        return {interior.minXMm, from, interior.minXMm + door.clearDepthMm, to};
    case Wall::East:
        return {interior.maxXMm - door.clearDepthMm, from, interior.maxXMm, to};
    case Wall::None:
        break;
    }
    return {};
}

Expected<RoomTemplate> parseRoomTemplate(std::string_view text, std::string_view context) {
    auto value = detail::parseDataJson(text, context);
    if (!value) {
        return propagate(value.error());
    }
    DocumentReader document(text, std::string(context));
    RoomTemplate roomTemplate = readDocument(document, *value);
    if (document.failed()) {
        return propagate(document.error());
    }
    return roomTemplate;
}

Expected<RoomTemplate> loadRoomTemplate(const std::filesystem::path& assetRoot, std::string_view id) {
    if (!isValidDataId(id)) {
        return makeError(ErrorCode::InvalidArgument, "not a template id", std::string(id));
    }
    const std::filesystem::path path =
        assetRoot / "world" / "templates" / core::pathFromUtf8(std::string(id) + ".json");
    auto text = detail::readDataFile(path);
    if (!text) {
        return propagate(text.error());
    }
    const std::string context = core::pathToUtf8Lossy(path);
    auto roomTemplate = parseRoomTemplate(*text, context);
    if (roomTemplate && roomTemplate->id != id) {
        return makeError(ErrorCode::ValidationFailed,
                         "/id: the template's id \"" + roomTemplate->id + "\" is not its file's name \"" +
                             std::string(id) + "\"",
                         context);
    }
    return roomTemplate;
}

} // namespace rv::world
