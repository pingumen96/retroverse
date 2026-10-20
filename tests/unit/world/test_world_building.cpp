// The building's data and floor layout (rv/world/building.hpp, ADR-0013 §1, §2, §7; I4 step 1): building.json and
// systems.json as shipped and what their readers refuse, the classification of systems (their own row, the platform-tag
// fallback, "other", "excluded") with their floors, the placement arithmetic (the shorter row, north on a tie, full
// rows, the lobby's width, z following a room's depth while x stays, the room's frame on its floor's), and the seeds of
// every form of room id.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <rv/core/file_io.hpp>
#include <rv/core/seed.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>
#include <rv/world/building.hpp>
#include <rv/world/seeds.hpp>

using Catch::Matchers::ContainsSubstring;
using rv::world::BuildingData;
using rv::world::Classification;
using rv::world::ClassificationSource;
using rv::world::Footprint;
using rv::world::Room;
using rv::world::RoomPlacement;
using rv::world::SystemKind;
using rv::world::SystemsData;

namespace {

const std::filesystem::path& assets() {
    static const std::filesystem::path root = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);
    return root;
}

const BuildingData& building() {
    static const BuildingData data = [] {
        auto loaded = rv::world::loadBuildingData(assets());
        REQUIRE(loaded.has_value());
        return *loaded;
    }();
    return data;
}

const SystemsData& systems() {
    static const SystemsData data = [] {
        auto loaded = rv::world::loadSystemsData(assets());
        REQUIRE(loaded.has_value());
        return *loaded;
    }();
    return data;
}

rv::catalog::SystemName systemName(std::string_view name) {
    auto parsed = rv::catalog::SystemName::parse(name);
    REQUIRE(parsed.has_value());
    return *parsed;
}

/// A room of the base template's interior (9 x 10 m) on `floor`, `growth` steps of 2 m deeper.
Room roomOn(std::string_view floor, std::string_view name = "snes", std::uint32_t growth = 0) {
    Room room;
    room.id = *rv::world::RoomId::forSystem(systemName(name));
    room.floor = floor;
    room.minXMm = -4500;
    room.maxXMm = 4500;
    room.minZMm = -5000;
    room.maxZMm = 5000 + 2000 * static_cast<std::int64_t>(growth);
    room.growthSteps = growth;
    return room;
}

/// Places `room` on its floor among `rooms` and adds it.
Room enter(std::vector<Room>& rooms, Room room) {
    const auto placement =
        rv::world::placeEnteringRoom(room, rv::world::floorRows(rooms, room.floor, building().layout),
                                     building().layout);
    REQUIRE(placement.has_value());
    room.placement = *placement;
    rooms.push_back(room);
    return room;
}

rv::Error buildingRefusal(const std::string& text) {
    auto data = rv::world::parseBuildingData(text, "building.json");
    REQUIRE_FALSE(data.has_value());
    return data.error();
}

rv::Error systemsRefusal(const std::string& text) {
    auto data = rv::world::parseSystemsData(text, "systems.json");
    REQUIRE_FALSE(data.has_value());
    return data.error();
}

} // namespace

