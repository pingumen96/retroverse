// The WorldPlan's canonical JSON (rv/world/plan_json.hpp, ADR-0010 §8, ADR-0013 §5): one plan is one text, its lists
// sorted whatever their order in memory; the text is read back as the same plan and written again byte for byte; a
// plan of a newer schema (3), a hand-edited text and every malformed field are refused with the line they stand on; a
// schema-1 text is read as its content and never written; a room's content digest covers its record and elements.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rv/core/sha256.hpp>
#include <rv/world/plan_json.hpp>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::EndsWith;
using rv::catalog::GameId;
using rv::world::DressedLevel;
using rv::world::Fixture;
using rv::world::FixtureId;
using rv::world::FixtureLevel;
using rv::world::FixtureRole;
using rv::world::LevelId;
using rv::world::LightRole;
using rv::world::PlanLight;
using rv::world::Placement;
using rv::world::RoomId;
using rv::world::Section;
using rv::world::SlotId;
using rv::world::UnplacedGame;
using rv::world::UnplacedReason;
using rv::world::WorldPlan;

namespace {

GameId game(std::uint64_t value) {
    return *GameId::fromValue(value);
}

RoomId snesRoom() {
    return *RoomId::forSystem(*rv::catalog::SystemName::parse("snes"));
}

FixtureId fixtureId(std::string_view socket, std::string_view part = {}) {
    return *FixtureId::make(snesRoom(), socket, part);
}

/// A small plan with a field of every kind, its lists out of their canonical order.
WorldPlan samplePlan() {
    WorldPlan plan;
    plan.generatorVersion = {1, 0};
    plan.randomAlgorithm = "splitmix64/1";
    plan.titleKeyRule = "title-key/1";
    plan.catalogSortKeyVersion = 1;
    plan.seed = 18446744073709551615ull;
    plan.preset = "retro_museum_cozy";
    plan.inputDigest = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    plan.floors = {{"gen4", 3}, {"ground", 0}};

    rv::world::Room room;
    room.id = snesRoom();
    room.templateId = "console_room_9x10";
    room.templateVersion = 1;
    room.minXMm = -4500;
    room.maxXMm = 4500;
    room.minZMm = -5000;
    room.maxZMm = 5000;
    room.heightMm = 3200;
    room.seed = 42;
    room.mediaFamily = "cartridge_home_16bit";
    room.themePack = "classic_16bit_warm";
    room.kind = rv::world::RoomKind::Console;
    room.floor = "gen4";
    room.sections = {room.id.system()};
    room.placement = {7650, -6650, 0};
    room.generatorVersion = {1, 0};
    plan.rooms.push_back(room);

    Fixture module;
    module.id = fixtureId("hw-w");
    module.role = FixtureRole::HeroModule;
    module.prefab = {"furniture.shelf.hero_module.generic", "shelf"};
    module.xMm = -2425;
    module.zMm = -4795;
    module.widthMm = 900;
    module.heightMm = 2000;
    module.depthMm = 350;
    module.levels = {{4, 6, Section::Band}, {0, 33, Section::Spine}};
    Fixture crt;
    crt.id = fixtureId("hero", "crt");
    crt.role = FixtureRole::HeroCorner;
    crt.prefab = {"electronics.crt.consumer.generic", "crt"};
    crt.xMm = -805;
    crt.yMm = 550;
    crt.zMm = -4690;
    crt.widthMm = 710;
    crt.heightMm = 610;
    crt.depthMm = 560;
    Fixture display;
    display.id = fixtureId("dc01");
    display.role = FixtureRole::Display;
    display.prefab = {"furniture.display_case.generic", "display_case"};
    display.xMm = -1000;
    display.zMm = 1000;
    display.yawCdeg = 9000;
    display.widthMm = 1200;
    display.heightMm = 900;
    display.depthMm = 500;
    display.levels = {{0, 11, Section::Featured}};
    plan.fixtures = {module, crt, display};

    plan.placements = {{*SlotId::make(fixtureId("hw-w"), 0, 1), game(1), 3, 0},
                       {*SlotId::make(fixtureId("dc01"), 0, 0), game(2), 0, 120}};
    plan.dressing = {{*LevelId::make(fixtureId("hw-w"), 0), 2, 31, {"decor.storage_box.generic", "storage_box"}}};
    plan.unplaced = {{game(4), snesRoom(), UnplacedReason::NoCover, {}},
                     {game(3), snesRoom(), UnplacedReason::DuplicateTitle, game(1)}};
    return plan;
}

// The canonical text of samplePlan(), written by hand from the format: keys by code point, one element per line in the
// top-level arrays, nothing else between the tokens, a final line feed.
const std::string kSampleText =
    "{\"catalogSortKeyVersion\":1,\"dressing\":[\n"
    "{\"count\":31,\"firstSlot\":2,\"level\":\"room:snes/fx:hw-w/l:0\","
    "\"prop\":{\"kind\":\"storage_box\",\"manifest\":\"decor.storage_box.generic\"}}\n"
    "],\"fixtures\":[\n"
    "{\"depthMm\":500,\"heightMm\":900,\"id\":\"room:snes/fx:dc01\","
    "\"levels\":[{\"level\":0,\"section\":\"featured\",\"slots\":11}],"
    "\"prefab\":{\"kind\":\"display_case\",\"manifest\":\"furniture.display_case.generic\"},\"role\":\"display\","
    "\"widthMm\":1200,\"xMm\":-1000,\"yMm\":0,\"yawCdeg\":9000,\"zMm\":1000},\n"
    "{\"depthMm\":560,\"heightMm\":610,\"id\":\"room:snes/fx:hero.crt\",\"levels\":[],"
    "\"prefab\":{\"kind\":\"crt\",\"manifest\":\"electronics.crt.consumer.generic\"},\"role\":\"hero-corner\","
    "\"widthMm\":710,\"xMm\":-805,\"yMm\":550,\"yawCdeg\":0,\"zMm\":-4690},\n"
    "{\"depthMm\":350,\"heightMm\":2000,\"id\":\"room:snes/fx:hw-w\","
    "\"levels\":[{\"level\":0,\"section\":\"spine\",\"slots\":33},{\"level\":4,\"section\":\"band\",\"slots\":6}],"
    "\"prefab\":{\"kind\":\"shelf\",\"manifest\":\"furniture.shelf.hero_module.generic\"},\"role\":\"hero-module\","
    "\"widthMm\":900,\"xMm\":-2425,\"yMm\":0,\"yawCdeg\":0,\"zMm\":-4795}\n"
    "],\"floors\":[\n"
    "{\"id\":\"ground\",\"order\":0},\n"
    "{\"id\":\"gen4\",\"order\":3}\n"
    "],\"generatorVersion\":\"1.0\","
    "\"inputDigest\":\"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\",\"library\":\"esde\","
    "\"lights\":[],\"placements\":[\n"
    "{\"game\":\"g0000000000000002\",\"insetMm\":0,\"slot\":\"room:snes/fx:dc01/s:0.0\",\"yawCdeg\":120},\n"
    "{\"game\":\"g0000000000000001\",\"insetMm\":3,\"slot\":\"room:snes/fx:hw-w/s:0.1\",\"yawCdeg\":0}\n"
    "],\"preset\":\"retro_museum_cozy\",\"randomAlgorithm\":\"splitmix64/1\",\"rooms\":[\n"
    "{\"floor\":\"gen4\",\"generatorVersion\":\"1.0\",\"growthSteps\":0,\"heightMm\":3200,\"id\":\"room:snes\","
    "\"kind\":\"console\",\"maxXMm\":4500,\"maxZMm\":5000,\"mediaFamily\":\"cartridge_home_16bit\",\"minXMm\":-4500,"
    "\"minZMm\":-5000,\"placement\":{\"xMm\":7650,\"yawHalfTurns\":0,\"zMm\":-6650},\"sections\":[\"snes\"],"
    "\"seed\":42,\"template\":\"console_room_9x10\",\"templateVersion\":1,\"themePack\":\"classic_16bit_warm\"}\n"
    "],\"schemaVersion\":2,\"seed\":18446744073709551615,\"titleKeyRule\":\"title-key/1\",\"unplaced\":[\n"
    "{\"game\":\"g0000000000000003\",\"reason\":\"duplicate-title\",\"representative\":\"g0000000000000001\","
    "\"room\":\"room:snes\"},\n"
    "{\"game\":\"g0000000000000004\",\"reason\":\"no-cover\",\"room\":\"room:snes\"}\n"
    "]}\n";

// The same plan as I3 wrote it in schema 1 (no floors, no building fields in its room).
const std::string kSampleTextV1 =
    "{\"catalogSortKeyVersion\":1,\"dressing\":[\n"
    "{\"count\":31,\"firstSlot\":2,\"level\":\"room:snes/fx:hw-w/l:0\","
    "\"prop\":{\"kind\":\"storage_box\",\"manifest\":\"decor.storage_box.generic\"}}\n"
    "],\"fixtures\":[\n"
    "{\"depthMm\":500,\"heightMm\":900,\"id\":\"room:snes/fx:dc01\","
    "\"levels\":[{\"level\":0,\"section\":\"featured\",\"slots\":11}],"
    "\"prefab\":{\"kind\":\"display_case\",\"manifest\":\"furniture.display_case.generic\"},\"role\":\"display\","
    "\"widthMm\":1200,\"xMm\":-1000,\"yMm\":0,\"yawCdeg\":9000,\"zMm\":1000},\n"
    "{\"depthMm\":560,\"heightMm\":610,\"id\":\"room:snes/fx:hero.crt\",\"levels\":[],"
    "\"prefab\":{\"kind\":\"crt\",\"manifest\":\"electronics.crt.consumer.generic\"},\"role\":\"hero-corner\","
    "\"widthMm\":710,\"xMm\":-805,\"yMm\":550,\"yawCdeg\":0,\"zMm\":-4690},\n"
    "{\"depthMm\":350,\"heightMm\":2000,\"id\":\"room:snes/fx:hw-w\","
    "\"levels\":[{\"level\":0,\"section\":\"spine\",\"slots\":33},{\"level\":4,\"section\":\"band\",\"slots\":6}],"
    "\"prefab\":{\"kind\":\"shelf\",\"manifest\":\"furniture.shelf.hero_module.generic\"},\"role\":\"hero-module\","
    "\"widthMm\":900,\"xMm\":-2425,\"yMm\":0,\"yawCdeg\":0,\"zMm\":-4795}\n"
    "],\"generatorVersion\":\"1.0\","
    "\"inputDigest\":\"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\",\"library\":\"esde\","
    "\"lights\":[],\"placements\":[\n"
    "{\"game\":\"g0000000000000002\",\"insetMm\":0,\"slot\":\"room:snes/fx:dc01/s:0.0\",\"yawCdeg\":120},\n"
    "{\"game\":\"g0000000000000001\",\"insetMm\":3,\"slot\":\"room:snes/fx:hw-w/s:0.1\",\"yawCdeg\":0}\n"
    "],\"preset\":\"retro_museum_cozy\",\"randomAlgorithm\":\"splitmix64/1\",\"rooms\":[\n"
    "{\"growthSteps\":0,\"heightMm\":3200,\"id\":\"room:snes\",\"maxXMm\":4500,\"maxZMm\":5000,"
    "\"mediaFamily\":\"cartridge_home_16bit\",\"minXMm\":-4500,\"minZMm\":-5000,\"seed\":42,"
    "\"template\":\"console_room_9x10\",\"templateVersion\":1,\"themePack\":\"classic_16bit_warm\"}\n"
    "],\"schemaVersion\":1,\"seed\":18446744073709551615,\"titleKeyRule\":\"title-key/1\",\"unplaced\":[\n"
    "{\"game\":\"g0000000000000003\",\"reason\":\"duplicate-title\",\"representative\":\"g0000000000000001\","
    "\"room\":\"room:snes\"},\n"
    "{\"game\":\"g0000000000000004\",\"reason\":\"no-cover\",\"room\":\"room:snes\"}\n"
    "]}\n";

std::string replaced(std::string text, std::string_view from, std::string_view to) {
    const std::size_t at = text.find(from);
    REQUIRE(at != std::string::npos);
    text.replace(at, from.size(), to);
    return text;
}

/// The 1-based line of the first occurrence of `needle`.
std::size_t lineOf(std::string_view text, std::string_view needle) {
    const std::size_t at = text.find(needle);
    REQUIRE(at != std::string_view::npos);
    return 1 + static_cast<std::size_t>(std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(at), '\n'));
}

