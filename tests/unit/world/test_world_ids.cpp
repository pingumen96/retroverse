// The world's identifiers (rv/world/ids.hpp, ADR-0010 §7, ADR-0013 §5): their text forms (the four forms of a room
// id among them, with hostile system names), read back exactly, the malformed ones
// refused, and their order (by parts, numbers as numbers).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <rv/world/ids.hpp>

using rv::world::FixtureId;
using rv::world::LevelId;
using rv::world::RoomId;
using rv::world::SlotId;

namespace {

RoomId room(std::string_view system = "snes") {
    auto name = rv::catalog::SystemName::parse(system);
    REQUIRE(name.has_value());
    auto id = RoomId::forSystem(*name);
    REQUIRE(id.has_value());
    return *id;
}

FixtureId fixture(std::string_view socket, std::string_view part = {}) {
    auto id = FixtureId::make(room(), socket, part);
    REQUIRE(id.has_value());
    return *id;
}

} // namespace

TEST_CASE("world/ids: socket names, data ids and rule names", "[world][ids]") {
    for (const std::string_view name :
         {"w03", "e01", "hero", "dc01", "door-e", "hw-w", "a", "abcdefghijklmnopqrstuvwxyz012345"}) {
        INFO(name);
        CHECK(rv::world::isValidSocketName(name));
    }
    for (const std::string_view name :
         {"", "W03", "3w", "-w", "w_03", "w.03", "w/03", "w 03", "wé", "abcdefghijklmnopqrstuvwxyz0123456"}) {
        INFO(name);
        CHECK_FALSE(rv::world::isValidSocketName(name));
    }
    CHECK(rv::world::isValidDataId("console_room_9x10"));
    CHECK(rv::world::isValidDataId("cartridge_home_16bit"));
    CHECK_FALSE(rv::world::isValidDataId("Console"));
    CHECK_FALSE(rv::world::isValidDataId("9x10"));
    CHECK_FALSE(rv::world::isValidDataId("door-e"));
    CHECK_FALSE(rv::world::isValidDataId(""));
    CHECK(rv::world::isValidPrefabKind("tv_cabinet"));
    CHECK_FALSE(rv::world::isValidPrefabKind("tv-cabinet"));
    CHECK(rv::world::isValidRuleName("splitmix64/1"));
    CHECK(rv::world::isValidRuleName("title-key/12"));
    for (const std::string_view rule :
         {"splitmix64", "splitmix64/0", "splitmix64/01", "/1", "Split/1", "a/1/2", "a/"}) {
        INFO(rule);
        CHECK_FALSE(rv::world::isValidRuleName(rule));
    }
}

TEST_CASE("world/ids: every id's text is read back as the same id", "[world][ids]") {
    const RoomId snes = room();
    CHECK(snes.toString() == "room:snes");
    CHECK(snes.system().str() == "snes");

    const FixtureId w03 = fixture("w03");
    CHECK(w03.toString() == "room:snes/fx:w03");
    CHECK(w03.localText() == "fx:w03");
    const FixtureId crt = fixture("hero", "crt");
    CHECK(crt.toString() == "room:snes/fx:hero.crt");
    CHECK(crt.localText() == "fx:hero.crt");
    CHECK(crt.socket() == "hero");
    CHECK(crt.part() == "crt");

    auto level = LevelId::make(w03, 2);
    REQUIRE(level.has_value());
    CHECK(level->toString() == "room:snes/fx:w03/l:2");
    CHECK(level->localText() == "fx:w03/l:2");

    auto slot = SlotId::make(w03, 3, 12);
    REQUIRE(slot.has_value());
    CHECK(slot->toString() == "room:snes/fx:w03/s:3.12");
    CHECK(slot->levelId() == *LevelId::make(w03, 3));

    // A system name may hold what ES-DE allows in a folder name (spaces, letters beyond ASCII): the room's id keeps it.
    const RoomId spaced = room("Arcade Plus é");
    CHECK(spaced.toString() == "room:Arcade Plus é");
    CHECK(*RoomId::parse(spaced.toString()) == spaced);

    CHECK(*RoomId::parse("room:snes") == snes);
    CHECK(*FixtureId::parse("room:snes/fx:w03") == w03);
    CHECK(*FixtureId::parse("room:snes/fx:hero.crt") == crt);
    CHECK(*LevelId::parse("room:snes/fx:w03/l:2") == *level);
    CHECK(*SlotId::parse("room:snes/fx:w03/s:3.12") == *slot);
    CHECK(*SlotId::parse("room:snes/fx:hero.crt/s:0.0") == *SlotId::make(crt, 0, 0));
    CHECK(*SlotId::parse("room:snes/fx:w03/s:999.9999") == *SlotId::make(w03, 999, 9999));
}