TEST_CASE("world/building: building.json and systems.json as shipped", "[world][building]") {
    const BuildingData& data = building();
    std::vector<std::string> floors;
    for (const rv::world::FloorSpec& floor : data.floors) {
        floors.push_back(floor.id);
    }
    // ADR-0013 §1, in the elevator's order.
    CHECK(floors == std::vector<std::string>{"ground", "gen2", "gen3", "gen4", "gen5", "gen6", "gen7", "handheld",
                                             "computer"});
    CHECK(*data.order("ground") == 0);
    CHECK(*data.order("gen4") == 3);
    CHECK(*data.order("computer") == 8);
    CHECK_FALSE(data.order("attic").has_value());
    CHECK(data.planFloor("gen4") == rv::world::Floor{"gen4", 3});
    CHECK(data.layout == rv::world::BuildingLayout{3000, 6000, 3000, 150, 12});
    // Generations 1 and 2 on gen2, 7 to 9 on gen7; every handheld on handheld; computers and "other" together.
    CHECK(data.floorFor(SystemKind::Home, 1)->id == "gen2");
    CHECK(data.floorFor(SystemKind::Home, 2)->id == "gen2");
    CHECK(data.floorFor(SystemKind::Home, 3)->id == "gen3");
    CHECK(data.floorFor(SystemKind::Home, 6)->id == "gen6");
    for (const std::uint32_t generation : {7u, 8u, 9u}) {
        CHECK(data.floorFor(SystemKind::Home, generation)->id == "gen7");
    }
    for (std::uint32_t generation = 1; generation <= 9; ++generation) {
        CHECK(data.floorFor(SystemKind::Handheld, generation)->id == "handheld");
    }
    CHECK(data.floorFor(SystemKind::Arcade, 0)->id == "ground");
    CHECK(data.floorFor(SystemKind::Computer, 0)->id == "computer");
    CHECK(data.floorFor(SystemKind::Other, 0)->id == "computer");
    CHECK(data.floorFor(SystemKind::Excluded, 0) == nullptr);

    // systems.json: the SNES as I3 knew it, the defaults per kind, the excluded systems.
    const rv::world::SystemEntry* snes = systems().find(systemName("snes"));
    REQUIRE(snes != nullptr);
    CHECK(snes->kind == SystemKind::Home);
    CHECK(snes->generation == 4);
    CHECK(snes->family == "cartridge_home_16bit");
    CHECK(snes->platforms == std::vector<std::string>{"snes"});
    CHECK(systems().defaultsOf(SystemKind::Home).console.has_value());
    CHECK_FALSE(systems().defaultsOf(SystemKind::Arcade).console.has_value());
    CHECK_FALSE(systems().defaultsOf(SystemKind::Computer).console.has_value());
    CHECK_FALSE(systems().defaultsOf(SystemKind::Other).console.has_value());
    CHECK(systems().fallback.family == "generic_case");
    for (const std::string_view excluded : {"desktop", "emulators", "kodi"}) {
        REQUIRE(systems().find(systemName(excluded)) != nullptr);
        CHECK(systems().find(systemName(excluded))->kind == SystemKind::Excluded);
    }
    // Every system of the table lands on a floor, but the excluded ones.
    for (const rv::world::SystemEntry& entry : systems().systems) {
        INFO(entry.system.str());
        const Classification classification = rv::world::classify(entry.system, {}, systems(), data);
        CHECK(classification.hasRoom() == (entry.kind != SystemKind::Excluded));
    }
}

