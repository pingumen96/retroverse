// retroverse-cli worldplan and where-is on a building (I4 step 1, ADR-0013 §12): a building of two systems generated
// from selection files (--selection repeatable), shown with its floors and rooms, a game found with its floor, the
// building updated room by room (nothing new, then one room's new titles), a system without a room refused, and a
// frozen schema-1 plan (tests/fixtures/world/v1/) shown, checked, asked and updated lifted into the building.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/cli/command.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/log.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/random.hpp>
#include <rv/core/sha256.hpp>
#include <rv/core/text.hpp>
#include <rv/world/plan_json.hpp>
#include <rv/world/room_selection.hpp>
#include <rv/world/world_store.hpp>

#include "../world/world_generation_support.hpp"
#include "../world/world_update_support.hpp"
#include "support/fixture.hpp"
#include "support/temp_dir.hpp"

using rv::cli::ExitCode;
using rv::cli::Output;

namespace {

rv::core::PathEnvironment testEnvironment(const std::filesystem::path& executableDir) {
    rv::core::PathEnvironment environment;
    environment.executableDir = executableDir;
    environment.sourceAssetDir = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);
    return environment;
}

/// A run of the dispatcher logs to stderr; this puts back the plain console logger afterwards.
class LoggingReset {
public:
    LoggingReset() = default;
    ~LoggingReset() {
        rv::core::LogConfig config;
        config.console = false;
        rv::core::initLogging(config);
        rv::core::shutdownLogging();
    }
    LoggingReset(const LoggingReset&) = delete;
    LoggingReset& operator=(const LoggingReset&) = delete;
};

struct RunResult {
    ExitCode code = ExitCode::Ok;
    std::string out;
    std::string err;
};

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

std::string readText(const std::filesystem::path& path) {
    auto text = rv::core::readTextFile(path);
    REQUIRE(text.has_value());
    return *text;
}

/// makeSelection()'s selection for the room of `system`.
rv::world::RoomSelection selectionOf(std::string_view system, std::size_t titles, std::uint64_t seed) {
    rv::world::RoomSelection selection = rv::test::world::makeSelection(titles, seed);
    selection.room = *rv::world::RoomId::forSystem(*rv::catalog::SystemName::parse(system));
    for (rv::world::UnplacedGame& game : selection.unplaced) {
        game.room = selection.room;
    }
    return selection;
}

struct Session {
    rv::test::TempDir dir{"rv-cli-worldplan-building"};
    std::string data = rv::core::pathToUtf8(dir.file("data"));
    rv::core::PathEnvironment environment = testEnvironment(dir.path());
    rv::world::WorldFiles files = rv::world::WorldFiles::in(dir.file("data") / "worlds" / "esde");

    RunResult run(std::vector<std::string> arguments) const {
        arguments.insert(arguments.begin(), {"--data-dir", data});
        Output out = Output::memory();
        Output err = Output::memory();
        const ExitCode code = rv::cli::run(arguments, out, err, environment);
        return RunResult{code, out.text(), err.text()};
    }
    std::string write(std::string_view name, const rv::world::RoomSelection& selection) const {
        const std::filesystem::path path = dir.file(std::string(name));
        REQUIRE(rv::core::writeTextFileAtomic(path, *rv::world::writeRoomSelection(selection)).has_value());
        return rv::core::pathToUtf8(path);
    }
};

} // namespace