rv::Error refusal(const std::string& text) {
    auto plan = rv::world::readPlan(text, "world-plan.json");
    REQUIRE_FALSE(plan.has_value());
    return plan.error();
}

} // namespace

TEST_CASE("world/plan_json: a plan's canonical text, its lists sorted", "[world][plan_json]") {
    const WorldPlan plan = samplePlan();
    auto text = rv::world::writePlan(plan);
    REQUIRE(text.has_value());
    CHECK(*text == kSampleText);

    // The order in memory does not matter: reversed lists give the same text.
    WorldPlan reversed = plan;
    std::reverse(reversed.fixtures.begin(), reversed.fixtures.end());
    std::reverse(reversed.placements.begin(), reversed.placements.end());
    std::reverse(reversed.unplaced.begin(), reversed.unplaced.end());
    std::reverse(reversed.fixtures.front().levels.begin(), reversed.fixtures.front().levels.end());
    CHECK(*rv::world::writePlan(reversed) == kSampleText);

    // The digest is the SHA-256 of the text.
    auto digest = rv::world::planDigest(plan);
    REQUIRE(digest.has_value());
    CHECK(*digest == rv::core::sha256Hex(kSampleText));
    CHECK(rv::world::isSha256Hex(*digest));
}

TEST_CASE("world/plan_json: the text is read back as the plan and written again byte for byte", "[world][plan_json]") {
    auto plan = rv::world::readPlan(kSampleText, "world-plan.json");
    REQUIRE(plan.has_value());
    WorldPlan expected = samplePlan();
    rv::world::sortPlan(expected);
    CHECK(*plan == expected);
    CHECK(*rv::world::writePlan(*plan) == kSampleText);

    // An empty plan has a text too.
    WorldPlan empty = samplePlan();
    empty.rooms.clear();
    empty.fixtures.clear();
    empty.placements.clear();
    empty.dressing.clear();
    empty.unplaced.clear();
    auto emptyText = rv::world::writePlan(empty);
    REQUIRE(emptyText.has_value());
    CHECK_THAT(*emptyText, ContainsSubstring("\"fixtures\":[],"));
    rv::world::sortPlan(empty);
    CHECK(*rv::world::readPlan(*emptyText, "empty.json") == empty);
}