TEST_CASE("world/building: systems.json classifies every system of ES-DE 3.4.1", "[world][building]") {
    // tests/fixtures/world/esde-3.4.1-systems.txt: the names of es_systems.xml's 195 systems (I4 step 2).
    auto text = rv::core::readTextFile(rv::core::pathFromUtf8(RV_TEST_SOURCE_DIR) / "tests" / "fixtures" / "world" /
                                       "esde-3.4.1-systems.txt");
    REQUIRE(text.has_value());
    std::set<std::string> esde;
    for (const std::string_view line : rv::core::split(*text, '\n')) {
        if (!line.empty() && line.front() != '#') {
            esde.insert(std::string(line));
        }
    }
    CHECK(esde.size() == 195);
    std::set<std::string> table;
    std::map<SystemKind, std::size_t> kinds;
    for (const rv::world::SystemEntry& entry : systems().systems) {
        table.insert(entry.system.str());
        ++kinds[entry.kind];
    }
    // Total over ES-DE's systems, and nothing else.
    CHECK(table == esde);
    CHECK(kinds[SystemKind::Home] == 69);
    CHECK(kinds[SystemKind::Handheld] == 24);
    CHECK(kinds[SystemKind::Arcade] == 24);
    CHECK(kinds[SystemKind::Computer] == 44);
    CHECK(kinds[SystemKind::Other] == 31);
    CHECK(kinds[SystemKind::Excluded] == 3);
    std::uint32_t previousFloor = 0;
    for (const std::string& name : esde) {
        INFO(name);
        const Classification classification = rv::world::classify(systemName(name), {}, systems(), building());
        CHECK(classification.source == ClassificationSource::Table);
        if (classification.kind() == SystemKind::Excluded) {
            CHECK_FALSE(classification.hasRoom());
            continue;
        }
        // A floor and a generic sign (test_world_families.cpp checks the families).
        CHECK(classification.hasRoom());
        CHECK_FALSE(classification.entry.sign.empty());
    }
    // Signs are generic texts, never a trade name (ADR-0013 §1, style §17.2): none names a maker, a console or a
    // trademarked format.
    constexpr std::array<std::string_view, 42> kTradeNames{
        "LASERDISC", "NINTENDO",  "SEGA",     "SONY",          "PLAYSTATION", "ATARI",      "COMMODORE",
        "AMIGA",     "WORKBENCH", "GAME BOY", "GAMEBOY",       "XBOX",        "NEO GEO",    "NEOGEO",
        "SNES",      "FAMICOM",   "GENESIS",  "MEGA DRIVE",    "DREAMCAST",   "WINDOWS",    "MS-DOS",
        "MSX",       "SPECTRUM",  "COLECO",   "INTELLIVISION", "VECTREX",     "TURBOGRAFX", "PC ENGINE",
        "3DO",       "JAGUAR",    "LYNX",     "WONDERSWAN",    "BANDAI",      "SNK",        "CAPCOM",
        "STEAM",     "ANDROID",   "PIONEER",  "PHILIPS",       "APPLE",       "MACINTOSH",  "SATURN"};
    for (const rv::world::SystemEntry& entry : systems().systems) {
        for (const std::string_view name : kTradeNames) {
            INFO(entry.system.str() << "'s sign \"" << entry.sign << "\"");
            CHECK(entry.sign.find(name) == std::string::npos);
        }
    }
    // The rows stand in the floors' order (the platform-tag fallback takes the first match), the excluded ones last.
    for (const rv::world::SystemEntry& entry : systems().systems) {
        INFO(entry.system.str());
        const Classification classification = rv::world::classify(entry.system, {}, systems(), building());
        const std::uint32_t floor = classification.hasRoom() ? *building().order(classification.floor)
                                                             : static_cast<std::uint32_t>(building().floors.size());
        CHECK(floor >= previousFloor);
        previousFloor = floor;
    }
    // ADR-0013's examples: the floors of a few systems.
    const std::vector<std::pair<std::string_view, std::string_view>> floors = {
        {"mame", "ground"},  {"atari2600", "gen2"}, {"nes", "gen3"},       {"snes", "gen4"},     {"psx", "gen5"},
        {"ps2", "gen6"},     {"wii", "gen7"},       {"switch", "gen7"},    {"gb", "handheld"},   {"psp", "handheld"},
        {"amiga", "computer"}, {"c64", "computer"}, {"scummvm", "computer"}, {"neogeo", "gen4"}};
    for (const auto& [system, floor] : floors) {
        INFO(system);
        CHECK(rv::world::classify(systemName(system), {}, systems(), building()).floor == floor);
    }
}

