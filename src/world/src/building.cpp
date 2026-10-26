#include <rv/world/building.hpp>

#include <algorithm>
#include <set>
#include <utility>

#include <rv/core/utf8.hpp>
#include <rv/world/platform_manifest.hpp>

#include "data_reader.hpp"

namespace rv::world {
namespace {

using detail::DocumentReader;
using detail::ObjectReader;
using Json = nlohmann::json;

constexpr std::int64_t kMaxLayoutMm = 100'000;
constexpr std::string_view kColourForm = "an sRGB colour #RRGGBB in uppercase hexadecimal";
constexpr std::string_view kKinds = "one of home, handheld, arcade, computer, other, excluded";

bool hasGenerations(SystemKind kind) noexcept {
    return kind == SystemKind::Home || kind == SystemKind::Handheld;
}

bool isSignText(std::string_view text) noexcept {
    if (text.empty() || text.size() > kMaxSignTextLength || text.front() == ' ' || text.back() == ' ') {
        return false;
    }
    return std::all_of(text.begin(), text.end(), [](char c) { return c >= 0x20 && c <= 0x7E; });
}

/// A platform tag as ES-DE's systems reader keeps them: lowercase, no blank or comma.
bool isPlatformTag(std::string_view text) noexcept {
    return !text.empty() && text.size() <= 64 && std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
    });
}

std::vector<std::string> readTags(const ObjectReader& reader, std::string_view key) {
    std::vector<std::string> tags;
    const Json& array = reader.array(key);
    detail::forEachElement(array, reader.pointer(key), [&](const Json& element, const std::string& pointer) {
        std::string tag = detail::readString(reader.document(), element, pointer);
        if (!reader.document().failed() && !isPlatformTag(tag)) {
            reader.document().fail(pointer, "expected a platform tag in lowercase, found \"" + tag + "\"");
        }
        tags.push_back(std::move(tag));
    });
    if (!reader.document().failed() && tags.empty()) {
        reader.document().fail(reader.pointer(key), "names no platform tag");
    }
    return tags;
}

KindDefaults readDefaults(const ObjectReader& reader) {
    KindDefaults defaults;
    defaults.themePack = reader.dataId("themePack");
    if (!reader.document().failed() && !reader.value("console").is_null()) {
        defaults.console = reader.prefab("console");
    }
    const ObjectReader plastic = reader.object("plastic", {"accent", "cartridge", "console"});
    defaults.cartridgePlastic = plastic.checkedString("cartridge", isSrgbHexColor, kColourForm);
    defaults.consolePlastic = plastic.checkedString("console", isSrgbHexColor, kColourForm);
    defaults.accentPlastic = plastic.checkedString("accent", isSrgbHexColor, kColourForm);
    if (!reader.document().failed() && reader.has("sign")) {
        defaults.sign = reader.checkedString("sign", isSignText,
                                             "1 to 32 printable ASCII characters without a space at either end");
    }
    return defaults;
}