TEST_CASE("world/plan_json: shared, arcade and expansion rooms round-trip byte for byte", "[world][plan_json]") {
    // Schema 2's reserved forms (ADR-0013 §5, §8), which nothing generates yet: a shared room of several sections in
    // their recorded order, the arcade room, an expansion of a console room and one of the shared room.
    WorldPlan plan = samplePlan();
    const rv::world::Room head = plan.rooms.front();
    const auto system = [](std::string_view name) { return *rv::catalog::SystemName::parse(name); };
    rv::world::Room shared = head;
    shared.id = *RoomId::sharedArea("gen4");
    shared.kind = rv::world::RoomKind::Shared;
    shared.sections = {system("pcengine"), system("megadrive")};
    shared.placement = {7650, 6650, 1};
    rv::world::Room sharedMore = shared;
    sharedMore.id = *RoomId::sharedArea("gen4", 2);
    sharedMore.expands = shared.id;
    sharedMore.placement = {16950, 6650, 1};
    rv::world::Room arcade = head;
    arcade.id = *RoomId::arcadeArea();
    arcade.kind = rv::world::RoomKind::Arcade;
    arcade.floor = "ground";
    arcade.sections = {system("mame"), system("fbneo")};
    arcade.mediaFamily = "generic_case";
    rv::world::Room expansion = head;
    expansion.id = *RoomId::forSystem(head.id.system(), 2);
    expansion.expands = head.id;
    expansion.placement = {16950, -6650, 0};
    plan.rooms = {sharedMore, expansion, arcade, head, shared};
    plan.unplaced.push_back({game(9), shared.id, UnplacedReason::Capacity, {}});

    auto text = rv::world::writePlan(plan);
    REQUIRE(text.has_value());
    CHECK_THAT(*text, ContainsSubstring(R"("expands":"room:snes")"));
    CHECK_THAT(*text, ContainsSubstring(R"("expands":"area:shared:gen4")"));
    CHECK_THAT(*text, ContainsSubstring(R"("sections":["pcengine","megadrive"])"));
    CHECK_THAT(*text, ContainsSubstring(R"("id":"area:arcade","kind":"arcade")"));
    auto read = rv::world::readPlan(*text, "building.json");
    REQUIRE(read.has_value());
    rv::world::sortPlan(plan);
    CHECK(*read == plan);
    CHECK(*rv::world::writePlan(*read) == *text);
    // Rooms in id order: the arcade room, the shared rooms, then the systems' rooms, each chain's head first.
    std::vector<std::string> ids;
    for (const rv::world::Room& room : read->rooms) {
        ids.push_back(room.id.toString());
    }
    CHECK(ids == std::vector<std::string>{"area:arcade", "area:shared:gen4", "area:shared:gen4:2", "room:snes",
                                          "room:snes:2"});
}