TEST_CASE("world/building: classify() by the table, the platform tags, the fallback", "[world][building]") {
    struct Case {
        std::string_view system;
        std::vector<std::string> tags;
        SystemKind kind;
        std::string_view floor;
        ClassificationSource source;
        std::string_view family;
        std::string_view matched;
    };
    const std::vector<Case> cases = {
        {"snes", {"snes"}, SystemKind::Home, "gen4", ClassificationSource::Table, "cartridge_home_16bit", ""},
        // Its own row wins over its tags.
        {"snes", {"nes"}, SystemKind::Home, "gen4", ClassificationSource::Table, "cartridge_home_16bit", ""},
        {"nes", {}, SystemKind::Home, "gen3", ClassificationSource::Table, "cartridge_home_8bit", ""},
        {"n64", {}, SystemKind::Home, "gen5", ClassificationSource::Table, "cartridge_home_64bit", ""},
        {"gb", {}, SystemKind::Handheld, "handheld", ClassificationSource::Table, "cartridge_handheld", ""},
        {"nds", {}, SystemKind::Handheld, "handheld", ClassificationSource::Table, "case_handheld", ""},
        {"mame", {}, SystemKind::Arcade, "ground", ClassificationSource::Table, "generic_case", ""},
        {"amiga", {}, SystemKind::Computer, "computer", ClassificationSource::Table, "big_box", ""},
        {"pico8", {}, SystemKind::Other, "computer", ClassificationSource::Table, "generic_case", ""},
        {"desktop", {}, SystemKind::Excluded, "", ClassificationSource::Table, "", ""},
        // A custom system sharing a platform tag takes the first such system's row, in the table's order.
        {"snes-hacks", {"snes"}, SystemKind::Home, "gen4", ClassificationSource::PlatformTag, "cartridge_home_16bit",
         "snes"},
        {"my arcade", {"arcade"}, SystemKind::Arcade, "ground", ClassificationSource::PlatformTag, "generic_case",
         "arcade"},
        // The table's rows stand in the floors' order: the first "pcwindows" row is the first computer's.
        {"pc-tools", {"pcwindows"}, SystemKind::Computer, "computer", ClassificationSource::PlatformTag, "big_box",
         "windows9x"},
        {"daphne-copy", {"daphne"}, SystemKind::Arcade, "ground", ClassificationSource::PlatformTag, "generic_case",
         "daphne"},
        {"oldpc", {"zzz", "pc"}, SystemKind::Computer, "computer", ClassificationSource::PlatformTag, "big_box", "pc"},
        // No row, no shared tag: "other" with the fallback's family.
        {"mystery", {"zzz"}, SystemKind::Other, "computer", ClassificationSource::Fallback, "generic_case", ""},
        {"Unknown Système", {}, SystemKind::Other, "computer", ClassificationSource::Fallback, "generic_case", ""},
    };
    for (const Case& test : cases) {
        INFO(test.system);
        const Classification classification =
            rv::world::classify(systemName(test.system), test.tags, systems(), building());
        CHECK(classification.kind() == test.kind);
        CHECK(classification.floor == test.floor);
        CHECK(classification.source == test.source);
        CHECK(classification.entry.system == systemName(test.system));
        CHECK(classification.hasRoom() == (test.kind != SystemKind::Excluded));
        if (test.kind != SystemKind::Excluded) {
            CHECK(classification.entry.family == test.family);
        }
        CHECK(classification.matched.str() == test.matched);
        if (test.source != ClassificationSource::Table) {
            CHECK(classification.entry.platforms == test.tags);
        }
    }
    // The generations beyond the shipped table's: 1 and 2 on gen2, 7 to 9 on gen7.
    SystemsData custom = systems();
    for (const std::uint32_t generation : {1u, 2u, 7u, 8u, 9u}) {
        rv::world::SystemEntry entry;
        entry.system = systemName("home" + std::to_string(generation));
        entry.kind = SystemKind::Home;
        entry.generation = generation;
        entry.family = "cartridge_home_16bit";
        entry.sign = "TEST";
        entry.platforms = {"t"};
        custom.systems.push_back(entry);
    }
    CHECK(rv::world::classify(systemName("home1"), {}, custom, building()).floor == "gen2");
    CHECK(rv::world::classify(systemName("home2"), {}, custom, building()).floor == "gen2");
    CHECK(rv::world::classify(systemName("home7"), {}, custom, building()).floor == "gen7");
    CHECK(rv::world::classify(systemName("home9"), {}, custom, building()).floor == "gen7");
}

