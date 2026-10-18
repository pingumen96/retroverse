#include <rv/world/plan_json.hpp>

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

Json prefabJson(const PrefabRef& prefab) {
    return Json{{"kind", prefab.kind}, {"manifest", prefab.manifest}};
}

Json roomJson(const Room& room) {
    return Json{
        {"id", room.id.toString()}, {"template", room.templateId},     {"templateVersion", room.templateVersion},
        {"minXMm", room.minXMm},    {"maxXMm", room.maxXMm},           {"minZMm", room.minZMm},
        {"maxZMm", room.maxZMm},    {"heightMm", room.heightMm},       {"growthSteps", room.growthSteps},
        {"seed", room.seed},        {"mediaFamily", room.mediaFamily}, {"themePack", room.themePack}};
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

UnplacedGame readUnplaced(const ObjectReader& reader) {
    UnplacedGame unplaced;
    unplaced.game = reader.parsed<catalog::GameId>("game", catalog::GameId::parse, "a GameId");
    unplaced.room = reader.parsed<RoomId>("room", RoomId::parse, "a room id (room:<system>)");
    unplaced.reason = reader.enumeration<UnplacedReason>("reason", parseUnplacedReason,
                                                         "one of duplicate-title, no-cover, unavailable, capacity");
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
                {"rooms", std::move(rooms)},
                {"fixtures", std::move(fixtures)},
                {"placements", std::move(placements)},
                {"dressing", std::move(dressing)},
                {"unplaced", std::move(unplaced)}};
}

Room readRoom(const ObjectReader& reader) {
    Room room;
    room.id = reader.parsed<RoomId>("id", RoomId::parse, "a room id (room:<system>)");
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

/// Reads the plan from its document, recording the first problem in `document`.
WorldPlan readDocument(DocumentReader& document, const Json& value) {
    const ObjectReader reader(document, value, "",
                              {"catalogSortKeyVersion", "dressing", "fixtures", "generatorVersion", "inputDigest",
                               "library", "placements", "preset", "randomAlgorithm", "rooms", "schemaVersion", "seed",
                               "titleKeyRule", "unplaced"});
    WorldPlan plan;
    plan.schemaVersion = reader.integer("schemaVersion", kPlanSchemaVersion, kPlanSchemaVersion);
    plan.generatorVersion =
        reader.enumeration<GeneratorVersion>("generatorVersion", GeneratorVersion::parse, "a version <major>.<minor>");
    plan.randomAlgorithm = reader.checkedString("randomAlgorithm", isValidRuleName, "a rule name (<name>/<version>)");
    plan.titleKeyRule = reader.checkedString("titleKeyRule", isValidRuleName, "a rule name (<name>/<version>)");
    plan.catalogSortKeyVersion = static_cast<std::uint32_t>(reader.integer("catalogSortKeyVersion", 1, kMaxVersion));
    plan.library = reader.parsed<catalog::LibraryId>("library", catalog::LibraryId::parse, "a library id");
    plan.seed = reader.unsignedInteger("seed");
    plan.preset = reader.dataId("preset");
    plan.inputDigest = reader.checkedString("inputDigest", isSha256Hex, "a SHA-256 in lowercase hexadecimal");

    plan.rooms = readList<Room>(reader, "rooms",
                                {"growthSteps", "heightMm", "id", "maxXMm", "maxZMm", "mediaFamily", "minXMm", "minZMm",
                                 "seed", "template", "templateVersion", "themePack"},
                                readRoom);
    detail::requireStrictOrder(
        document, plan.rooms, "/rooms", [](const Room& room) { return std::tie(room.id); }, "rooms are sorted by id");
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
    plan.unplaced = detail::readUnplacedList(reader, "unplaced");
    return plan;
}

/// The plan's canonical document and text. The writer checks what it wrote with the reader, so that a plan it writes
/// is always one it reads back.
Expected<std::pair<Json, std::string>> canonicalPlan(const WorldPlan& plan) {
    WorldPlan sorted = plan;
    sortPlan(sorted);
    Json document = documentJson(sorted);
    auto text = core::writeCanonicalJson(document);
    if (!text) {
        return propagate(text.error());
    }
    DocumentReader reader(*text, "world plan");
    (void)readDocument(reader, document);
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

Expected<WorldPlan> readPlan(std::string_view text, std::string_view context) {
    auto value = core::parseCanonicalJson(text, context);
    if (!value) {
        return propagate(value.error());
    }
    DocumentReader document(text, std::string(context));
    // The schema first: a newer plan is reported as such, whatever else it holds.
    if (value->is_object()) {
        const auto found = value->find("schemaVersion");
        if (found != value->end() && found->is_number_unsigned() &&
            found->get<std::uint64_t>() > static_cast<std::uint64_t>(kPlanSchemaVersion)) {
            document.fail("/schemaVersion",
                          "schema " + found->dump() + " is newer than this build reads (" +
                              std::to_string(kPlanSchemaVersion) + "): the plan was written by a newer Retroverse",
                          ErrorCode::Unsupported);
            return propagate(document.error());
        }
    }
    WorldPlan plan = readDocument(document, *value);
    if (document.failed()) {
        return propagate(document.error());
    }
    return plan;
}

} // namespace rv::world