TEST_CASE("world/plan_json: a schema-1 text is read as its content and never written", "[world][plan_json]") {
    auto v1 = rv::world::readPlan(kSampleTextV1, "world-plan.json");
    REQUIRE(v1.has_value());
    CHECK(v1->schemaVersion == 1);
    CHECK(v1->floors.empty());
    REQUIRE(v1->rooms.size() == 1);
    CHECK(v1->rooms.front().floor.empty());
    CHECK(v1->rooms.front().sections.empty());
    // Its content is the schema-2 plan's but for the building's fields.
    WorldPlan expected = samplePlan();
    rv::world::sortPlan(expected);
    CHECK(v1->fixtures == expected.fixtures);
    CHECK(v1->placements == expected.placements);
    CHECK(v1->dressing == expected.dressing);
    CHECK(v1->unplaced == expected.unplaced);
    // Only schema 2 is written: a schema-1 plan is lifted first (lift.hpp).
    auto refused = rv::world::writePlan(*v1);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == rv::ErrorCode::InvalidArgument);
    CHECK_THAT(refused.error().message, ContainsSubstring("lifted first"));
    // A schema-1 text with schema 2's keys, a room-kept entry or an area's room, and a schema-2 text without its
    // floors, are refused.
    CHECK(refusal(replaced(kSampleTextV1, "{\"catalogSortKeyVersion\":1,",
                           "{\"catalogSortKeyVersion\":1,\"floors\":[],"))
              .code == rv::ErrorCode::ValidationFailed);
    CHECK_THAT(refusal(replaced(kSampleTextV1, "\"reason\":\"no-cover\"", "\"reason\":\"room-kept\""))
                   .message,
               ContainsSubstring("schema 1 has no reason room-kept"));
    CHECK_THAT(refusal(replaced(kSampleText, "],\"floors\":[\n{\"id\":\"ground\",\"order\":0},\n"
                                             "{\"id\":\"gen4\",\"order\":3}\n],",
                                "],"))
                   .message,
               ContainsSubstring("/floors: missing"));
}

TEST_CASE("world/plan_json: a room's content digest covers its record and its elements", "[world][plan_json]") {
    const WorldPlan plan = samplePlan();
    const RoomId room = snesRoom();
    auto digest = rv::world::roomContentDigest(plan, room);
    REQUIRE(digest.has_value());
    CHECK(rv::world::isSha256Hex(*digest));
    // The order in memory and the plan's header do not matter; the room's elements do.
    WorldPlan reversed = plan;
    std::reverse(reversed.fixtures.begin(), reversed.fixtures.end());
    reversed.inputDigest = std::string(64, 'b');
    reversed.seed = 7;
    CHECK(*rv::world::roomContentDigest(reversed, room) == *digest);
    WorldPlan moved = plan;
    moved.placements.front().insetMm += 1;
    CHECK(*rv::world::roomContentDigest(moved, room) != *digest);
    WorldPlan grown = plan;
    grown.rooms.front().placement.zMm -= 2000;
    CHECK(*rv::world::roomContentDigest(grown, room) != *digest);
    WorldPlan relisted = plan;
    relisted.unplaced.pop_back();
    CHECK(*rv::world::roomContentDigest(relisted, room) != *digest);
    // Another room's elements do not: a second room added to the plan leaves this one's digest.
    WorldPlan building = plan;
    rv::world::Room other = plan.rooms.front();
    other.id = *RoomId::forSystem(*rv::catalog::SystemName::parse("megadrive"));
    other.sections = {other.id.system()};
    other.placement.yawHalfTurns = 1;
    building.rooms.push_back(other);
    building.unplaced.push_back({game(77), other.id, UnplacedReason::NoCover, {}});
    CHECK(*rv::world::roomContentDigest(building, room) == *digest);
    CHECK(*rv::world::roomContentDigest(building, other.id) != *digest);
    CHECK(rv::world::roomContentDigest(plan, other.id).error().code == rv::ErrorCode::NotFound);
    // Every room's at once: the same digests.
    const auto all = rv::world::roomContentDigests(building);
    REQUIRE(all.has_value());
    CHECK(all->size() == 2);
    CHECK(all->at(room) == *digest);
    CHECK(all->at(other.id) == *rv::world::roomContentDigest(building, other.id));
}