TEST_CASE("world/building: the readers refuse malformed data", "[world][building]") {
    const std::string floors = R"({"schemaVersion": 1, "floors": [)";
    const std::string layout =
        R"(], "layout": {"hallWidthMm": 3000, "lobbyWidthMm": 6000, "rowStartXMm": 3000, "wallAllowanceMm": 150,
            "maxRoomsPerRow": 12}})";
    const std::string complete = R"({"id": "ground", "kinds": ["arcade"]},
        {"id": "home", "kinds": ["home", "handheld"]}, {"id": "pc", "kinds": ["computer", "other"]})";
    REQUIRE(rv::world::parseBuildingData(floors + complete + layout, "building.json").has_value());
    CHECK_THAT(buildingRefusal(floors + R"({"id": "pc", "kinds": ["computer", "other"]},
        {"id": "ground", "kinds": ["arcade", "home", "handheld"]})" + layout)
                   .message,
               ContainsSubstring("the first floor is the ground floor"));
    CHECK_THAT(buildingRefusal(floors + R"({"id": "ground", "kinds": ["arcade", "home", "handheld"]})" + layout)
                   .message,
               ContainsSubstring("no floor holds computer"));
    CHECK_THAT(buildingRefusal(floors + complete + R"(, {"id": "more", "kinds": ["home"], "generations": [3]})" +
                               layout)
                   .message,
               ContainsSubstring("several floors hold home of generation 3"));
    CHECK_THAT(buildingRefusal(floors + R"({"id": "ground", "kinds": ["arcade", "excluded"]})" + layout).message,
               ContainsSubstring("expected one of home, handheld, arcade, computer, other"));
    CHECK_THAT(buildingRefusal(floors + complete + R"(, {"id": "home", "kinds": ["arcade"]})" + layout).message,
               ContainsSubstring("appears twice"));
    CHECK_THAT(buildingRefusal(floors + R"({"id": "ground", "kinds": ["arcade"], "generations": [1]})" + layout)
                   .message,
               ContainsSubstring("no kind with generations"));
    std::string odd = floors + complete + layout;
    odd.replace(odd.find("3000"), 4, "3001");
    CHECK_THAT(buildingRefusal(odd).message, ContainsSubstring("must be even"));

    const std::string head = R"({"schemaVersion": 1, "defaults": {
        "home": {"themePack": "t", "console": null, "plastic": {"cartridge": "#000000", "console": "#000000",
                 "accent": "#000000"}},
        "handheld": {"themePack": "t", "console": null, "plastic": {"cartridge": "#000000", "console": "#000000",
                     "accent": "#000000"}},
        "arcade": {"themePack": "t", "console": null, "plastic": {"cartridge": "#000000", "console": "#000000",
                   "accent": "#000000"}},
        "computer": {"themePack": "t", "console": null, "plastic": {"cartridge": "#000000", "console": "#000000",
                     "accent": "#000000"}},
        "other": {"themePack": "t", "console": null, "plastic": {"cartridge": "#000000", "console": "#000000",
                  "accent": "#000000"}}},
        "fallback": {"family": "generic_case", "order": 9, "sign": "MORE"}, "systems": [)";
    const std::string snes =
        R"({"system": "snes", "kind": "home", "generation": 4, "family": "f", "order": 1, "sign": "S",
            "platforms": ["snes"]})";
    REQUIRE(rv::world::parseSystemsData(head + snes + "]}", "systems.json").has_value());
    CHECK_THAT(systemsRefusal(head + snes + ", " + snes + "]}").message, ContainsSubstring("appears twice"));
    std::string noGeneration = snes;
    noGeneration.replace(noGeneration.find(R"("generation": 4, )"), 17, "");
    CHECK_THAT(systemsRefusal(head + noGeneration + "]}").message, ContainsSubstring("/systems/0/generation"));
    CHECK_THAT(systemsRefusal(head + R"({"system": "amiga", "kind": "computer", "generation": 4, "family": "f",
                                         "order": 1, "sign": "S", "platforms": ["amiga"]}]})")
                   .message,
               ContainsSubstring("only a home console or a handheld has a generation"));
    CHECK_THAT(systemsRefusal(head + R"({"system": "kodi", "kind": "excluded", "family": "f",
                                         "platforms": ["pcwindows"]}]})")
                   .message,
               ContainsSubstring("names its kind and platforms only"));
    CHECK_THAT(systemsRefusal(head + R"({"system": "kodi", "kind": "excluded", "platforms": ["PC"]}]})").message,
               ContainsSubstring("platform tag in lowercase"));
    CHECK_THAT(systemsRefusal(head + R"({"system": "kodi", "kind": "excluded", "platforms": []}]})").message,
               ContainsSubstring("names no platform tag"));
    std::string badSign = snes;
    badSign.replace(badSign.find(R"("sign": "S")"), 11, R"("sign": " S")");
    CHECK_THAT(systemsRefusal(head + badSign + "]}").message, ContainsSubstring("/systems/0/sign"));
}