TEST_CASE("cli/worldplan: a building of selection files, its floors, where-is with the floor", "[cli][worldplan]") {
    const Session session;
    const LoggingReset loggingReset;
    const rv::world::RoomSelection snes = selectionOf("snes", 57, 1);
    rv::world::RoomSelection megadrive = selectionOf("megadrive", 30, 2);
    const std::string snesFile = session.write("snes.json", snes);
    const std::string megadriveFile = session.write("megadrive.json", megadrive);

    const RunResult generated = session.run({"worldplan", "generate", "--selection", snesFile, "--selection",
                                             megadriveFile, "--library", "esde", "--seed", "102938"});
    INFO(generated.err);
    REQUIRE(generated.code == ExitCode::Ok);
    CHECK(contains(generated.out, "Generated the plan of room:megadrive, room:snes: "));
    CHECK(contains(generated.out, "Floors:          ground (order 0, 0 rooms), gen4 (order 3, 2 rooms)"));
    CHECK(contains(generated.out, "gen4, console room of snes, north row at x 7650 z -6650 mm, generator 1.3"));
    CHECK(contains(generated.out, "gen4, console room of megadrive, south row at x 7650 z 6650 mm, generator 1.3"));

    const RunResult shown = session.run({"worldplan", "show", "--library", "esde", "--json"});
    REQUIRE(shown.code == ExitCode::Ok);
    const nlohmann::json summary = nlohmann::json::parse(shown.out);
    CHECK(summary["schemaVersion"] == 2);
    CHECK(summary["lifted"] == false);
    CHECK(summary["floors"].size() == 2);
    REQUIRE(summary["rooms"].size() == 2);
    CHECK(summary["rooms"][1]["id"] == "room:snes");
    CHECK(summary["rooms"][1]["kind"] == "console");
    CHECK(summary["rooms"][1]["floor"] == "gen4");
    CHECK(summary["rooms"][1]["sections"] == nlohmann::json::array({"snes"}));
    CHECK(summary["rooms"][1]["placement"]["row"] == "north row");

    // where-is names the floor.
    const std::string game = megadrive.titles.front().game.toString();
    const RunResult found = session.run({"where-is", game, "--library", "esde"});
    INFO(found.err);
    REQUIRE(found.code == ExitCode::Ok);
    CHECK(contains(found.out, game + " stands in room:megadrive\n"));
    CHECK(contains(found.out, "Floor:           gen4\n"));
    const RunResult foundJson = session.run({"where-is", game, "--library", "esde", "--json"});
    REQUIRE(foundJson.code == ExitCode::Ok);
    CHECK(nlohmann::json::parse(foundJson.out)["floor"] == "gen4");

    // The update, room by room: nothing new; then five new titles of megadrive, the snes room as it stands.
    const std::string before = readText(session.files.plan);
    const RunResult same = session.run({"worldplan", "update", "--selection", snesFile, "--selection", megadriveFile,
                                        "--library", "esde", "--seed", "102938"});
    INFO(same.err);
    CHECK(same.code == ExitCode::Ok);
    CHECK(contains(same.out, "is up to date"));
    CHECK(readText(session.files.plan) == before);
    std::set<std::uint64_t> taken = rv::test::world::gameValuesOf(megadrive);
    rv::core::Random random(5);
    for (int i = 0; i < 5; ++i) {
        rv::test::world::addTitle(megadrive, random, taken);
    }
    const std::string moreFile = session.write("megadrive-more.json", megadrive);
    const RunResult updated =
        session.run({"worldplan", "update", "--selection", moreFile, "--library", "esde", "--seed", "102938"});
    INFO(updated.err);
    REQUIRE(updated.code == ExitCode::Ok);
    CHECK(contains(updated.out, "Updated the plan of room:megadrive, room:snes: "));
    CHECK(contains(updated.out, "Changes:         5 added, "));
    auto plan = rv::world::readPlan(readText(session.files.plan), "plan");
    REQUIRE(plan.has_value());
    auto previous = rv::world::readPlan(before, "before");
    REQUIRE(previous.has_value());
    CHECK(*rv::world::roomContentDigest(*plan, snes.room) == *rv::world::roomContentDigest(*previous, snes.room));
    CHECK(*rv::world::roomContentDigest(*plan, megadrive.room) !=
          *rv::world::roomContentDigest(*previous, megadrive.room));

    // A system without a room: exit 1, nothing written.
    const std::string genesisFile = session.write("genesis.json", selectionOf("genesis", 10, 3));
    const std::string after = readText(session.files.plan);
    const RunResult refused =
        session.run({"worldplan", "update", "--selection", genesisFile, "--library", "esde", "--seed", "102938"});
    CHECK(refused.code == ExitCode::ProblemsFound);
    CHECK(contains(refused.err, "the plan has no room for genesis"));
    CHECK(readText(session.files.plan) == after);
}