TEST_CASE("world/plan_json: a large plan round-trips byte for byte", "[world][plan_json]") {
    WorldPlan plan = samplePlan();
    plan.placements.clear();
    // 2,000 placements over 8 modules of 7 levels of 36 slots, in a scrambled order.
    for (std::uint64_t i = 0; i < 2000; ++i) {
        const std::uint64_t n = (i * 7919) % 2000;
        const std::string socket = "w0" + std::to_string(1 + n / 252);
        const auto slot = SlotId::make(fixtureId(socket), static_cast<std::uint32_t>((n % 252) / 36),
                                       static_cast<std::uint32_t>(n % 36));
        REQUIRE(slot.has_value());
        plan.placements.push_back({*slot, game(1000 + n), static_cast<std::int64_t>(n % 5), 0});
    }
    auto text = rv::world::writePlan(plan);
    REQUIRE(text.has_value());
    auto read = rv::world::readPlan(*text, "large.json");
    REQUIRE(read.has_value());
    CHECK(read->placements.size() == 2000);
    CHECK(std::is_sorted(read->placements.begin(), read->placements.end()));
    CHECK(*rv::world::writePlan(*read) == *text);
}

TEST_CASE("world/plan_json: a newer schema is refused as such", "[world][plan_json]") {
    const rv::Error newer = refusal(replaced(kSampleText, "\"schemaVersion\":2", "\"schemaVersion\":3"));
    CHECK(newer.code == rv::ErrorCode::Unsupported);
    CHECK_THAT(newer.message, ContainsSubstring("newer"));
    CHECK(newer.context == "world-plan.json:15");

    // Even when the rest of it would not be read by this build.
    std::string future = replaced(kSampleText, "\"schemaVersion\":2", "\"schemaVersion\":7");
    future = replaced(future, "{\"catalogSortKeyVersion\":1,", "{\"aNewList\":[],\"catalogSortKeyVersion\":1,");
    CHECK(refusal(future).code == rv::ErrorCode::Unsupported);

    const rv::Error older = refusal(replaced(kSampleText, "\"schemaVersion\":2", "\"schemaVersion\":0"));
    CHECK(older.code == rv::ErrorCode::ValidationFailed);
    CHECK_THAT(older.message, ContainsSubstring("/schemaVersion"));
}

TEST_CASE("world/plan_json: a text that is not the canonical one is refused", "[world][plan_json]") {
    // White space, an escaped slash, a missing final line feed, a fraction: core::parseCanonicalJson refuses them.
    for (const std::string& text :
         {replaced(kSampleText, "\"seed\":42", "\"seed\": 42"),
          replaced(kSampleText, "room:snes/fx:dc01/s:0.0", "room:snes\\/fx:dc01/s:0.0"),
          kSampleText.substr(0, kSampleText.size() - 1), replaced(kSampleText, "\"insetMm\":3", "\"insetMm\":3.0")}) {
        const rv::Error error = refusal(text);
        CHECK(error.code == rv::ErrorCode::ValidationFailed);
    }
    CHECK(refusal("{").code == rv::ErrorCode::ParseError);
    CHECK(refusal("[]\n").code == rv::ErrorCode::ValidationFailed);
}