TEST_CASE("world/building: a floor's generations bind only the kinds that have them", "[world][building]") {
    // A floor of the first home generations that also holds "other": the reader counts every "other" system there, and
    // floorFor() and classify() find it there too (a system the reader accepts never stays without a floor).
    const std::string text = R"({"schemaVersion": 1, "floors": [{"id": "ground", "kinds": ["arcade"]},
        {"id": "gen2", "kinds": ["home", "other"], "generations": [1, 2]},
        {"id": "home", "kinds": ["home"], "generations": [3, 4, 5, 6, 7, 8, 9]},
        {"id": "pocket", "kinds": ["handheld"]}, {"id": "pc", "kinds": ["computer"]}],
        "layout": {"hallWidthMm": 3000, "lobbyWidthMm": 6000, "rowStartXMm": 3000, "wallAllowanceMm": 150,
                   "maxRoomsPerRow": 12}})";
    auto data = rv::world::parseBuildingData(text, "building.json");
    REQUIRE(data.has_value());
    CHECK(data->floorFor(SystemKind::Other, 0)->id == "gen2");
    CHECK(data->floorFor(SystemKind::Home, 2)->id == "gen2");
    CHECK(data->floorFor(SystemKind::Home, 3)->id == "home");
    CHECK(data->floorFor(SystemKind::Handheld, 4)->id == "pocket");
    CHECK(data->floorFor(SystemKind::Computer, 0)->id == "pc");
    const Classification pico = rv::world::classify(systemName("pico8"), {}, systems(), *data);
    CHECK(pico.kind() == SystemKind::Other);
    CHECK(pico.floor == "gen2");
    CHECK(pico.hasRoom());
    const Classification mystery = rv::world::classify(systemName("mystery"), {}, systems(), *data);
    CHECK(mystery.source == ClassificationSource::Fallback);
    CHECK(mystery.floor == "gen2");
}

TEST_CASE("world/building: rooms enter the shorter row at its east end, north on a tie", "[world][building]") {
    const rv::world::BuildingLayout& layout = building().layout;
    std::vector<Room> rooms;
    // The first room: the north row's first position, its footprint's west edge on the rows' start, its doorway wall
    // (the south wall, maxZ + wall) on the hall's north edge.
    const Room first = enter(rooms, roomOn("gen4", "snes"));
    CHECK(first.placement == RoomPlacement{3000 + 150 + 4500, -1500 - 150 - 5000, 0});
    CHECK(rv::world::floorFootprint(first, layout) == Footprint{3000, -11800, 12300, -1500});
    // The second: the south row (now the shorter), half a turn, its doorway wall on the hall's south edge.
    const Room second = enter(rooms, roomOn("gen4", "megadrive"));
    CHECK(second.placement == RoomPlacement{3000 + 4500 + 150, 1500 + 150 + 5000, 1});
    CHECK(rv::world::floorFootprint(second, layout) == Footprint{3000, 1500, 12300, 11800});
    // A tie: north.
    const Room third = enter(rooms, roomOn("gen4", "genesis"));
    CHECK(third.placement == RoomPlacement{12300 + 4650, -6650, 0});
    const Room fourth = enter(rooms, roomOn("gen4", "pcengine"));
    CHECK(fourth.placement.yawHalfTurns == 1);
    CHECK(fourth.placement.xMm == 12300 + 4650);
    // A wider room makes its row longer: the next one goes to the other row.
    Room wide = roomOn("gen4", "sfc");
    wide.minXMm = -6000;
    wide.maxXMm = 6000;
    const Room fifth = enter(rooms, wide);
    CHECK(fifth.placement == RoomPlacement{21600 + 6150, -6650, 0});
    const Room sixth = enter(rooms, roomOn("gen4", "nes"));
    CHECK(sixth.placement.yawHalfTurns == 1);
    CHECK(sixth.placement.xMm == 21600 + 4650);
    // Rooms of another floor are not in these rows; the ground floor's lobby is 6 m wide.
    std::vector<Room> ground;
    const Room arcade = enter(ground, roomOn("ground", "mame"));
    CHECK(arcade.placement == RoomPlacement{7650, -3000 - 150 - 5000, 0});
    CHECK(rv::world::floorRows(rooms, "ground", layout) == rv::world::FloorRows{3000, 3000, 0, 0});
    // No two rooms of the floor overlap.
    for (std::size_t i = 0; i < rooms.size(); ++i) {
        for (std::size_t j = i + 1; j < rooms.size(); ++j) {
            CHECK_FALSE(rv::world::floorFootprint(rooms[i], layout)
                            .overlaps(rv::world::floorFootprint(rooms[j], layout)));
        }
    }
}

TEST_CASE("world/building: full rows take no room; a row full alone sends the room to the other", "[world][building]") {
    rv::world::BuildingLayout layout = building().layout;
    layout.maxRoomsPerRow = 1;
    rv::world::FloorRows rows;
    rows.northEndMm = 30000;
    rows.southEndMm = 12000;
    rows.northRooms = 0;
    rows.southRooms = 1;
    // The south row is shorter but full: north.
    auto placement = rv::world::placeEnteringRoom(roomOn("gen4"), rows, layout);
    REQUIRE(placement.has_value());
    CHECK(placement->yawHalfTurns == 0);
    CHECK(placement->xMm == 30000 + 4650);
    rows.northRooms = 1;
    CHECK_FALSE(rv::world::placeEnteringRoom(roomOn("gen4"), rows, layout).has_value());
}