TEST_CASE("cli/worldplan: a frozen schema-1 plan shown, checked, asked and updated lifted", "[cli][worldplan]") {
    const Session session;
    const LoggingReset loggingReset;
    const std::filesystem::path oracle =
        rv::core::pathFromUtf8(RV_TEST_SOURCE_DIR) / "tests" / "fixtures" / "world" / "v1" / "plan-50-seed-102938.json";
    const std::string v1 = readText(oracle);
    REQUIRE(rv::core::writeTextFileAtomic(session.files.plan, v1).has_value());
    const std::string planFile = rv::core::pathToUtf8(session.files.plan);

    const RunResult shown = session.run({"worldplan", "show", planFile});
    INFO(shown.err);
    REQUIRE(shown.code == ExitCode::Ok);
    CHECK(contains(shown.out, "(schema 1, lifted into the building)"));
    CHECK(contains(shown.out, "Digest:          " + rv::core::sha256Hex(v1)));
    CHECK(contains(shown.out, "Floors:          ground (order 0, 0 rooms), gen4 (order 3, 1 room)"));
    CHECK(contains(shown.out, "gen4, console room of snes, north row at x 7650 z -6650 mm, generator 1.2"));

    const RunResult checked = session.run({"worldplan", "check", planFile});
    INFO(checked.out);
    CHECK(checked.code == ExitCode::Ok);
    CHECK(contains(checked.out, "Result: passed"));

    auto plan = rv::world::readPlan(v1, "v1");
    REQUIRE(plan.has_value());
    const std::string game = plan->placements.front().game.toString();
    const RunResult found = session.run({"where-is", game, "--plan", planFile});
    REQUIRE(found.code == ExitCode::Ok);
    CHECK(contains(found.out, "Floor:           gen4\n"));

    // The update with the oracle's selection writes the building in schema 2, its room's contents as they were.
    const std::string selectionFile = session.write("selection.json", rv::test::world::makeSelection(50, 20261018));
    const RunResult updated =
        session.run({"worldplan", "update", "--selection", selectionFile, "--library", "esde", "--seed", "102938"});
    INFO(updated.err);
    REQUIRE(updated.code == ExitCode::Ok);
    CHECK(contains(updated.out, "Updated the plan of room:snes (lifted from schema 1): "));
    CHECK(contains(updated.out, "Changes:         0 added, 50 kept, 0 newly unplaced"));
    auto lifted = rv::world::readPlan(readText(session.files.plan), "lifted");
    REQUIRE(lifted.has_value());
    CHECK(lifted->schemaVersion == 2);
    CHECK(lifted->placements == plan->placements);
    CHECK(lifted->fixtures == plan->fixtures);
    // Its backup is the schema-1 file.
    bool backedUp = false;
    for (const auto& entry : std::filesystem::directory_iterator(session.files.backups)) {
        backedUp = backedUp || readText(entry.path()) == v1;
    }
    CHECK(backedUp);
    // Again: up to date.
    const RunResult again =
        session.run({"worldplan", "update", "--selection", selectionFile, "--library", "esde", "--seed", "102938"});
    CHECK(again.code == ExitCode::Ok);
    CHECK(contains(again.out, "is up to date"));
}