TEST_CASE("world/plan_json: malformed fields are refused with their line", "[world][plan_json]") {
    struct Case {
        std::string_view from;
        std::string_view to;
        std::string_view message; ///< a part of the message
        std::string_view lineOf;  ///< text on the line the error must name (in the edited text)
    };
    const Case cases[] = {
        {"{\"catalogSortKeyVersion\":1,", "{\"aaa\":1,\"catalogSortKeyVersion\":1,", "/aaa: unknown key", "aaa"},
        {"\"library\":\"esde\",", "", "/library: missing", "catalogSortKeyVersion"},
        {"\"library\":\"esde\"", "\"library\":\"ES DE\"", "/library: expected a library id", "library"},
        {"\"seed\":18446744073709551615", "\"seed\":\"x\"", "/seed: expected an integer", "\"seed\":\"x\""},
        {"\"seed\":18446744073709551615", "\"seed\":-1", "/seed: expected an integer from 0", "\"seed\":-1"},
        {"\"generatorVersion\":\"1.0\"", "\"generatorVersion\":\"1.01\"", "/generatorVersion", "generatorVersion"},
        {"\"generatorVersion\":\"1.0\"", "\"generatorVersion\":\"1\"", "/generatorVersion", "generatorVersion"},
        {"\"randomAlgorithm\":\"splitmix64/1\"", "\"randomAlgorithm\":\"splitmix64\"", "/randomAlgorithm",
         "randomAlgorithm"},
        {"\"inputDigest\":\"e3b0", "\"inputDigest\":\"E3B0", "/inputDigest", "inputDigest"},
        {"\"preset\":\"retro_museum_cozy\"", "\"preset\":\"Retro\"", "/preset", "\"preset\""},
        {"\"id\":\"room:snes\",", "\"id\":\"snes\",", "/rooms/0/id", "\"id\":\"snes\""},
        {"\"maxXMm\":4500", "\"maxXMm\":-4500", "/rooms/0/maxXMm: must be greater", "\"maxXMm\":-4500"},
        {"\"growthSteps\":0", "\"growthSteps\":1001", "/rooms/0/growthSteps", "growthSteps"},
        {"\"themePack\":\"classic_16bit_warm\"", "\"themePack\":7", "/rooms/0/themePack: expected a string",
         "themePack"},
        {"\"role\":\"display\"", "\"role\":\"shelf\"", "/fixtures/0/role: expected one of", "\"role\":\"shelf\""},
        {"\"yawCdeg\":9000", "\"yawCdeg\":36000", "/fixtures/0/yawCdeg", "36000"},
        {"\"widthMm\":1200", "\"widthMm\":0", "/fixtures/0/widthMm", "\"widthMm\":0"},
        {"\"kind\":\"crt\"", "\"kind\":\"CRT\"", "/fixtures/1/prefab/kind", "CRT"},
        {"\"manifest\":\"electronics.crt.consumer.generic\"", "\"manifest\":\"crt\"", "/fixtures/1/prefab/manifest",
         "\"manifest\":\"crt\""},
        {"{\"level\":0,\"section\":\"spine\",\"slots\":33},{\"level\":4,\"section\":\"band\",\"slots\":6}",
         "{\"level\":4,\"section\":\"band\",\"slots\":6},{\"level\":0,\"section\":\"spine\",\"slots\":33}",
         "/fixtures/2/levels/1: out of order", "hero_module"},
        {"\"section\":\"featured\",\"slots\":11", "\"section\":\"featured\",\"slots\":0", "/fixtures/0/levels/0/slots",
         "\"slots\":0"},
        {"\"section\":\"featured\"", "\"section\":\"face\"", "/fixtures/0/levels/0/section", "face"},
        {"room:snes/fx:hw-w/s:0.1", "room:snes/fx:hw-w/s:00.1", "/placements/1/slot: expected a slot id", "s:00.1"},
        {"\"game\":\"g0000000000000002\"", "\"game\":\"g0000000000000000\"", "/placements/0/game", "g0000000000000000"},
        {"\"insetMm\":3", "\"insetMm\":-3", "/placements/1/insetMm", "\"insetMm\":-3"},
        {"\"count\":31", "\"count\":0", "/dressing/0/count", "\"count\":0"},
        {"\"firstSlot\":2", "\"firstSlot\":9999", "/dressing/0/count: the slots run past", "firstSlot"},
        {"room:snes/fx:hw-w/l:0", "room:snes/fx:hw-w/l:x", "/dressing/0/level", "l:x"},
        {"\"reason\":\"no-cover\",", "\"reason\":\"no-cover\",\"representative\":\"g0000000000000001\",",
         "/unplaced/1/representative: only a duplicate-title", "no-cover"},
        {"\"representative\":\"g0000000000000001\",", "", "/unplaced/0/representative: missing", "duplicate-title"},
        {"\"reason\":\"no-cover\"", "\"reason\":\"hidden\"", "/unplaced/1/reason", "hidden"},
        // Schema 2's fields (I4 step 1).
        {"{\"id\":\"ground\",\"order\":0},\n{\"id\":\"gen4\",\"order\":3}",
         "{\"id\":\"gen4\",\"order\":3},\n{\"id\":\"ground\",\"order\":0}", "/floors/1: out of order",
         "\"ground\""},
        {"{\"id\":\"gen4\",\"order\":3}", "{\"id\":\"ground\",\"order\":3}", "/floors/1/id: appears twice",
         "\"order\":3"},
        {"{\"id\":\"gen4\",\"order\":3}", "{\"id\":\"Gen4\",\"order\":3}", "/floors/1/id", "Gen4"},
        {"\"kind\":\"console\"", "\"kind\":\"hall\"", "/rooms/0/kind: expected one of", "hall"},
        {"\"kind\":\"console\"", "\"kind\":\"shared\"", "/rooms/0/kind: a shared room's id is not room:snes",
         "\"kind\":\"shared\""},
        {"\"sections\":[\"snes\"]", "\"sections\":[\"nes\"]", "/rooms/0/sections: a console room holds exactly",
         "\"nes\""},
        {"\"sections\":[\"snes\"]", "\"sections\":[]", "/rooms/0/sections: names no system", "\"sections\":[]"},
        {"\"sections\":[\"snes\"]", "\"sections\":[\"snes\",\"snes\"]", "/rooms/0/sections/1: appears twice",
         "\"sections\""},
        {"\"floor\":\"gen4\",", "\"floor\":\"\",", "/rooms/0/floor", "\"floor\":\"\""},
        {"\"yawHalfTurns\":0", "\"yawHalfTurns\":2", "/rooms/0/placement/yawHalfTurns", "\"yawHalfTurns\":2"},
        {"{\"floor\":\"gen4\",", "{\"expands\":\"room:snes\",\"floor\":\"gen4\",",
         "/rooms/0/expands: an expansion, and only one", "expands"},
        {"\"floor\":\"gen4\",\"generatorVersion\":\"1.0\"", "\"floor\":\"gen4\",\"generatorVersion\":\"1.1\"",
         "/generatorVersion: the plan's generator version is the highest of its rooms', 1.1", "\"inputDigest\""},
    };
    for (const Case& test : cases) {
        INFO(test.to);
        const std::string text = replaced(kSampleText, test.from, test.to);
        const rv::Error error = refusal(text);
        CHECK(error.code == rv::ErrorCode::ValidationFailed);
        CHECK_THAT(error.message, ContainsSubstring(std::string(test.message)));
        CHECK(error.context == "world-plan.json:" + std::to_string(lineOf(text, test.lineOf)));
    }
}