TEST_CASE("world/building: a growing room keeps its x and moves away from the hall", "[world][building]") {
    const rv::world::BuildingLayout& layout = building().layout;
    std::vector<Room> rooms;
    Room north = enter(rooms, roomOn("gen4", "snes"));
    Room south = enter(rooms, roomOn("gen4", "megadrive"));
    for (const std::uint32_t growth : {1u, 2u, 3u}) {
        INFO(growth);
        for (Room* room : {&north, &south}) {
            const RoomPlacement before = room->placement;
            room->maxZMm = 5000 + 2000 * static_cast<std::int64_t>(growth);
            room->growthSteps = growth;
            room->placement = rv::world::followDepth(*room, layout);
            CHECK(room->placement.xMm == before.xMm);
            CHECK(room->placement.yawHalfTurns == before.yawHalfTurns);
            // Its doorway wall still on the hall's edge.
            const Footprint footprint = rv::world::floorFootprint(*room, layout);
            if (room == &north) {
                CHECK(room->placement.zMm == before.zMm - 2000);
                CHECK(footprint.maxZMm == -1500);
            } else {
                CHECK(room->placement.zMm == before.zMm + 2000);
                CHECK(footprint.minZMm == 1500);
            }
        }
    }
    // The north wall stays put in the room's frame; on the floor the far wall moves away from the hall.
    CHECK(rv::world::floorFootprint(north, layout).minZMm == -1500 - 300 - 16000);
}

TEST_CASE("world/building: the room's frame on its floor's", "[world][building]") {
    const RoomPlacement north{7650, -6650, 0};
    const RoomPlacement south{7650, 6650, 1};
    // The doorway (the south wall's middle) faces the hall from both rows.
    CHECK(rv::world::roomToFloor(north, 0, 5150) == rv::world::FloorPoint{7650, -1500});
    CHECK(rv::world::roomToFloor(south, 0, 5150) == rv::world::FloorPoint{7650, 1500});
    // The room's east is the floor's east in the north row, its west in the south row.
    CHECK(rv::world::roomToFloor(north, 1000, 0) == rv::world::FloorPoint{8650, -6650});
    CHECK(rv::world::roomToFloor(south, 1000, 0) == rv::world::FloorPoint{6650, 6650});
    CHECK(rv::world::roomToFloor(south, Footprint{-100, -200, 300, 400}) == Footprint{7350, 6250, 7750, 6850});
    CHECK(rv::world::roomYawToFloor(north, 9000) == 9000);
    CHECK(rv::world::roomYawToFloor(south, 9000) == 27000);
    CHECK(rv::world::roomYawToFloor(south, 27000) == 9000);
    CHECK(rv::world::roomYawToFloor(south, 0) == 18000);
    CHECK(rv::world::rowOf(north) == rv::world::Row::North);
    CHECK(rv::world::rowOf(south) == rv::world::Row::South);
    CHECK(rv::world::hallWidthOf("ground", building().layout) == 6000);
    CHECK(rv::world::hallWidthOf("gen4", building().layout) == 3000);
}

TEST_CASE("world/building: every form of room id seeds its room by its text", "[world][building]") {
    // ADR-0013 §5: seeds derive from the id's text, so room:snes keeps its seed (the oracle's golden values are in
    // tests/unit/core/test_seed_random.cpp).
    const std::uint64_t world = 102938;
    for (const std::string_view text :
         {"room:snes", "room:snes:2", "area:shared:gen4", "area:shared:gen4:2", "area:arcade", "area:arcade:3"}) {
        INFO(text);
        const auto room = rv::world::RoomId::parse(text);
        REQUIRE(room.has_value());
        CHECK(rv::world::roomSeed(world, *room) == rv::core::deriveSeed(world, "room", text));
    }
    CHECK(rv::world::roomSeed(102938, *rv::world::RoomId::forSystem(systemName("snes"))) == 8977635278716121260ull);
}