TEST_CASE("cli/worldplan: a stored building is regenerated whole; --system and --selection agree; --scene's room",
          "[cli][worldplan]") {
    const Session session;
    const LoggingReset loggingReset;
    const rv::world::RoomSelection snes = selectionOf("snes", 57, 1);
    rv::world::RoomSelection megadrive = selectionOf("megadrive", 30, 2);
    const std::string snesFile = session.write("snes.json", snes);
    const std::string megadriveFile = session.write("megadrive.json", megadrive);
    const std::vector<std::string> common{"--library", "esde", "--seed", "102938"};
    const auto run = [&](std::vector<std::string> arguments) {
        arguments.insert(arguments.end(), common.begin(), common.end());
        return session.run(arguments);
    };
    REQUIRE(run({"worldplan", "generate", "--selection", snesFile, "--selection", megadriveFile}).code ==
            ExitCode::Ok);
    const std::string stored = readText(session.files.plan);

    // --replace regenerates the whole building: selection files that leave out a stored room's system are refused,
    // with --system or without, and the plan stays as it was.
    const RunResult withSystem =
        run({"worldplan", "generate", "--system", "snes", "--selection", snesFile, "--replace"});
    CHECK(withSystem.code == ExitCode::UsageError);
    CHECK(contains(withSystem.err, "leave out megadrive of the stored building"));
    const RunResult filesOnly = run({"worldplan", "generate", "--selection", snesFile, "--replace"});
    CHECK(filesOnly.code == ExitCode::UsageError);
    CHECK(readText(session.files.plan) == stored);
    // --system and the files name the same systems.
    const RunResult moreFiles =
        run({"worldplan", "generate", "--system", "snes", "--selection", snesFile, "--selection", megadriveFile,
             "--plan", rv::core::pathToUtf8(session.dir.file("elsewhere.json"))});
    CHECK(moreFiles.code == ExitCode::UsageError);
    CHECK(contains(moreFiles.err, "the --selection file of megadrive names a system --system does not"));
    // Every stored system given: the building regenerated (here the same plan, so the file stays as it was).
    const RunResult whole =
        run({"worldplan", "generate", "--selection", snesFile, "--selection", megadriveFile, "--replace"});
    INFO(whole.err);
    CHECK(whole.code == ExitCode::Ok);
    CHECK(contains(whole.out, "Regenerated the plan of room:megadrive, room:snes: "));
    CHECK(readText(session.files.plan) == stored);

    // update: the rooms updated are the files'; --system given with them names the same systems.
    std::set<std::uint64_t> taken = rv::test::world::gameValuesOf(megadrive);
    rv::core::Random random(6);
    rv::test::world::addTitle(megadrive, random, taken);
    const std::string moreFile = session.write("megadrive-more.json", megadrive);
    const RunResult mismatched = run({"worldplan", "update", "--system", "snes", "--selection", moreFile});
    CHECK(mismatched.code == ExitCode::UsageError);
    CHECK(contains(mismatched.err, "--system names snes, the --selection files megadrive"));
    CHECK(readText(session.files.plan) == stored);
    // One room's selection with nothing new: up to date, the plan as it was (its inputDigest, of both rooms, kept).
    const RunResult oneRoom = run({"worldplan", "update", "--system", "snes", "--selection", snesFile});
    INFO(oneRoom.err);
    CHECK(oneRoom.code == ExitCode::Ok);
    CHECK(contains(oneRoom.out, "is up to date"));
    CHECK(readText(session.files.plan) == stored);

    // --scene on a building of two rooms names its room.
    const std::string scene = rv::core::pathToUtf8(session.dir.file("room.scene.json"));
    const RunResult unnamed = session.run({"worldplan", "show", "--library", "esde", "--scene", scene});
    CHECK(unnamed.code == ExitCode::UsageError);
    CHECK(contains(unnamed.err, "the plan holds 2 rooms: --room <id>"));
    const RunResult absent =
        session.run({"worldplan", "show", "--library", "esde", "--scene", scene, "--room", "room:genesis"});
    CHECK(absent.code == ExitCode::UsageError);
    CHECK(contains(absent.err, "the plan holds no room room:genesis"));
    const RunResult named =
        session.run({"worldplan", "show", "--library", "esde", "--scene", scene, "--room", "room:snes"});
    INFO(named.err);
    REQUIRE(named.code == ExitCode::Ok);
    CHECK(contains(named.out, " of room:snes: "));
    const RunResult namedJson = session.run(
        {"worldplan", "show", "--library", "esde", "--scene", scene, "--room", "room:megadrive", "--json"});
    REQUIRE(namedJson.code == ExitCode::Ok);
    CHECK(nlohmann::json::parse(namedJson.out)["sceneRoom"] == "room:megadrive");
    CHECK(session.run({"worldplan", "show", "--library", "esde", "--room", "room:snes"}).code ==
          ExitCode::UsageError);
}

