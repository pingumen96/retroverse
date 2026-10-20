#include <rv/world/plan_json.hpp>

#include <algorithm>
#include <initializer_list>
#include <set>
#include <tuple>
#include <utility>

#include <rv/core/canonical_json.hpp>
#include <rv/core/sha256.hpp>

#include "data_reader.hpp"
#include "plan_parts.hpp"

namespace rv::world {
namespace {

using detail::DocumentReader;
using detail::ObjectReader;
using Json = nlohmann::json;

/// Coordinates within 1 km of a room's origin, sizes up to 100 m: far beyond any room, and well inside int64 sums.
constexpr std::int64_t kMaxCoordinateMm = 1'000'000;
constexpr std::int64_t kMaxSizeMm = 100'000;
constexpr std::int64_t kMaxVersion = 999'999;
/// A placement's own yaw is a small offset; anything up to half a turn is a readable plan.
constexpr std::int64_t kMaxPlacementYawCdeg = 18'000;
constexpr std::int64_t kMaxInsetMm = 1'000;
constexpr std::int64_t kMaxGrowthSteps = 1'000;
constexpr std::int64_t kMaxFloorOrder = 999;

Json prefabJson(const PrefabRef& prefab) {
    return Json{{"kind", prefab.kind}, {"manifest", prefab.manifest}};
}

Json roomJson(const Room& room) {
    Json sections = Json::array();
    for (const catalog::SystemName& system : room.sections) {
        sections.push_back(system.str());
    }
    Json json{{"id", room.id.toString()},
              {"kind", toString(room.kind)},
              {"floor", room.floor},
              {"sections", std::move(sections)},
              {"placement", Json{{"xMm", room.placement.xMm},
                                 {"zMm", room.placement.zMm},
                                 {"yawHalfTurns", room.placement.yawHalfTurns}}},
              {"generatorVersion", room.generatorVersion.toString()},
              {"template", room.templateId},
              {"templateVersion", room.templateVersion},
              {"minXMm", room.minXMm},
              {"maxXMm", room.maxXMm},
              {"minZMm", room.minZMm},
              {"maxZMm", room.maxZMm},
              {"heightMm", room.heightMm},
              {"growthSteps", room.growthSteps},
              {"seed", room.seed},
              {"mediaFamily", room.mediaFamily},
              {"themePack", room.themePack}};
    if (!room.expands.empty()) {
        json["expands"] = room.expands.toString();
    }
    return json;
}

Json floorJson(const Floor& floor) {
    return Json{{"id", floor.id}, {"order", floor.order}};
}

Json fixtureJson(const Fixture& fixture) {
    Json levels = Json::array();
    for (const FixtureLevel& level : fixture.levels) {
        levels.push_back(Json{{"level", level.level}, {"slots", level.slots}, {"section", toString(level.section)}});
    }
    return Json{{"id", fixture.id.toString()},
                {"role", toString(fixture.role)},
                {"prefab", prefabJson(fixture.prefab)},
                {"xMm", fixture.xMm},
                {"yMm", fixture.yMm},
                {"zMm", fixture.zMm},
                {"yawCdeg", fixture.yawCdeg},
                {"widthMm", fixture.widthMm},
                {"heightMm", fixture.heightMm},
                {"depthMm", fixture.depthMm},
                {"levels", std::move(levels)}};
}

Json placementJson(const Placement& placement) {
    return Json{{"slot", placement.slot.toString()},
                {"game", placement.game.toString()},
                {"insetMm", placement.insetMm},
                {"yawCdeg", placement.yawCdeg}};
}

Json dressingJson(const DressedLevel& dressed) {
    return Json{{"level", dressed.level.toString()},
                {"firstSlot", dressed.firstSlot},
                {"count", dressed.count},
                {"prop", prefabJson(dressed.prop)}};
}

Json lightJson(const PlanLight& light) {
    return Json{{"id", light.id.toString()},
                {"role", toString(light.role)},
                {"xMm", light.xMm},
                {"yMm", light.yMm},
                {"zMm", light.zMm},
                {"targetXMm", light.targetXMm},
                {"targetYMm", light.targetYMm},
                {"targetZMm", light.targetZMm},
                {"spanMm", light.spanMm}};
}

UnplacedGame readUnplaced(const ObjectReader& reader) {
    UnplacedGame unplaced;
    unplaced.game = reader.parsed<catalog::GameId>("game", catalog::GameId::parse, "a GameId");
    unplaced.room = reader.parsed<RoomId>("room", RoomId::parse, "a room id (room:<system>, area:...)");
    unplaced.reason = reader.enumeration<UnplacedReason>(
        "reason", parseUnplacedReason, "one of duplicate-title, no-cover, unavailable, capacity, room-kept");
    if (reader.document().failed()) {
        return unplaced;
    }
    if (unplaced.reason == UnplacedReason::DuplicateTitle) {
        unplaced.representative = reader.parsed<catalog::GameId>("representative", catalog::GameId::parse, "a GameId");
    } else if (reader.has("representative")) {
        reader.document().fail(reader.pointer("representative"), "only a duplicate-title entry names a representative");
    }
    return unplaced;
}

} // namespace

namespace detail {

Json unplacedJson(const UnplacedGame& unplaced) {
    Json json{
        {"game", unplaced.game.toString()}, {"room", unplaced.room.toString()}, {"reason", toString(unplaced.reason)}};
    if (unplaced.reason == UnplacedReason::DuplicateTitle || unplaced.representative.isValid()) {
        // Written whenever set, so that a representative on another reason is refused by the reader, not dropped.
        json["representative"] = unplaced.representative.toString();
    }
    return json;
}

std::vector<UnplacedGame> readUnplacedList(const ObjectReader& reader, std::string_view key) {
    std::vector<UnplacedGame> games;
    const Json& array = reader.array(key);
    games.reserve(array.size());
    forEachElement(array, reader.pointer(key), [&](const Json& element, const std::string& pointer) {
        games.push_back(readUnplaced(
            ObjectReader(reader.document(), element, pointer, {"game", "reason", "representative", "room"})));
    });
    requireStrictOrder(
        reader.document(), games, reader.pointer(key), [](const UnplacedGame& game) { return game; },
        "unplaced games are sorted by game, then room, reason and representative");
    return games;
}

} // namespace detail

namespace {

Json documentJson(const WorldPlan& plan) {
    Json floors = Json::array();
    for (const Floor& floor : plan.floors) {
        floors.push_back(floorJson(floor));
    }
    Json rooms = Json::array();
    for (const Room& room : plan.rooms) {
        rooms.push_back(roomJson(room));
    }
    Json fixtures = Json::array();
    for (const Fixture& fixture : plan.fixtures) {
        fixtures.push_back(fixtureJson(fixture));
    }
    Json placements = Json::array();
    for (const Placement& placement : plan.placements) {
        placements.push_back(placementJson(placement));
    }
    Json dressing = Json::array();
    for (const DressedLevel& dressed : plan.dressing) {
        dressing.push_back(dressingJson(dressed));
    }
    Json lights = Json::array();
    for (const PlanLight& light : plan.lights) {
        lights.push_back(lightJson(light));
    }
    Json unplaced = Json::array();
    for (const UnplacedGame& game : plan.unplaced) {
        unplaced.push_back(detail::unplacedJson(game));
    }
    return Json{{"schemaVersion", plan.schemaVersion},
                {"generatorVersion", plan.generatorVersion.toString()},
                {"randomAlgorithm", plan.randomAlgorithm},
                {"titleKeyRule", plan.titleKeyRule},
                {"catalogSortKeyVersion", plan.catalogSortKeyVersion},
                {"library", plan.library.str()},
                {"seed", plan.seed},
                {"preset", plan.preset},
                {"inputDigest", plan.inputDigest},
                {"floors", std::move(floors)},
                {"rooms", std::move(rooms)},
                {"fixtures", std::move(fixtures)},
                {"placements", std::move(placements)},
                {"dressing", std::move(dressing)},
                {"lights", std::move(lights)},
                {"unplaced", std::move(unplaced)}};
}

/// The keys of a room of each schema.
const std::initializer_list<std::string_view> kRoomKeysV1{"growthSteps", "heightMm", "id", "maxXMm", "maxZMm",
                                                          "mediaFamily", "minXMm", "minZMm", "seed", "template",
                                                          "templateVersion", "themePack"};
const std::initializer_list<std::string_view> kRoomKeysV2{
    "expands", "floor",    "generatorVersion", "growthSteps", "heightMm",    "id",       "kind",
    "maxXMm",  "maxZMm",   "mediaFamily",      "minXMm",      "minZMm",      "placement", "sections",
    "seed",    "template", "templateVersion",  "themePack"};

/// Schema 2's own fields of a room, and their agreement with its id (the kind of the id's form, a console room's one
/// section its system, a shared room's floor, an expansion's head).
void readBuildingFields(const ObjectReader& reader, Room& room) {
    DocumentReader& document = reader.document();
    room.kind = reader.enumeration<RoomKind>("kind", parseRoomKind, "one of console, shared, arcade");
    room.floor = reader.dataId("floor");
    const Json& sections = reader.array("sections");
    detail::forEachElement(sections, reader.pointer("sections"), [&](const Json& element, const std::string& pointer) {
        const std::string text = detail::readString(document, element, pointer);
        if (document.failed()) {
            return;
        }
        auto system = catalog::SystemName::parse(text);
        if (!system) {
            document.fail(pointer, "expected an ES-DE system name, found \"" + text + "\"");
            return;
        }
        if (std::find(room.sections.begin(), room.sections.end(), *system) != room.sections.end()) {
            document.fail(pointer, "appears twice");
            return;
        }
        room.sections.push_back(std::move(*system));
    });
    if (!document.failed() && room.sections.empty()) {
        document.fail(reader.pointer("sections"), "names no system");
    }
    const ObjectReader placement = reader.object("placement", {"xMm", "yawHalfTurns", "zMm"});
    room.placement.xMm = placement.integer("xMm", -kMaxCoordinateMm, kMaxCoordinateMm);
    room.placement.zMm = placement.integer("zMm", -kMaxCoordinateMm, kMaxCoordinateMm);
    room.placement.yawHalfTurns = static_cast<std::uint32_t>(placement.integer("yawHalfTurns", 0, 1));
    room.generatorVersion =
        reader.enumeration<GeneratorVersion>("generatorVersion", GeneratorVersion::parse, "a version <major>.<minor>");
    if (reader.has("expands")) {
        room.expands = reader.parsed<RoomId>("expands", RoomId::parse, "a room id");
    }
    if (document.failed()) {
        return;
    }
    const RoomIdKind form = room.kind == RoomKind::Console  ? RoomIdKind::System
                            : room.kind == RoomKind::Shared ? RoomIdKind::Shared
                                                            : RoomIdKind::Arcade;
    if (room.id.kind() != form) {
        document.fail(reader.pointer("kind"),
                      "a " + std::string(toString(room.kind)) + " room's id is not " + room.id.toString());
    } else if (room.kind == RoomKind::Console &&
               (room.sections.size() != 1 || room.sections.front() != room.id.system())) {
        document.fail(reader.pointer("sections"), "a console room holds exactly its own system");
    } else if (room.kind == RoomKind::Shared && room.id.floor() != room.floor) {
        document.fail(reader.pointer("floor"), "a shared room stands on the floor its id names");
    } else if (room.id.isExpansion() != !room.expands.empty() ||
               (!room.expands.empty() && room.expands != room.id.head())) {
        document.fail(reader.pointer("expands"), "an expansion, and only one, names the head of its chain");
    }
}

Room readRoom(const ObjectReader& reader, std::int64_t schema) {
    Room room;
    room.id = reader.parsed<RoomId>("id", RoomId::parse, "a room id (room:<system>, area:...)");
    if (schema == 1 && !reader.document().failed() &&
        (room.id.kind() != RoomIdKind::System || room.id.isExpansion())) {
        reader.document().fail(reader.pointer("id"), "a schema-1 room is a system's room, room:<system>");
    }
    room.templateId = reader.dataId("template");
    room.templateVersion = static_cast<std::uint32_t>(reader.integer("templateVersion", 1, kMaxVersion));
    room.minXMm = reader.integer("minXMm", -kMaxCoordinateMm, kMaxCoordinateMm);
    room.maxXMm = reader.integer("maxXMm", -kMaxCoordinateMm, kMaxCoordinateMm);
    room.minZMm = reader.integer("minZMm", -kMaxCoordinateMm, kMaxCoordinateMm);
    room.maxZMm = reader.integer("maxZMm", -kMaxCoordinateMm, kMaxCoordinateMm);
    room.heightMm = reader.integer("heightMm", 1, kMaxSizeMm);
    room.growthSteps = static_cast<std::uint32_t>(reader.integer("growthSteps", 0, kMaxGrowthSteps));
    room.seed = reader.unsignedInteger("seed");
    room.mediaFamily = reader.dataId("mediaFamily");
    room.themePack = reader.dataId("themePack");
    if (!reader.document().failed() && room.minXMm >= room.maxXMm) {
        reader.document().fail(reader.pointer("maxXMm"), "must be greater than minXMm");
    }
    if (!reader.document().failed() && room.minZMm >= room.maxZMm) {
        reader.document().fail(reader.pointer("maxZMm"), "must be greater than minZMm");
    }
    if (schema >= 2 && !reader.document().failed()) {
        readBuildingFields(reader, room);
    }
    return room;
}

FixtureLevel readLevel(const ObjectReader& reader) {
    FixtureLevel level;
    level.level = static_cast<std::uint32_t>(reader.integer("level", 0, kMaxLevel));
    level.slots = static_cast<std::uint32_t>(reader.integer("slots", 1, std::int64_t{kMaxSlotIndex} + 1));
    level.section = reader.enumeration<Section>("section", parseSection, "one of featured, band, spine");
    return level;
}

Fixture readFixture(const ObjectReader& reader) {
    Fixture fixture;
    fixture.id = reader.parsed<FixtureId>("id", FixtureId::parse, "a fixture id (<room>/fx:<socket>[.<part>])");
    fixture.role = reader.enumeration<FixtureRole>("role", parseFixtureRole,
                                                   "one of hero-module, side-module, display, hero-corner, decoration");
    fixture.prefab = reader.prefab("prefab");
    fixture.xMm = reader.integer("xMm", -kMaxCoordinateMm, kMaxCoordinateMm);
    fixture.yMm = reader.integer("yMm", -kMaxCoordinateMm, kMaxCoordinateMm);
    fixture.zMm = reader.integer("zMm", -kMaxCoordinateMm, kMaxCoordinateMm);
    fixture.yawCdeg = static_cast<std::int32_t>(reader.integer("yawCdeg", 0, 35'999));
    fixture.widthMm = reader.integer("widthMm", 1, kMaxSizeMm);
    fixture.heightMm = reader.integer("heightMm", 1, kMaxSizeMm);
    fixture.depthMm = reader.integer("depthMm", 1, kMaxSizeMm);
    const Json& levels = reader.array("levels");
    detail::forEachElement(levels, reader.pointer("levels"), [&](const Json& element, const std::string& pointer) {
        fixture.levels.push_back(
            readLevel(ObjectReader(reader.document(), element, pointer, {"level", "section", "slots"})));
    });
    detail::requireStrictOrder(
        reader.document(), fixture.levels, reader.pointer("levels"),
        [](const FixtureLevel& level) { return level.level; }, "levels are sorted by level");
    return fixture;
}

Placement readPlacement(const ObjectReader& reader) {
    Placement placement;
    placement.slot = reader.parsed<SlotId>("slot", SlotId::parse, "a slot id (<fixture>/s:<level>.<index>)");
    placement.game = reader.parsed<catalog::GameId>("game", catalog::GameId::parse, "a GameId");
    placement.insetMm = reader.integer("insetMm", 0, kMaxInsetMm);
    placement.yawCdeg =
        static_cast<std::int32_t>(reader.integer("yawCdeg", -kMaxPlacementYawCdeg, kMaxPlacementYawCdeg));
    return placement;
}

DressedLevel readDressed(const ObjectReader& reader) {
    DressedLevel dressed;
    dressed.level = reader.parsed<LevelId>("level", LevelId::parse, "a level id (<fixture>/l:<level>)");
    dressed.firstSlot = static_cast<std::uint32_t>(reader.integer("firstSlot", 0, kMaxSlotIndex));
    dressed.count = static_cast<std::uint32_t>(reader.integer("count", 1, std::int64_t{kMaxSlotIndex} + 1));
    dressed.prop = reader.prefab("prop");
    if (!reader.document().failed() &&
        std::int64_t{dressed.firstSlot} + dressed.count > std::int64_t{kMaxSlotIndex} + 1) {
        reader.document().fail(reader.pointer("count"), "the slots run past the last slot index");
    }
    return dressed;
}

PlanLight readLight(const ObjectReader& reader) {
    PlanLight light;
    light.id = reader.parsed<FixtureId>("id", FixtureId::parse, "a fixture id (<room>/fx:<socket>[.<part>])");
    light.role =
        reader.enumeration<LightRole>("role", parseLightRole, "one of key, pendant, lamp, picture, wall-washer");
    light.xMm = reader.integer("xMm", -kMaxCoordinateMm, kMaxCoordinateMm);
    light.yMm = reader.integer("yMm", -kMaxCoordinateMm, kMaxCoordinateMm);
    light.zMm = reader.integer("zMm", -kMaxCoordinateMm, kMaxCoordinateMm);
    light.targetXMm = reader.integer("targetXMm", -kMaxCoordinateMm, kMaxCoordinateMm);
    light.targetYMm = reader.integer("targetYMm", -kMaxCoordinateMm, kMaxCoordinateMm);
    light.targetZMm = reader.integer("targetZMm", -kMaxCoordinateMm, kMaxCoordinateMm);
    light.spanMm = reader.integer("spanMm", 0, kMaxSizeMm);
    if (reader.document().failed() || lightKind(light.role) != LightKind::Point) {
        return light;
    }
    if (light.targetXMm != light.xMm || light.targetYMm != light.yMm || light.targetZMm != light.zMm) {
        reader.document().fail(reader.pointer("targetXMm"), "a point light's target is its own position");
    } else if (light.spanMm != 0) {
        reader.document().fail(reader.pointer("spanMm"), "a point light has no span");
    }
    return light;
}

template <class T, class Read>
std::vector<T> readList(const ObjectReader& document, std::string_view key,
                        std::initializer_list<std::string_view> keys, Read&& read) {
    std::vector<T> items;
    const Json& array = document.array(key);
    items.reserve(array.size());
    detail::forEachElement(array, document.pointer(key), [&](const Json& element, const std::string& pointer) {
        items.push_back(read(ObjectReader(document.document(), element, pointer, keys)));
    });
    return items;
}

/// Reads the plan from its document, of schema `schema` (1 or 2), recording the first problem in `document`.
WorldPlan readDocument(DocumentReader& document, const Json& value, std::int64_t schema) {
    const ObjectReader reader =
        schema == 1 ? ObjectReader(document, value, "",
                                   {"catalogSortKeyVersion", "dressing", "fixtures", "generatorVersion", "inputDigest",
                                    "library", "lights", "placements", "preset", "randomAlgorithm", "rooms",
                                    "schemaVersion", "seed", "titleKeyRule", "unplaced"})
                    : ObjectReader(document, value, "",
                                   {"catalogSortKeyVersion", "dressing", "fixtures", "floors", "generatorVersion",
                                    "inputDigest", "library", "lights", "placements", "preset", "randomAlgorithm",
                                    "rooms", "schemaVersion", "seed", "titleKeyRule", "unplaced"});
    WorldPlan plan;
    plan.schemaVersion = reader.integer("schemaVersion", schema, schema);
    plan.generatorVersion =
        reader.enumeration<GeneratorVersion>("generatorVersion", GeneratorVersion::parse, "a version <major>.<minor>");
    plan.randomAlgorithm = reader.checkedString("randomAlgorithm", isValidRuleName, "a rule name (<name>/<version>)");
    plan.titleKeyRule = reader.checkedString("titleKeyRule", isValidRuleName, "a rule name (<name>/<version>)");
    plan.catalogSortKeyVersion = static_cast<std::uint32_t>(reader.integer("catalogSortKeyVersion", 1, kMaxVersion));
    plan.library = reader.parsed<catalog::LibraryId>("library", catalog::LibraryId::parse, "a library id");
    plan.seed = reader.unsignedInteger("seed");
    plan.preset = reader.dataId("preset");
    plan.inputDigest = reader.checkedString("inputDigest", isSha256Hex, "a SHA-256 in lowercase hexadecimal");

    if (schema >= 2) {
        plan.floors = readList<Floor>(reader, "floors", {"id", "order"}, [](const ObjectReader& floor) {
            Floor read;
            read.id = floor.dataId("id");
            read.order = static_cast<std::uint32_t>(floor.integer("order", 0, kMaxFloorOrder));
            return read;
        });
        detail::requireStrictOrder(
            document, plan.floors, "/floors", [](const Floor& floor) { return std::tie(floor.order, floor.id); },
            "floors are sorted by order, then id");
        std::set<std::string> floorIds;
        for (std::size_t i = 0; i < plan.floors.size() && !document.failed(); ++i) {
            if (!floorIds.insert(plan.floors[i].id).second) {
                document.fail(detail::pointerChild(detail::pointerChild("/floors", i), "id"), "appears twice");
            }
        }
    }
    plan.rooms = readList<Room>(reader, "rooms", schema == 1 ? kRoomKeysV1 : kRoomKeysV2,
                                [schema](const ObjectReader& room) { return readRoom(room, schema); });
    detail::requireStrictOrder(
        document, plan.rooms, "/rooms", [](const Room& room) { return std::tie(room.id); }, "rooms are sorted by id");
    if (schema >= 2 && !document.failed() && !plan.rooms.empty()) {
        GeneratorVersion highest = plan.rooms.front().generatorVersion;
        for (const Room& room : plan.rooms) {
            highest = std::max(highest, room.generatorVersion);
        }
        if (plan.generatorVersion != highest) {
            document.fail("/generatorVersion",
                          "the plan's generator version is the highest of its rooms', " + highest.toString());
        }
    }
    plan.fixtures = readList<Fixture>(
        reader, "fixtures",
        {"depthMm", "heightMm", "id", "levels", "prefab", "role", "widthMm", "xMm", "yMm", "yawCdeg", "zMm"},
        readFixture);
    detail::requireStrictOrder(
        document, plan.fixtures, "/fixtures", [](const Fixture& fixture) { return std::tie(fixture.id); },
        "fixtures are sorted by id");
    plan.placements = readList<Placement>(reader, "placements", {"game", "insetMm", "slot", "yawCdeg"}, readPlacement);
    detail::requireStrictOrder(
        document, plan.placements, "/placements",
        [](const Placement& placement) { return std::tie(placement.slot, placement.game); },
        "placements are sorted by slot, then game");
    plan.dressing = readList<DressedLevel>(reader, "dressing", {"count", "firstSlot", "level", "prop"}, readDressed);
    detail::requireStrictOrder(
        document, plan.dressing, "/dressing",
        [](const DressedLevel& dressed) { return std::tie(dressed.level, dressed.firstSlot); },
        "dressing is sorted by level, then first slot");
    plan.lights = readList<PlanLight>(
        reader, "lights", {"id", "role", "spanMm", "targetXMm", "targetYMm", "targetZMm", "xMm", "yMm", "zMm"},
        readLight);
    detail::requireStrictOrder(
        document, plan.lights, "/lights", [](const PlanLight& light) { return std::tie(light.id); },
        "lights are sorted by id");
    plan.unplaced = detail::readUnplacedList(reader, "unplaced");
    if (schema == 1) {
        for (std::size_t i = 0; i < plan.unplaced.size() && !document.failed(); ++i) {
            if (plan.unplaced[i].reason == UnplacedReason::RoomKept) {
                document.fail(detail::pointerChild(detail::pointerChild("/unplaced", i), "reason"),
                              "schema 1 has no reason room-kept");
            }
        }
    }
    return plan;
}

/// The plan's canonical document and text. The writer checks what it wrote with the reader, so that a plan it writes
/// is always one it reads back. Only schema 2 is written.
Expected<std::pair<Json, std::string>> canonicalPlan(const WorldPlan& plan) {
    if (plan.schemaVersion != kPlanSchemaVersion) {
        return makeError(ErrorCode::InvalidArgument,
                         "only schema " + std::to_string(kPlanSchemaVersion) + " is written; a plan of schema " +
                             std::to_string(plan.schemaVersion) + " is lifted first (lift.hpp)",
                         "/schemaVersion");
    }
    WorldPlan sorted = plan;
    sortPlan(sorted);
    Json document = documentJson(sorted);
    auto text = core::writeCanonicalJson(document);
    if (!text) {
        return propagate(text.error());
    }
    DocumentReader reader(*text, "world plan");
    (void)readDocument(reader, document, kPlanSchemaVersion);
    if (reader.failed()) {
        Error error = reader.error();
        error.code = ErrorCode::InvalidArgument;
        return propagate(std::move(error));
    }
    return std::pair{std::move(document), std::move(*text)};
}

} // namespace

Expected<nlohmann::json> planToJson(const WorldPlan& plan) {
    auto canonical = canonicalPlan(plan);
    if (!canonical) {
        return propagate(canonical.error());
    }
    return std::move(canonical->first);
}

Expected<std::string> writePlan(const WorldPlan& plan) {
    auto canonical = canonicalPlan(plan);
    if (!canonical) {
        return propagate(canonical.error());
    }
    return std::move(canonical->second);
}

Expected<std::string> planDigest(const WorldPlan& plan) {
    auto text = writePlan(plan);
    if (!text) {
        return propagate(text.error());
    }
    return core::sha256Hex(*text);
}

Expected<std::string> detail::sortedPlanDigest(const WorldPlan& sortedPlan) {
    return core::canonicalJsonDigest(documentJson(sortedPlan));
}

Expected<WorldPlan> readPlan(std::string_view text, std::string_view context) {
    auto value = core::parseCanonicalJson(text, context);
    if (!value) {
        return propagate(value.error());
    }
    DocumentReader document(text, std::string(context));
    // The schema first: a newer plan is reported as such, whatever else it holds; an older one is read by its own
    // keys.
    std::int64_t schema = kPlanSchemaVersion;
    if (value->is_object()) {
        const auto found = value->find("schemaVersion");
        if (found != value->end() && found->is_number_unsigned()) {
            const std::uint64_t written = found->get<std::uint64_t>();
            if (written > static_cast<std::uint64_t>(kPlanSchemaVersion)) {
                document.fail("/schemaVersion",
                              "schema " + found->dump() + " is newer than this build reads (" +
                                  std::to_string(kPlanSchemaVersion) + "): the plan was written by a newer Retroverse",
                              ErrorCode::Unsupported);
                return propagate(document.error());
            }
            if (written >= static_cast<std::uint64_t>(kOldestPlanSchemaVersion)) {
                schema = static_cast<std::int64_t>(written);
            }
        }
    }
    WorldPlan plan = readDocument(document, *value, schema);
    if (document.failed()) {
        return propagate(document.error());
    }
    return plan;
}

Expected<std::string> roomContentDigest(const WorldPlan& plan, const RoomId& room) {
    WorldPlan sorted = plan;
    sortPlan(sorted);
    const auto found = std::find_if(sorted.rooms.begin(), sorted.rooms.end(),
                                    [&](const Room& candidate) { return candidate.id == room; });
    if (found == sorted.rooms.end()) {
        return makeError(ErrorCode::NotFound, "the plan has no such room", room.toString());
    }
    Json fixtures = Json::array();
    for (const Fixture& fixture : sorted.fixtures) {
        if (fixture.id.room() == room) {
            fixtures.push_back(fixtureJson(fixture));
        }
    }
    Json placements = Json::array();
    for (const Placement& placement : sorted.placements) {
        if (placement.slot.fixture().room() == room) {
            placements.push_back(placementJson(placement));
        }
    }
    Json dressing = Json::array();
    for (const DressedLevel& dressed : sorted.dressing) {
        if (dressed.level.fixture().room() == room) {
            dressing.push_back(dressingJson(dressed));
        }
    }
    Json lights = Json::array();
    for (const PlanLight& light : sorted.lights) {
        if (light.id.room() == room) {
            lights.push_back(lightJson(light));
        }
    }
    Json unplaced = Json::array();
    for (const UnplacedGame& game : sorted.unplaced) {
        if (game.room == room) {
            unplaced.push_back(detail::unplacedJson(game));
        }
    }
    return core::canonicalJsonDigest(Json{{"room", roomJson(*found)},
                                          {"fixtures", std::move(fixtures)},
                                          {"placements", std::move(placements)},
                                          {"dressing", std::move(dressing)},
                                          {"lights", std::move(lights)},
                                          {"unplaced", std::move(unplaced)}});
}

} // namespace rv::world