TEST_CASE("world/plan_json: lists out of order or holding a key twice are refused", "[world][plan_json]") {
    const std::string first =
        "{\"game\":\"g0000000000000002\",\"insetMm\":0,\"slot\":\"room:snes/fx:dc01/s:0.0\",\"yawCdeg\":120}";
    const std::string second =
        "{\"game\":\"g0000000000000001\",\"insetMm\":3,\"slot\":\"room:snes/fx:hw-w/s:0.1\",\"yawCdeg\":0}";

    const std::string swapped = replaced(kSampleText, first + ",\n" + second, second + ",\n" + first);
    const rv::Error outOfOrder = refusal(swapped);
    CHECK_THAT(outOfOrder.message, ContainsSubstring("/placements/1: out of order: placements are sorted by slot"));
    CHECK(outOfOrder.context == "world-plan.json:12");

    // The same slot and game twice, even with another offset.
    const std::string again = replaced(second, "\"insetMm\":3", "\"insetMm\":4");
    const rv::Error twice = refusal(replaced(kSampleText, second, second + ",\n" + again));
    CHECK_THAT(twice.message, ContainsSubstring("/placements/2: appears twice"));
    CHECK(twice.context == "world-plan.json:13");

    // Two games in one slot are a plan the reader reads: the validation (I3 step 4) refuses it.
    const std::string shared = replaced(second, "g0000000000000001", "g0000000000000005");
    auto twoGames = rv::world::readPlan(replaced(kSampleText, second, second + ",\n" + shared), "world-plan.json");
    REQUIRE(twoGames.has_value());
    CHECK(twoGames->placements.size() == 3);

    // A fixture twice.
    const std::size_t fixtureStart = kSampleText.find("{\"depthMm\":560");
    const std::size_t fixtureEnd = kSampleText.find('\n', fixtureStart);
    const std::string crtLine = kSampleText.substr(fixtureStart, fixtureEnd - fixtureStart);
    const rv::Error fixtureTwice = refusal(replaced(kSampleText, crtLine, crtLine + "\n" + crtLine));
    CHECK_THAT(fixtureTwice.message, ContainsSubstring("/fixtures/2: appears twice"));
}

TEST_CASE("world/plan_json: the writer refuses what the reader would", "[world][plan_json]") {
    WorldPlan noRoomId = samplePlan();
    noRoomId.rooms.front().id = RoomId{};
    auto refused = rv::world::writePlan(noRoomId);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == rv::ErrorCode::InvalidArgument);
    CHECK_THAT(refused.error().message, ContainsSubstring("/rooms/0/id"));

    WorldPlan fixtureTwice = samplePlan();
    fixtureTwice.fixtures.push_back(fixtureTwice.fixtures.front());
    refused = rv::world::writePlan(fixtureTwice);
    REQUIRE_FALSE(refused.has_value());
    CHECK_THAT(refused.error().message, ContainsSubstring("appears twice"));

    WorldPlan stray = samplePlan();
    stray.unplaced.front().representative = game(9); // on a no-cover entry
    refused = rv::world::writePlan(stray);
    REQUIRE_FALSE(refused.has_value());
    CHECK_THAT(refused.error().message, ContainsSubstring("only a duplicate-title"));

    WorldPlan noRepresentative = samplePlan();
    noRepresentative.unplaced.back().representative = GameId{};
    CHECK_FALSE(rv::world::writePlan(noRepresentative).has_value());

    WorldPlan badDigest = samplePlan();
    badDigest.inputDigest = "abc";
    CHECK_FALSE(rv::world::planToJson(badDigest).has_value());
    CHECK_FALSE(rv::world::planDigest(badDigest).has_value());

    WorldPlan newer = samplePlan();
    newer.schemaVersion = 3;
    CHECK_FALSE(rv::world::writePlan(newer).has_value());
    WorldPlan older = samplePlan();
    older.schemaVersion = 1;
    CHECK_FALSE(rv::world::writePlan(older).has_value());
}