TEST_CASE("world/ids: malformed ids are refused", "[world][ids]") {
    for (const std::string_view text :
         {"", "room:", "snes", "Room:snes", "room:sn/es", "room:snes/", "room: snes", "room:snes "}) {
        INFO(text);
        CHECK_FALSE(RoomId::parse(text).has_value());
    }
    for (const std::string_view text : {"room:snes", "room:snes/fx:", "room:snes/fx:W03", "room:snes/fx:w03.",
                                        "room:snes/fx:.crt", "room:snes/fx:hero.crt.x", "room:snes/fx:w03/",
                                        "room:snes/fx:w03/s:1.2", "room:snes/f:w03", "fx:w03", "room:snes/fx:w03 "}) {
        INFO(text);
        CHECK_FALSE(FixtureId::parse(text).has_value());
    }
    for (const std::string_view text :
         {"room:snes/fx:w03/l:", "room:snes/fx:w03/l:02", "room:snes/fx:w03/l:-1", "room:snes/fx:w03/l:1000",
          "room:snes/fx:w03/l:1.0", "room:snes/fx:w03/s:1.0", "room:snes/l:1"}) {
        INFO(text);
        CHECK_FALSE(LevelId::parse(text).has_value());
    }
    for (const std::string_view text :
         {"room:snes/fx:w03/s:", "room:snes/fx:w03/s:1", "room:snes/fx:w03/s:1.", "room:snes/fx:w03/s:.1",
          "room:snes/fx:w03/s:01.1", "room:snes/fx:w03/s:1.01", "room:snes/fx:w03/s:1000.0",
          "room:snes/fx:w03/s:0.10000", "room:snes/fx:w03/s:1.2.3", "room:snes/fx:w03/s:+1.2", "room:snes/fx:w03/l:1",
          "room:snes/fx:w03/s:1.2/s:1.2"}) {
        INFO(text);
        CHECK_FALSE(SlotId::parse(text).has_value());
    }
    CHECK_FALSE(RoomId::forSystem(rv::catalog::SystemName{}).has_value());
    CHECK_FALSE(FixtureId::make(RoomId{}, "w03").has_value());
    CHECK_FALSE(FixtureId::make(room(), "W03").has_value());
    CHECK_FALSE(FixtureId::make(room(), "hero", "Crt").has_value());
    CHECK_FALSE(LevelId::make(FixtureId{}, 0).has_value());
    CHECK_FALSE(LevelId::make(fixture("w03"), 1000).has_value());
    CHECK_FALSE(SlotId::make(fixture("w03"), 0, 10000).has_value());
}

TEST_CASE("world/ids: ids order by their parts, numbers as numbers", "[world][ids]") {
    const FixtureId w03 = fixture("w03");
    std::vector<SlotId> slots = {*SlotId::make(w03, 10, 0), *SlotId::make(w03, 2, 10), *SlotId::make(w03, 2, 9),
                                 *SlotId::make(fixture("e01"), 5, 5)};
    std::sort(slots.begin(), slots.end());
    std::vector<std::string> texts;
    for (const SlotId& slot : slots) {
        texts.push_back(slot.toString());
    }
    CHECK(texts == std::vector<std::string>{"room:snes/fx:e01/s:5.5", "room:snes/fx:w03/s:2.9",
                                            "room:snes/fx:w03/s:2.10", "room:snes/fx:w03/s:10.0"});
    // A socket's only fixture before its parts.
    CHECK(fixture("hero") < fixture("hero", "crt"));
    CHECK(fixture("hero", "crt") < fixture("hero", "rug"));
    CHECK(room("nes") < room("snes"));
    // Empty ids print as nothing.
    CHECK(FixtureId{}.toString().empty());
    CHECK(SlotId{}.toString().empty());
    CHECK(SlotId{}.levelId().empty());
}