TEST_CASE("cli/worldplan: the building fixture's whole building, then its additions", "[cli][worldplan]") {
    // I4 steps 2 and 4 (tests/fixtures/esde/building/README.md): every system of the catalog gets its room on its floor
    // with its media family, or a section: mame of the arcade room, the systems under 30 distinct titles of their
    // floor's shared room (amiga's 20 on the computer floor; the custom system, on the handheld floor by gb's platform
    // tag, and pokemini); kodi is excluded; then additions/ brings titles to three systems (pokemini past 30: it stays
    // in its section), which an update places without moving anything, and a new system, which the update cannot place
    // yet (step 6a).
    auto fixture = rv::test::Fixture::materialize("esde/building");
    INFO((fixture ? "materialized" : fixture.error().toString()));
    REQUIRE(fixture.has_value());
    const Session session;
    const LoggingReset loggingReset;
    const RunResult created =
        session.run({"library", "create", "--home", rv::core::pathToUtf8(fixture->root().lexically_normal()),
                     "--install", rv::core::pathToUtf8(fixture->path("Programs/ES-DE").lexically_normal())});
    INFO(created.err);
    REQUIRE(created.code == ExitCode::Ok);
    const RunResult imported = session.run({"import"});
    INFO(imported.out << imported.err);
    REQUIRE(imported.code == ExitCode::Ok);

    const RunResult generated = session.run({"worldplan", "generate", "--seed", "102938"});
    INFO(generated.out << generated.err);
    REQUIRE(generated.code == ExitCode::Ok);
    CHECK(contains(generated.err, "kodi left out of the building: no room (excluded)"));
    auto plan = rv::world::readPlan(readText(session.files.plan), "plan");
    REQUIRE(plan.has_value());
    struct Expected {
        std::string_view room;
        std::string_view floor;
        std::string_view family;
        std::size_t placed;
    };
    const Expected expected[] = {
        {"area:arcade", "ground", "generic_case", 15},           // mame: 25 sets, 15 with a cover
        {"area:shared:computer", "computer", "generic_case", 20}, // amiga: no media at all, generated labels
        {"area:shared:handheld", "handheld", "generic_case", 14}, // pocketlab (gb's row by its tag) 6, pokemini 8
        {"room:gb", "handheld", "cartridge_handheld", 30},       // 32 games, 2 second releases
        {"room:psx", "gen5", "disc_case_cd", 40},                // two of them multi-disc
        {"room:snes", "gen4", "cartridge_home_16bit", 40},       // 42 games, 2 second releases
    };
    REQUIRE(plan->rooms.size() == std::size(expected));
    std::vector<std::string> floors;
    for (const rv::world::Floor& floor : plan->floors) {
        floors.push_back(floor.id);
    }
    CHECK(floors == std::vector<std::string>{"ground", "gen4", "gen5", "handheld", "computer"});
    for (std::size_t i = 0; i < plan->rooms.size(); ++i) {
        const rv::world::Room& room = plan->rooms[i];
        INFO(room.id.toString());
        CHECK(room.id.toString() == expected[i].room);
        CHECK(room.floor == expected[i].floor);
        CHECK(room.mediaFamily == expected[i].family);
        std::size_t placed = 0;
        for (const rv::world::Placement& placement : plan->placements) {
            placed += placement.slot.fixture().room() == room.id ? 1 : 0;
        }
        CHECK(placed == expected[i].placed);
    }
    const auto sectionsOf = [&](std::string_view id) {
        std::vector<std::string> sections;
        for (const rv::world::Room& room : plan->rooms) {
            if (room.id.toString() == id) {
                for (const rv::catalog::SystemName& system : room.sections) {
                    sections.push_back(system.str());
                }
            }
        }
        return sections;
    };
    CHECK(sectionsOf("area:arcade") == std::vector<std::string>{"mame"});
    CHECK(sectionsOf("area:shared:handheld") == std::vector<std::string>{"pocketlab", "pokemini"});
    // The arcade sets without a cover: unplaced, no-cover (the room places only the covered ones).
    const rv::world::RoomId mame = *rv::world::RoomId::parse("area:arcade");
    std::size_t noCover = 0;
    std::size_t otherwise = 0;
    for (const rv::world::UnplacedGame& unplaced : plan->unplaced) {
        if (unplaced.room == mame) {
            (unplaced.reason == rv::world::UnplacedReason::NoCover ? noCover : otherwise) += 1;
        }
    }
    CHECK(noCover == 10);
    CHECK(otherwise == 0);
    const RunResult checked = session.run({"worldplan", "check", rv::core::pathToUtf8(session.files.plan)});
    INFO(checked.out);
    CHECK(checked.code == ExitCode::Ok);

    // Selection files must give every section of a shared room: pokemini's alone is refused (exit 2, naming the
    // section without one), the plan left as it was.
    const std::string generatedText = readText(session.files.plan);
    const std::string pokeminiFile = session.write("pokemini.json", selectionOf("pokemini", 8, 4));
    const RunResult oneSection =
        session.run({"worldplan", "update", "--selection", pokeminiFile, "--seed", "102938"});
    INFO(oneSection.out << oneSection.err);
    CHECK(oneSection.code == ExitCode::UsageError);
    CHECK(contains(oneSection.err, "area:shared:handheld is updated with the selections of all its sections: no "
                                   "--selection file of pocketlab"));
    CHECK(readText(session.files.plan) == generatedText);

    // The shared room's scene: the sign of a room of several systems, without a console (I4 step 4).
    const std::filesystem::path pocketScene = session.dir.file("shared.scene.json");
    const RunResult scene = session.run(
        {"worldplan", "show", "--scene", rv::core::pathToUtf8(pocketScene), "--room", "area:shared:handheld"});
    INFO(scene.err);
    REQUIRE(scene.code == ExitCode::Ok);
    const nlohmann::json pocketObjects = nlohmann::json::parse(readText(pocketScene))["objects"];
    std::vector<std::string> signs;
    for (const nlohmann::json& object : pocketObjects) {
        CHECK(object["id"] != "fx:hero.console");
        const auto textures = object.find("textures");
        if (textures != object.end() && textures->contains("picture") &&
            (*textures)["picture"].value("pattern", "") == "sign") {
            signs.push_back((*textures)["picture"].value("title", ""));
        }
    }
    CHECK(signs == std::vector<std::string>{"MORE GAMES"});

    // I4 step 3: a floor's hall and rooms, and its scene: the handheld floor's two rooms on two rows along its hall.
    const RunResult floor = session.run({"worldplan", "show", "--floor", "handheld"});
    INFO(floor.err);
    REQUIRE(floor.code == ExitCode::Ok);
    CHECK(contains(floor.out, "Floor handheld: hall 12300 x 3000 mm, 3200 mm high, 2 light(s) every 6000 mm, the "
                              "elevator at its west end; 2 room(s)\n"));
    CHECK(contains(floor.out, "  room:gb  north row, doorway x 6850 to 8450 mm onto the hall\n"));
    CHECK(contains(floor.out, "  area:shared:handheld  south row, doorway x 6850 to 8450 mm onto the hall\n"));
    const std::filesystem::path floorScene = session.dir.file("handheld.scene.json");
    const RunResult floorJson =
        session.run({"worldplan", "show", "--floor", "handheld", "--scene", rv::core::pathToUtf8(floorScene), "--json"});
    INFO(floorJson.err);
    REQUIRE(floorJson.code == ExitCode::Ok);
    const nlohmann::json shownFloor = nlohmann::json::parse(floorJson.out);
    CHECK(shownFloor["sceneFloor"] == "handheld");
    CHECK(shownFloor["floorScene"]["hall"]["lengthMm"] == 12300);
    CHECK(shownFloor["floorScene"]["rooms"].size() == 2);
    const nlohmann::json handheldScene = nlohmann::json::parse(readText(floorScene));
    CHECK(handheldScene["name"] == "floor:handheld");
    CHECK(handheldScene["areas"].size() == 3);
    std::vector<std::string> floorSigns;
    for (const nlohmann::json& object : handheldScene["objects"]) {
        const auto textures = object.find("textures");
        if (textures != object.end() && textures->contains("picture") &&
            (*textures)["picture"].value("pattern", "") == "sign") {
            floorSigns.push_back((*textures)["picture"].value("title", ""));
        }
    }
    // The rooms' signs (gb's, the shared room's) and the floor's by its elevator.
    std::sort(floorSigns.begin(), floorSigns.end());
    CHECK(floorSigns == std::vector<std::string>{"HANDHELDS", "MORE GAMES", "POCKET GAMES"});
    // --floor names a floor of the plan, and not with --room.
    CHECK(session.run({"worldplan", "show", "--floor", "gen7"}).code == ExitCode::UsageError);
    const RunResult both = session.run(
        {"worldplan", "show", "--floor", "gen4", "--room", "room:snes", "--scene", rv::core::pathToUtf8(floorScene)});
    CHECK(both.code == ExitCode::UsageError);
    CHECK(contains(both.err, "--room and --floor exclude each other"));

    // additions/: five titles more for snes and psx, 24 for pokemini (past 30), and the new megadrive.
    REQUIRE(fixture->applyOverlay("additions"));
    const RunResult again = session.run({"import"});
    INFO(again.out << again.err);
    REQUIRE(again.code == ExitCode::Ok);
    // pokemini's room is the shared one: naming it updates the room with its sections' selections (pocketlab's too).
    const RunResult updated = session.run({"worldplan", "update", "--system", "snes", "--system", "psx", "--system",
                                           "pokemini", "--seed", "102938"});
    INFO(updated.out << updated.err);
    REQUIRE(updated.code == ExitCode::Ok);
    CHECK(contains(updated.out, "Changes:         34 added, "));
    auto after = rv::world::readPlan(readText(session.files.plan), "after");
    REQUIRE(after.has_value());
    // Nothing placed before moved; the rooms not updated are as they were; pokemini past 30 distinct titles stays in
    // its section (ADR-0013 §3: sticky), its 24 new titles in the shared room.
    for (const rv::world::Placement& placement : plan->placements) {
        const auto kept = std::find(after->placements.begin(), after->placements.end(), placement);
        CHECK(kept != after->placements.end());
    }
    for (const std::string_view room : {"area:arcade", "area:shared:computer", "room:gb"}) {
        const rv::world::RoomId id = *rv::world::RoomId::parse(room);
        CHECK(*rv::world::roomContentDigest(*after, id) == *rv::world::roomContentDigest(*plan, id));
    }
    CHECK(after->rooms.size() == plan->rooms.size());
    const rv::world::RoomId sharedId = *rv::world::RoomId::parse("area:shared:handheld");
    std::size_t sharedPlaced = 0;
    for (const rv::world::Placement& placement : after->placements) {
        sharedPlaced += placement.slot.fixture().room() == sharedId ? 1 : 0;
    }
    CHECK(sharedPlaced == 14 + 24);
    // An update of the whole building (every room's sections) records the inputs of every room once, places nothing;
    // again, it writes nothing.
    const RunResult whole = session.run({"worldplan", "update", "--seed", "102938"});
    INFO(whole.out << whole.err);
    REQUIRE(whole.code == ExitCode::Ok);
    CHECK(contains(whole.out, "Changes:         0 added, "));
    const std::string wholeText = readText(session.files.plan);
    const RunResult same = session.run({"worldplan", "update", "--seed", "102938"});
    INFO(same.out << same.err);
    CHECK(same.code == ExitCode::Ok);
    CHECK(contains(same.out, "is up to date"));
    CHECK(readText(session.files.plan) == wholeText);
    const RunResult megadrive = session.run({"worldplan", "update", "--system", "megadrive", "--seed", "102938"});
    CHECK(megadrive.code == ExitCode::ProblemsFound);
    CHECK(contains(megadrive.err, "the plan has no room for megadrive"));
}