TEST_CASE("world/plan_json: generator versions and enumerations as text", "[world][plan_json]") {
    CHECK(rv::world::GeneratorVersion{12, 3}.toString() == "12.3");
    CHECK(*rv::world::GeneratorVersion::parse("0.0") == rv::world::GeneratorVersion{0, 0});
    for (const std::string_view text : {"", "1", "1.", ".1", "01.0", "1.00", "1.0.0", "-1.0", "1000000.0"}) {
        INFO(text);
        CHECK_FALSE(rv::world::GeneratorVersion::parse(text).has_value());
    }
    CHECK(rv::world::GeneratorVersion{1, 9} < rv::world::GeneratorVersion{2, 0});
    for (const FixtureRole role : {FixtureRole::HeroModule, FixtureRole::SideModule, FixtureRole::Display,
                                   FixtureRole::HeroCorner, FixtureRole::Decoration}) {
        CHECK(rv::world::parseFixtureRole(rv::world::toString(role)) == role);
    }
    for (const UnplacedReason reason : {UnplacedReason::DuplicateTitle, UnplacedReason::NoCover,
                                        UnplacedReason::Unavailable, UnplacedReason::Capacity,
                                        UnplacedReason::RoomKept}) {
        CHECK(rv::world::parseUnplacedReason(rv::world::toString(reason)) == reason);
    }
    CHECK(rv::world::toString(UnplacedReason::RoomKept) == "room-kept");
    for (const rv::world::RoomKind kind :
         {rv::world::RoomKind::Console, rv::world::RoomKind::Shared, rv::world::RoomKind::Arcade}) {
        CHECK(rv::world::parseRoomKind(rv::world::toString(kind)) == kind);
    }
    CHECK(rv::world::parseSection("band") == Section::Band);
    CHECK(rv::world::isFaceOut(Section::Featured));
    CHECK(rv::world::isFaceOut(Section::Band));
    CHECK_FALSE(rv::world::isFaceOut(Section::Spine));
    for (const LightRole role :
         {LightRole::Key, LightRole::Pendant, LightRole::Lamp, LightRole::Picture, LightRole::WallWasher}) {
        CHECK(rv::world::parseLightRole(rv::world::toString(role)) == role);
    }
    CHECK(rv::world::lightKind(LightRole::Pendant) == rv::world::LightKind::Point);
    CHECK(rv::world::lightKind(LightRole::Lamp) == rv::world::LightKind::Point);
    CHECK(rv::world::lightKind(LightRole::Key) == rv::world::LightKind::Spot);
    CHECK(rv::world::lightKind(LightRole::WallWasher) == rv::world::LightKind::Spot);
}

TEST_CASE("world/plan_json: lights are written by id and read back; a point light has no aim", "[world][plan_json]") {
    WorldPlan plan = samplePlan();
    const PlanLight washer{fixtureId("w01", "washer"), LightRole::WallWasher, -3270, 2950, -3500, -4230, 1000, -3500,
                           1900};
    const PlanLight pendant{fixtureId("room", "pendant"), LightRole::Pendant, 0, 2100, 0, 0, 2100, 0, 0};
    plan.lights = {washer, pendant};
    auto text = rv::world::writePlan(plan);
    REQUIRE(text.has_value());
    CHECK_THAT(*text, ContainsSubstring("\"lights\":[\n"
                                        "{\"id\":\"room:snes/fx:room.pendant\",\"role\":\"pendant\",\"spanMm\":0,"
                                        "\"targetXMm\":0,\"targetYMm\":2100,\"targetZMm\":0,\"xMm\":0,\"yMm\":2100,"
                                        "\"zMm\":0},\n"
                                        "{\"id\":\"room:snes/fx:w01.washer\",\"role\":\"wall-washer\",\"spanMm\":1900,"
                                        "\"targetXMm\":-4230,\"targetYMm\":1000,\"targetZMm\":-3500,\"xMm\":-3270,"
                                        "\"yMm\":2950,\"zMm\":-3500}\n"
                                        "],\"placements\":["));
    auto read = rv::world::readPlan(*text, "lights.json");
    REQUIRE(read.has_value());
    CHECK(read->lights == std::vector<PlanLight>{pendant, washer});
    CHECK(*rv::world::writePlan(*read) == *text);

    // A point light aimed elsewhere or given a span, an unknown role, the list out of order: refused.
    PlanLight aimed = pendant;
    aimed.targetZMm = 10;
    plan.lights = {aimed};
    CHECK_FALSE(rv::world::writePlan(plan).has_value());
    PlanLight spread = pendant;
    spread.spanMm = 5;
    plan.lights = {spread};
    CHECK_FALSE(rv::world::writePlan(plan).has_value());
    const rv::Error role = refusal(replaced(*text, "\"role\":\"pendant\"", "\"role\":\"torch\""));
    CHECK_THAT(role.toString(), ContainsSubstring("/lights/0/role"));
    const rv::Error order =
        refusal(replaced(replaced(*text, "fx:room.pendant", "fx:zz.pendant"), "fx:w01.washer", "fx:room.washer"));
    CHECK_THAT(order.toString(), ContainsSubstring("lights are sorted by id"));
}