TEST_CASE("world/ids: the room ids' four forms, read back exactly", "[world][ids]") {
    // ADR-0013 §5: room:<system>, room:<system>:<n> (n from 2), area:shared:<floor>[:<n>], area:arcade[:<n>].
    struct Case {
        std::string_view text;
        rv::world::RoomIdKind kind;
        std::string_view system;
        std::string_view floor;
        std::uint32_t index;
    };
    const Case cases[] = {
        {"room:snes", rv::world::RoomIdKind::System, "snes", "", 1},
        {"room:snes:2", rv::world::RoomIdKind::System, "snes", "", 2},
        {"room:snes:999", rv::world::RoomIdKind::System, "snes", "", 999},
        {"area:shared:gen4", rv::world::RoomIdKind::Shared, "", "gen4", 1},
        {"area:shared:gen4:3", rv::world::RoomIdKind::Shared, "", "gen4", 3},
        {"area:shared:computer", rv::world::RoomIdKind::Shared, "", "computer", 1},
        {"area:arcade", rv::world::RoomIdKind::Arcade, "", "", 1},
        {"area:arcade:2", rv::world::RoomIdKind::Arcade, "", "", 2},
        // Hostile system names that are still ES-DE system names: spaces, dots, Unicode, a name like a form.
        {"room:Arcade Plus é", rv::world::RoomIdKind::System, "Arcade Plus é", "", 1},
        {"room:a.b", rv::world::RoomIdKind::System, "a.b", "", 1},
        {"room:area", rv::world::RoomIdKind::System, "area", "", 1},
        {"room:fx", rv::world::RoomIdKind::System, "fx", "", 1},
        {"room:🎮 games:7", rv::world::RoomIdKind::System, "🎮 games", "", 7},
    };
    for (const Case& test : cases) {
        INFO(test.text);
        auto parsed = RoomId::parse(test.text);
        REQUIRE(parsed.has_value());
        CHECK(parsed->kind() == test.kind);
        CHECK(parsed->system().str() == test.system);
        CHECK(parsed->floor() == test.floor);
        CHECK(parsed->index() == test.index);
        CHECK(parsed->isExpansion() == (test.index > 1));
        CHECK(parsed->toString() == test.text);
        CHECK(parsed->head().index() == 1);
        // Fixtures, levels and slots of every form.
        auto slot = SlotId::make(*FixtureId::make(*parsed, "w03"), 3, 12);
        REQUIRE(slot.has_value());
        CHECK(slot->toString() == std::string(test.text) + "/fx:w03/s:3.12");
        CHECK(*SlotId::parse(slot->toString()) == *slot);
        CHECK(LevelId::parse(std::string(test.text) + "/fx:w03/l:2")->fixture().room() == *parsed);
    }
    // The makers give the same ids; forSystem() keeps the SNES room's id.
    CHECK(room().toString() == "room:snes");
    CHECK(*RoomId::forSystem(*rv::catalog::SystemName::parse("snes"), 1) == room());
    CHECK(RoomId::forSystem(*rv::catalog::SystemName::parse("snes"), 2)->toString() == "room:snes:2");
    CHECK(RoomId::forSystem(*rv::catalog::SystemName::parse("snes"), 2)->head() == room());
    CHECK(RoomId::sharedArea("gen4")->toString() == "area:shared:gen4");
    CHECK(RoomId::sharedArea("gen4", 2)->toString() == "area:shared:gen4:2");
    CHECK(RoomId::arcadeArea()->toString() == "area:arcade");
    CHECK(RoomId::arcadeArea(3)->toString() == "area:arcade:3");
    CHECK_FALSE(RoomId::forSystem(*rv::catalog::SystemName::parse("snes"), 0).has_value());
    CHECK_FALSE(RoomId::forSystem(*rv::catalog::SystemName::parse("snes"), 1000).has_value());
    CHECK_FALSE(RoomId::sharedArea("Gen4").has_value());
    CHECK_FALSE(RoomId::sharedArea("gen4", 0).has_value());
    CHECK_FALSE(RoomId::arcadeArea(0).has_value());
    CHECK(RoomId{}.empty());
    CHECK(RoomId{}.toString().empty());
}

TEST_CASE("world/ids: malformed room ids of every form are refused", "[world][ids]") {
    for (const std::string_view text :
         {"room:snes:", "room:snes:0", "room:snes:1", "room:snes:01", "room:snes:02", "room:snes:1000",
          "room:snes:2:3", "room:snes:x", "room:snes:-2", "room:snes: 2", "room::2", "area:", "area", "area:shared",
          "area:shared:", "area:shared:Gen4", "area:shared:gen 4", "area:shared:gen4:", "area:shared:gen4:1",
          "area:shared:gen4:01", "area:shared:gen4:2:2", "area:shared:4gen", "area:arcade:", "area:arcade:1",
          "area:arcade:01", "area:arcade:x", "area:arcadex", "area:arcade:shared", "area:lobby", "Area:arcade",
          "area:arcade ", " area:arcade", "room:sn:es", "room:a\b", "room:.", "room:..", "room:snes."}) {
        INFO(text);
        CHECK_FALSE(RoomId::parse(text).has_value());
    }
    for (const std::string_view text : {"area:shared:gen4/fx:", "area:arcade/fx:W03", "room:snes:1/fx:w03",
                                        "area:arcade:1/fx:w03", "area:shared:/fx:w03"}) {
        INFO(text);
        CHECK_FALSE(FixtureId::parse(text).has_value());
    }
}

TEST_CASE("world/ids: room ids order by form, then floor or system, then index", "[world][ids]") {
    std::vector<RoomId> rooms = {*RoomId::parse("room:snes:10"), *RoomId::parse("room:snes"),
                                 *RoomId::parse("area:shared:gen4"), *RoomId::parse("room:snes:2"),
                                 *RoomId::parse("area:arcade:2"), *RoomId::parse("area:arcade"),
                                 *RoomId::parse("room:nes"), *RoomId::parse("area:shared:gen3:2"), RoomId{}};
    std::sort(rooms.begin(), rooms.end());
    std::vector<std::string> texts;
    for (const RoomId& id : rooms) {
        texts.push_back(id.toString());
    }
    CHECK(texts == std::vector<std::string>{"", "area:arcade", "area:arcade:2", "area:shared:gen3:2",
                                            "area:shared:gen4", "room:nes", "room:snes", "room:snes:2",
                                            "room:snes:10"});
}