SystemEntry readSystem(const ObjectReader& reader) {
    SystemEntry entry;
    entry.system = reader.parsed<catalog::SystemName>("system", catalog::SystemName::parse, "an ES-DE system name");
    entry.kind = reader.enumeration<SystemKind>("kind", parseSystemKind, kKinds);
    if (reader.document().failed()) {
        return entry;
    }
    if (entry.kind == SystemKind::Excluded) {
        for (const std::string_view key : {"family", "generation", "order", "sign"}) {
            if (reader.has(key)) {
                reader.document().fail(reader.pointer(key), "an excluded system names its kind and platforms only");
                return entry;
            }
        }
        entry.platforms = readTags(reader, "platforms");
        return entry;
    }
    if (hasGenerations(entry.kind)) {
        entry.generation = static_cast<std::uint32_t>(reader.integer("generation", 1, kMaxGeneration));
    } else if (reader.has("generation")) {
        reader.document().fail(reader.pointer("generation"), "only a home console or a handheld has a generation");
        return entry;
    }
    entry.family = reader.dataId("family");
    entry.order = static_cast<std::uint32_t>(reader.integer("order", 0, 999'999));
    entry.sign = reader.checkedString("sign", isSignText,
                                      "1 to 32 printable ASCII characters without a space at either end");
    entry.platforms = readTags(reader, "platforms");
    return entry;
}

} // namespace

// --- building.json --------------------------------------------------------------------------------------------------

const FloorSpec* BuildingData::floor(std::string_view id) const noexcept {
    for (const FloorSpec& spec : floors) {
        if (spec.id == id) {
            return &spec;
        }
    }
    return nullptr;
}

std::optional<std::uint32_t> BuildingData::order(std::string_view id) const noexcept {
    for (std::size_t i = 0; i < floors.size(); ++i) {
        if (floors[i].id == id) {
            return static_cast<std::uint32_t>(i);
        }
    }
    return std::nullopt;
}

const FloorSpec* BuildingData::floorFor(SystemKind kind, std::uint32_t generation) const noexcept {
    if (kind == SystemKind::Excluded) {
        return nullptr;
    }
    for (const FloorSpec& spec : floors) {
        if (std::find(spec.kinds.begin(), spec.kinds.end(), kind) == spec.kinds.end()) {
            continue;
        }
        // A floor's generations hold only for the kinds that have generations, as the reader counts them.
        if (spec.generations.empty() || !hasGenerations(kind) ||
            std::find(spec.generations.begin(), spec.generations.end(), generation) != spec.generations.end()) {
            return &spec;
        }
    }
    return nullptr;
}

std::optional<Floor> BuildingData::planFloor(std::string_view id) const {
    const auto index = order(id);
    if (!index) {
        return std::nullopt;
    }
    return Floor{std::string(id), *index};
}

Expected<BuildingData> parseBuildingData(std::string_view text, std::string_view context) {
    auto value = detail::parseDataJson(text, context);
    if (!value) {
        return propagate(value.error());
    }
    DocumentReader document(text, std::string(context));
    const ObjectReader reader(document, *value, "", {"floors", "layout", "schemaVersion"});
    BuildingData building;
    (void)reader.integer("schemaVersion", 1, 1);
    const Json& floors = reader.array("floors");
    detail::forEachElement(floors, "/floors", [&](const Json& element, const std::string& pointer) {
        const ObjectReader floor(document, element, pointer, {"generations", "id", "kinds", "sign", "themePack"});
        FloorSpec spec;
        spec.id = floor.dataId("id");
        if (!document.failed() && floor.has("themePack")) {
            spec.themePack = floor.dataId("themePack");
        }
        if (!document.failed() && floor.has("sign")) {
            spec.sign = floor.checkedString("sign", isSignText,
                                            "1 to 32 printable ASCII characters without a space at either end");
        } else if (!document.failed()) {
            // The id in uppercase (a data id: lowercase letters, digits, '-' and '_'), within a sign's length.
            spec.sign = spec.id.substr(0, kMaxSignTextLength);
            std::transform(spec.sign.begin(), spec.sign.end(), spec.sign.begin(),
                           [](char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; });
        }
        const Json& kinds = floor.array("kinds");
        detail::forEachElement(kinds, floor.pointer("kinds"), [&](const Json& kind, const std::string& kindPointer) {
            const std::string kindText = detail::readString(document, kind, kindPointer);
            if (document.failed()) {
                return;
            }
            const auto parsed = parseSystemKind(kindText);
            if (!parsed || *parsed == SystemKind::Excluded) {
                document.fail(kindPointer, "expected one of home, handheld, arcade, computer, other, found \"" +
                                               kindText + "\"");
                return;
            }
            if (std::find(spec.kinds.begin(), spec.kinds.end(), *parsed) != spec.kinds.end()) {
                document.fail(kindPointer, "appears twice");
                return;
            }
            spec.kinds.push_back(*parsed);
        });
        if (!document.failed() && spec.kinds.empty()) {
            document.fail(floor.pointer("kinds"), "names no kind");
        }
        if (floor.has("generations")) {
            const Json& generations = floor.array("generations");
            detail::forEachElement(generations, floor.pointer("generations"),
                                   [&](const Json& generation, const std::string& generationPointer) {
                                       spec.generations.push_back(static_cast<std::uint32_t>(detail::readInteger(
                                           document, generation, generationPointer, 1, kMaxGeneration)));
                                   });
            if (!document.failed() && spec.generations.empty()) {
                document.fail(floor.pointer("generations"), "names no generation");
            }
            if (!document.failed() && std::none_of(spec.kinds.begin(), spec.kinds.end(), hasGenerations)) {
                document.fail(floor.pointer("generations"), "the floor holds no kind with generations");
            }
        }
        building.floors.push_back(std::move(spec));
    });
    const ObjectReader layout = reader.object(
        "layout", {"hallWidthMm", "hub", "lobbyWidthMm", "maxRoomsPerRow", "rowStartXMm", "wallAllowanceMm"});
    building.layout.hallWidthMm = layout.integer("hallWidthMm", 2, kMaxLayoutMm);
    building.layout.lobbyWidthMm = layout.integer("lobbyWidthMm", 2, kMaxLayoutMm);
    building.layout.rowStartXMm = layout.integer("rowStartXMm", 0, kMaxLayoutMm);
    building.layout.wallAllowanceMm = layout.integer("wallAllowanceMm", 0, 10'000);
    building.layout.maxRoomsPerRow = static_cast<std::uint32_t>(layout.integer("maxRoomsPerRow", 1, 999));
    if (!document.failed() && building.layout.hallWidthMm % 2 != 0) {
        document.fail("/layout/hallWidthMm", "must be even");
    }
    if (!document.failed() && building.layout.lobbyWidthMm % 2 != 0) {
        document.fail("/layout/lobbyWidthMm", "must be even");
    }
    // The hub (I6 step 5): optional, HubLayout's defaults without it.
    if (!document.failed() && layout.has("hub")) {
        const ObjectReader hub = layout.object("hub", {"heightMm", "lengthMm", "rowStartXMm", "widthMm"});
        HubLayout& read = building.layout.hub;
        read.lengthMm = hub.integer("lengthMm", 12'000, 60'000);
        read.widthMm = hub.integer("widthMm", 8'000, 30'000);
        read.heightMm = hub.integer("heightMm", 4'000, 8'000);
        read.rowStartXMm = hub.integer("rowStartXMm", 0, kMaxLayoutMm);
        if (!document.failed() && read.widthMm % 2 != 0) {
            document.fail("/layout/hub/widthMm", "must be even");
        }
    }
    // Floors: ids once each, the ground floor first, every kind and generation on exactly one floor.
    std::set<std::string> ids;
    for (std::size_t i = 0; i < building.floors.size() && !document.failed(); ++i) {
        if (!ids.insert(building.floors[i].id).second) {
            document.fail(detail::pointerChild(detail::pointerChild("/floors", i), "id"), "appears twice");
        }
    }
    if (!document.failed() && (building.floors.empty() || building.floors.front().id != kGroundFloor)) {
        document.fail("/floors", "the first floor is the ground floor, \"" + std::string(kGroundFloor) + "\"");
    }
    for (const SystemKind kind :
         {SystemKind::Home, SystemKind::Handheld, SystemKind::Arcade, SystemKind::Computer, SystemKind::Other}) {
        for (std::uint32_t generation = 1; generation <= kMaxGeneration && !document.failed(); ++generation) {
            std::size_t holders = 0;
            for (const FloorSpec& spec : building.floors) {
                const bool holdsKind = std::find(spec.kinds.begin(), spec.kinds.end(), kind) != spec.kinds.end();
                const bool holdsGeneration =
                    spec.generations.empty() || !hasGenerations(kind) ||
                    std::find(spec.generations.begin(), spec.generations.end(), generation) != spec.generations.end();
                holders += holdsKind && holdsGeneration ? 1 : 0;
            }
            if (holders != 1) {
                document.fail("/floors", std::string(holders == 0 ? "no floor holds " : "several floors hold ") +
                                             std::string(toString(kind)) +
                                             (hasGenerations(kind) ? " of generation " + std::to_string(generation)
                                                                   : std::string()));
            }
            if (!hasGenerations(kind)) {
                break;
            }
        }
    }
    if (document.failed()) {
        return propagate(document.error());
    }
    return building;
}

Expected<BuildingData> loadBuildingData(const std::filesystem::path& assetRoot) {
    const std::filesystem::path path = assetRoot / "world" / "building.json";
    auto text = detail::readDataFile(path);
    if (!text) {
        return propagate(text.error());
    }
    return parseBuildingData(*text, core::pathToUtf8Lossy(path));
}

nlohmann::json hubLayoutToJson(const HubLayout& hub) {
    return Json{{"heightMm", hub.heightMm},
                {"lengthMm", hub.lengthMm},
                {"rowStartXMm", hub.rowStartXMm},
                {"widthMm", hub.widthMm}};
}

nlohmann::json buildingDataToJson(const BuildingData& building) {
    Json floors = Json::array();
    for (const FloorSpec& spec : building.floors) {
        Json kinds = Json::array();
        for (const SystemKind kind : spec.kinds) {
            kinds.push_back(toString(kind));
        }
        floors.push_back(Json{{"id", spec.id}, {"kinds", std::move(kinds)}, {"generations", spec.generations}});
    }
    const BuildingLayout& layout = building.layout;
    return Json{{"floors", std::move(floors)},
                {"layout", Json{{"hallWidthMm", layout.hallWidthMm},
                                {"lobbyWidthMm", layout.lobbyWidthMm},
                                {"rowStartXMm", layout.rowStartXMm},
                                {"wallAllowanceMm", layout.wallAllowanceMm},
                                {"maxRoomsPerRow", layout.maxRoomsPerRow}}}};
}

// --- The look of a room ---------------------------------------------------------------------------------------------

std::string themePackOf(RoomKind kind, std::string_view floor, SystemKind defaultsKind, const SystemsData& systems,
                        const BuildingData& building) {
    if (kind == RoomKind::Arcade) {
        return systems.arcade.themePack;
    }
    if (const FloorSpec* spec = building.floor(floor); spec != nullptr && !spec->themePack.empty()) {
        return spec->themePack;
    }
    return systems.defaultsOf(defaultsKind).themePack;
}

std::string floorThemePack(std::string_view floor, const SystemsData& systems, const BuildingData& building) {
    const FloorSpec* spec = building.floor(floor);
    if (spec != nullptr && !spec->themePack.empty()) {
        return spec->themePack;
    }
    return systems.defaultsOf(spec != nullptr && !spec->kinds.empty() ? spec->kinds.front() : SystemKind::Other)
        .themePack;
}

// --- systems.json ---------------------------------------------------------------------------------------------------

const SystemEntry* SystemsData::find(const catalog::SystemName& system) const noexcept {
    for (const SystemEntry& entry : systems) {
        if (entry.system == system) {
            return &entry;
        }
    }
    return nullptr;
}

const KindDefaults& SystemsData::defaultsOf(SystemKind kind) const noexcept {
    switch (kind) {
    case SystemKind::Home:
        return home;
    case SystemKind::Handheld:
        return handheld;
    case SystemKind::Arcade:
        return arcade;
    case SystemKind::Computer:
        return computer;
    case SystemKind::Other:
    case SystemKind::Excluded:
        break;
    }
    return other;
}

Expected<SystemsData> parseSystemsData(std::string_view text, std::string_view context) {
    auto value = detail::parseDataJson(text, context);
    if (!value) {
        return propagate(value.error());
    }
    DocumentReader document(text, std::string(context));
    const ObjectReader reader(document, *value, "", {"defaults", "fallback", "schemaVersion", "systems"});
    SystemsData data;
    (void)reader.integer("schemaVersion", 1, 1);
    const ObjectReader defaults = reader.object("defaults", {"arcade", "computer", "handheld", "home", "other"});
    const std::initializer_list<std::string_view> defaultKeys{"console", "plastic", "sign", "themePack"};
    data.home = readDefaults(defaults.object("home", defaultKeys));
    data.handheld = readDefaults(defaults.object("handheld", defaultKeys));
    data.arcade = readDefaults(defaults.object("arcade", defaultKeys));
    data.computer = readDefaults(defaults.object("computer", defaultKeys));
    data.other = readDefaults(defaults.object("other", defaultKeys));
    const ObjectReader fallback = reader.object("fallback", {"family", "order", "sign"});
    data.fallback.kind = SystemKind::Other;
    data.fallback.family = fallback.dataId("family");
    data.fallback.order = static_cast<std::uint32_t>(fallback.integer("order", 0, 999'999));
    data.fallback.sign = fallback.checkedString("sign", isSignText,
                                                "1 to 32 printable ASCII characters without a space at either end");
    const Json& systems = reader.array("systems");
    detail::forEachElement(systems, "/systems", [&](const Json& element, const std::string& pointer) {
        data.systems.push_back(readSystem(ObjectReader(
            document, element, pointer, {"family", "generation", "kind", "order", "platforms", "sign", "system"})));
    });
    std::set<catalog::SystemName> seen;
    for (std::size_t i = 0; i < data.systems.size() && !document.failed(); ++i) {
        if (!seen.insert(data.systems[i].system).second) {
            document.fail(detail::pointerChild(detail::pointerChild("/systems", i), "system"), "appears twice");
        }
    }
    if (document.failed()) {
        return propagate(document.error());
    }
    return data;
}

Expected<SystemsData> loadSystemsData(const std::filesystem::path& assetRoot) {
    const std::filesystem::path path = assetRoot / "world" / "systems.json";
    auto text = detail::readDataFile(path);
    if (!text) {
        return propagate(text.error());
    }
    return parseSystemsData(*text, core::pathToUtf8Lossy(path));
}

nlohmann::json systemEntryToJson(const SystemEntry& entry) {
    return Json{{"system", entry.system.str()},  {"kind", toString(entry.kind)}, {"generation", entry.generation},
                {"family", entry.family},        {"order", entry.order},         {"sign", entry.sign},
                {"platforms", entry.platforms}};
}

Classification classify(const catalog::SystemName& system, std::span<const std::string> platformTags,
                        const SystemsData& systems, const BuildingData& building) {
    Classification result;
    if (const SystemEntry* own = systems.find(system)) {
        result.entry = *own;
        result.source = ClassificationSource::Table;
    } else {
        const SystemEntry* matched = nullptr;
        for (const SystemEntry& entry : systems.systems) {
            const bool shares = std::any_of(platformTags.begin(), platformTags.end(), [&](const std::string& tag) {
                return std::find(entry.platforms.begin(), entry.platforms.end(), tag) != entry.platforms.end();
            });
            if (shares) {
                matched = &entry;
                break;
            }
        }
        if (matched != nullptr) {
            result.entry = *matched;
            result.source = ClassificationSource::PlatformTag;
            result.matched = matched->system;
        } else {
            result.entry = systems.fallback;
            result.entry.kind = SystemKind::Other;
            result.source = ClassificationSource::Fallback;
        }
        result.entry.system = system;
        result.entry.platforms.assign(platformTags.begin(), platformTags.end());
    }
    if (const FloorSpec* floor = building.floorFor(result.entry.kind, result.entry.generation)) {
        result.floor = floor->id;
    }
    return result;
}

// --- The floor's layout ---------------------------------------------------------------------------------------------

std::int64_t hallWidthOf(std::string_view floor, const BuildingLayout& layout) noexcept {
    return floor == kGroundFloor ? layout.hub.widthMm : layout.hallWidthMm;
}

std::int64_t rowStartOf(std::string_view floor, const BuildingLayout& layout) noexcept {
    return floor == kGroundFloor ? layout.hub.rowStartXMm : layout.rowStartXMm;
}

FloorPoint roomToFloor(const RoomPlacement& placement, std::int64_t xMm, std::int64_t zMm) noexcept {
    const std::int64_t sign = placement.yawHalfTurns % 2 == 0 ? 1 : -1;
    return {placement.xMm + sign * xMm, placement.zMm + sign * zMm};
}

Footprint roomToFloor(const RoomPlacement& placement, const Footprint& footprint) noexcept {
    const FloorPoint a = roomToFloor(placement, footprint.minXMm, footprint.minZMm);
    const FloorPoint b = roomToFloor(placement, footprint.maxXMm, footprint.maxZMm);
    return {std::min(a.xMm, b.xMm), std::min(a.zMm, b.zMm), std::max(a.xMm, b.xMm), std::max(a.zMm, b.zMm)};
}

std::int32_t roomYawToFloor(const RoomPlacement& placement, std::int32_t yawCdeg) noexcept {
    const std::int32_t turned = yawCdeg + static_cast<std::int32_t>(placement.yawHalfTurns % 2) * 18'000;
    return ((turned % 36'000) + 36'000) % 36'000;
}

Footprint roomFootprint(const Room& room, const BuildingLayout& layout) noexcept {
    const std::int64_t wall = layout.wallAllowanceMm;
    return {room.minXMm - wall, room.minZMm - wall, room.maxXMm + wall, room.maxZMm + wall};
}

Footprint floorFootprint(const Room& room, const BuildingLayout& layout) noexcept {
    return roomToFloor(room.placement, roomFootprint(room, layout));
}

std::int64_t abuttingZ(const Room& room, Row row, std::string_view floor, const BuildingLayout& layout) noexcept {
    const std::int64_t halfHall = hallWidthOf(floor, layout) / 2;
    const std::int64_t doorwayWall = room.maxZMm + layout.wallAllowanceMm;
    // North row: z + doorwayWall = -halfHall. South row, half a turn: z - doorwayWall = +halfHall.
    return row == Row::North ? -halfHall - doorwayWall : halfHall + doorwayWall;
}

RoomPlacement followDepth(const Room& room, const BuildingLayout& layout) noexcept {
    RoomPlacement placement = room.placement;
    placement.zMm = abuttingZ(room, rowOf(placement), room.floor, layout);
    return placement;
}

FloorRows floorRows(std::span<const Room> rooms, std::string_view floor, const BuildingLayout& layout) {
    FloorRows rows;
    rows.northEndMm = rowStartOf(floor, layout);
    rows.southEndMm = rows.northEndMm;
    for (const Room& room : rooms) {
        if (room.floor != floor) {
            continue;
        }
        const Footprint footprint = floorFootprint(room, layout);
        if (rowOf(room.placement) == Row::North) {
            rows.northEndMm = std::max(rows.northEndMm, footprint.maxXMm);
            ++rows.northRooms;
        } else {
            rows.southEndMm = std::max(rows.southEndMm, footprint.maxXMm);
            ++rows.southRooms;
        }
    }
    return rows;
}

std::optional<RoomPlacement> placeEnteringRoom(const Room& room, const FloorRows& rows,
                                               const BuildingLayout& layout) {
    const bool northOpen = rows.northRooms < layout.maxRoomsPerRow;
    const bool southOpen = rows.southRooms < layout.maxRoomsPerRow;
    if (!northOpen && !southOpen) {
        return std::nullopt;
    }
    const Row row = !southOpen || (northOpen && rows.northEndMm <= rows.southEndMm) ? Row::North : Row::South;
    const Footprint own = roomFootprint(room, layout);
    RoomPlacement placement;
    placement.yawHalfTurns = row == Row::North ? 0 : 1;
    // The footprint's west edge on the row's east end: x + minX (north), x - maxX (south, turned).
    placement.xMm = row == Row::North ? rows.northEndMm - own.minXMm : rows.southEndMm + own.maxXMm;
    placement.zMm = abuttingZ(room, row, room.floor, layout);
    return placement;
}

bool standsOnHub(const Room& room, const BuildingLayout& layout) noexcept {
    if (room.floor != kGroundFloor) {
        return true;
    }
    return room.placement.zMm == abuttingZ(room, rowOf(room.placement), room.floor, layout) &&
           floorFootprint(room, layout).minXMm >= layout.hub.rowStartXMm;
}

std::vector<ReplacedRoom> placeOnHub(std::vector<Room>& rooms, const BuildingLayout& layout) {
    std::vector<std::size_t> moving;
    std::vector<Room> standing;
    for (std::size_t i = 0; i < rooms.size(); ++i) {
        if (rooms[i].floor != kGroundFloor) {
            continue;
        }
        if (standsOnHub(rooms[i], layout)) {
            standing.push_back(rooms[i]);
        } else {
            moving.push_back(i);
        }
    }
    // West to east by where they stood, the north row first on a tie, then by id.
    std::sort(moving.begin(), moving.end(), [&](std::size_t a, std::size_t b) {
        const Footprint fa = floorFootprint(rooms[a], layout);
        const Footprint fb = floorFootprint(rooms[b], layout);
        if (fa.minXMm != fb.minXMm) {
            return fa.minXMm < fb.minXMm;
        }
        if (rowOf(rooms[a].placement) != rowOf(rooms[b].placement)) {
            return rowOf(rooms[a].placement) == Row::North;
        }
        return rooms[a].id < rooms[b].id;
    });
    std::vector<ReplacedRoom> replaced;
    for (const std::size_t index : moving) {
        Room& room = rooms[index];
        const FloorRows rows = floorRows(standing, kGroundFloor, layout);
        const Row row = rowOf(room.placement);
        const bool open = (row == Row::North ? rows.northRooms : rows.southRooms) < layout.maxRoomsPerRow;
        std::optional<RoomPlacement> placement;
        if (open) {
            // At the east end of its own row: x + minX (north), x - maxX (south, turned) on that end.
            const Footprint own = roomFootprint(room, layout);
            RoomPlacement kept = room.placement;
            kept.xMm = row == Row::North ? rows.northEndMm - own.minXMm : rows.southEndMm + own.maxXMm;
            kept.zMm = abuttingZ(room, row, kGroundFloor, layout);
            placement = kept;
        } else {
            placement = placeEnteringRoom(room, rows, layout);
        }
        if (!placement) {
            continue; // a hub whose rows are full: the room stays where it stood (validation tells)
        }
        replaced.push_back({room.id, room.placement, *placement});
        room.placement = *placement;
        standing.push_back(room);
    }
    std::sort(replaced.begin(), replaced.end());
    return replaced;
}

} // namespace rv::world
