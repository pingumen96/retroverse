// retroverse-cli worldplan generate and show (I3 step 3): a plan generated from a selection file into the world folder
// of the user data directory, never over an existing plan, equal to world::generate's; the seed from --seed or
// world.json; --plan and --json; the catalog's selection of an imported fixture (esde/standard), and none while its
// media root is out of reach; show's summary, --placements and --json, and the plans it refuses.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/cli/command.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/log.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/sha256.hpp>
#include <rv/core/text.hpp>
#include <rv/world/generation.hpp>
#include <rv/world/plan_json.hpp>
#include <rv/world/room_selection.hpp>

#include "../world/world_generation_support.hpp"
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

RunResult runCli(const std::vector<std::string>& arguments, const rv::core::PathEnvironment& environment) {
    Output out = Output::memory();
    Output err = Output::memory();
    const ExitCode code = rv::cli::run(arguments, out, err, environment);
    return RunResult{code, out.text(), err.text()};
}

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

std::string readText(const std::filesystem::path& path) {
    auto text = rv::core::readTextFile(path);
    REQUIRE(text.has_value());
    return *text;
}

/// world::generate's plan text for the same inputs as the command's (the shipped preset's configuration).
std::string expectedPlan(std::size_t titles, std::uint64_t seed) {
    rv::world::GenerationInput input;
    input.library = rv::test::world::library();
    input.selection = rv::test::world::makeSelection(titles, 4242);
    input.config.seed = seed;
    auto result = rv::world::generate(input, rv::test::world::snesData());
    REQUIRE(result.has_value());
    return *rv::world::writePlan(result->plan);
}

} // namespace

TEST_CASE("cli/worldplan generate: a plan from a selection file into the world folder", "[cli][worldplan]") {
    const rv::test::TempDir dir("rv-cli-worldplan-generate");
    const LoggingReset loggingReset;
    const std::string data = rv::core::pathToUtf8(dir.file("data"));
    const rv::core::PathEnvironment environment = testEnvironment(dir.path());
    const std::filesystem::path selection = dir.file("selection.json");
    REQUIRE(rv::core::writeTextFileAtomic(selection,
                                          *rv::world::writeRoomSelection(rv::test::world::makeSelection(890, 4242)))
                .has_value());
    const std::string selectionText = rv::core::pathToUtf8(selection);
    const std::filesystem::path planPath = dir.file("data") / "worlds" / "esde" / "world-plan.json";

    const RunResult generated = runCli({"--data-dir", data, "worldplan", "generate", "--system", "snes", "--selection",
                                        selectionText, "--library", "esde", "--seed", "102938"},
                                       environment);
    INFO(generated.err);
    REQUIRE(generated.code == ExitCode::Ok);
    CHECK(readText(planPath) == expectedPlan(890, 102938));
    CHECK(contains(generated.out, "Generated the plan of room:snes: "));
    CHECK(contains(generated.out, "featured slots:  44 titles in 44 slots, 0 free, 0 dressed"));
    CHECK(contains(generated.out, "band slots:      12 titles in 12 slots, 0 free, 0 dressed"));
    CHECK(contains(generated.out, "spine slots:     834 titles in "));
    CHECK(contains(generated.out, "side-module 3"));

    // Never over an existing plan: exit 1, the plan untouched.
    const RunResult again = runCli({"--data-dir", data, "worldplan", "generate", "--system", "snes", "--selection",
                                    selectionText, "--library", "esde", "--seed", "1"},
                                   environment);
    CHECK(again.code == ExitCode::ProblemsFound);
    CHECK(contains(again.err, "a plan exists already"));
    CHECK(readText(planPath) == expectedPlan(890, 102938));

    // --plan writes elsewhere (and replaces); --json gives the summary as JSON; world.json's seed is the default.
    REQUIRE(rv::core::writeTextFileAtomic(dir.file("data") / "config" / "world.json", "{\"seed\": 7}").has_value());
    const std::filesystem::path other = dir.file("other.json");
    const RunResult json = runCli({"--data-dir", data, "worldplan", "generate", "--system", "snes", "--selection",
                                   selectionText, "--library", "esde", "--plan", rv::core::pathToUtf8(other), "--json"},
                                  environment);
    INFO(json.err);
    REQUIRE(json.code == ExitCode::Ok);
    CHECK(readText(other) == expectedPlan(890, 7));
    const nlohmann::json summary = nlohmann::json::parse(json.out);
    CHECK(summary["seed"] == 7);
    CHECK(summary["placements"] == 890);
    CHECK(summary["sections"]["featured"]["titles"] == 44);
    CHECK(summary["sections"]["spine"]["titles"] == 834);
    CHECK(summary["fixtures"]["byRole"]["side-module"] == 3);
    CHECK(summary["unplaced"]["count"] == 44);
    CHECK(summary["plan"] == rv::core::pathToUtf8(other));
    CHECK(summary["digest"] == rv::core::sha256Hex(readText(other)));
}

TEST_CASE("cli/worldplan generate: usage and failures", "[cli][worldplan]") {
    const rv::test::TempDir dir("rv-cli-worldplan-generate-errors");
    const LoggingReset loggingReset;
    const std::string data = rv::core::pathToUtf8(dir.file("data"));
    const rv::core::PathEnvironment environment = testEnvironment(dir.path());
    const std::filesystem::path selection = dir.file("selection.json");
    REQUIRE(rv::core::writeTextFileAtomic(selection,
                                          *rv::world::writeRoomSelection(rv::test::world::makeSelection(10, 1)))
                .has_value());
    const std::string selectionText = rv::core::pathToUtf8(selection);
    const auto generate = [&](std::vector<std::string> extra) {
        std::vector<std::string> arguments{"--data-dir", data, "worldplan", "generate"};
        arguments.insert(arguments.end(), extra.begin(), extra.end());
        return runCli(arguments, environment);
    };

    const RunResult help = generate({"--help"});
    CHECK(help.code == ExitCode::Ok);
    CHECK(contains(help.out, "Usage: retroverse-cli [global options] worldplan generate [--system <system>]..."));
    // The catalog's selection needs the library library.json records.
    const RunResult noSelection = generate({"--system", "snes", "--library", "esde"});
    CHECK(noSelection.code == ExitCode::UsageError);
    CHECK(contains(noSelection.err, "library create"));
    CHECK(generate({"--system", "sn/es", "--selection", selectionText, "--library", "esde"}).code ==
          ExitCode::UsageError);
    // --system names the selection files' systems.
    const RunResult unnamed = generate({"--system", "nes", "--selection", selectionText, "--library", "esde"});
    CHECK(unnamed.code == ExitCode::UsageError);
    CHECK(contains(unnamed.err, "--system nes has no --selection file"));
    for (const std::string_view seed : {"-1", "18446744073709551616", "01", "x", ""}) {
        INFO(seed);
        const RunResult bad =
            generate({"--system", "snes", "--selection", selectionText, "--library", "esde", "--seed", std::string(seed)});
        CHECK(bad.code == ExitCode::UsageError);
    }
    CHECK(generate({"--system", "snes", "--selection", selectionText, "--library", "Not A Library"}).code ==
          ExitCode::UsageError);
    // No library.json and no --library: a usage error naming library create.
    const RunResult noLibrary = generate({"--system", "snes", "--selection", selectionText});
    CHECK(noLibrary.code == ExitCode::UsageError);
    CHECK(contains(noLibrary.err, "library create"));
    // A system whose room the asset root cannot give (kodi: excluded, ADR-0013 §2), a selection that is not one:
    // exit 3.
    rv::world::RoomSelection kodiSelection = rv::test::world::makeSelection(10, 1);
    kodiSelection.room = *rv::world::RoomId::forSystem(*rv::catalog::SystemName::parse("kodi"));
    for (rv::world::UnplacedGame& game : kodiSelection.unplaced) {
        game.room = kodiSelection.room;
    }
    REQUIRE(rv::core::writeTextFileAtomic(dir.file("kodi.json"), *rv::world::writeRoomSelection(kodiSelection))
                .has_value());
    const RunResult kodi =
        generate({"--system", "kodi", "--selection", rv::core::pathToUtf8(dir.file("kodi.json")), "--library", "esde"});
    CHECK(kodi.code == ExitCode::RuntimeFailure);
    CHECK(contains(kodi.err, "the world's data of kodi"));
    CHECK(contains(kodi.err, "gets no room (excluded)"));
    REQUIRE(rv::core::writeTextFileAtomic(dir.file("broken.json"), "{}\n").has_value());
    const RunResult broken =
        generate({"--system", "snes", "--selection", rv::core::pathToUtf8(dir.file("broken.json")), "--library", "esde"});
    CHECK(broken.code == ExitCode::RuntimeFailure);
    CHECK(contains(broken.err, "the selection"));
    CHECK_FALSE(std::filesystem::exists(dir.file("data") / "worlds"));
}

TEST_CASE("cli/worldplan show: a plan's summary, its listing, and the plans it refuses", "[cli][worldplan]") {
    const rv::test::TempDir dir("rv-cli-worldplan-show");
    const LoggingReset loggingReset;
    const std::string data = rv::core::pathToUtf8(dir.file("data"));
    const rv::core::PathEnvironment environment = testEnvironment(dir.path());
    const std::filesystem::path planPath = dir.file("data") / "worlds" / "esde" / "world-plan.json";
    const std::string planText = expectedPlan(50, 102938);
    REQUIRE(rv::core::writeTextFileAtomic(planPath, planText).has_value());

    const RunResult shown = runCli({"--data-dir", data, "worldplan", "show", "--library", "esde"}, environment);
    INFO(shown.err);
    REQUIRE(shown.code == ExitCode::Ok);
    CHECK(contains(shown.out, "Plan "));
    CHECK(contains(shown.out, "Digest:          " +rv::core::sha256Hex(planText)));
    CHECK(contains(shown.out, "room:snes (console_room_9x10 v2): 9000 x 10000 x 3200 mm, 0 growth steps"));
    CHECK(contains(shown.out, "featured slots:  44 titles in 44 slots"));
    CHECK(contains(shown.out, "band slots:      6 titles in 12 slots, 0 free, 6 dressed"));
    CHECK(contains(shown.out, "spine slots:     0 titles in 396 slots, 0 free, 396 dressed"));
    CHECK_FALSE(contains(shown.out, "Placements:\n"));

    const std::string file = rv::core::pathToUtf8(planPath);
    const RunResult listed = runCli({"--data-dir", data, "worldplan", "show", file, "--placements"}, environment);
    REQUIRE(listed.code == ExitCode::Ok);
    CHECK(contains(listed.out, "Fixtures:\n  room:snes/fx:dc01  display  furniture.display_case.generic"));
    CHECK(contains(listed.out, "Placements:\n  room:snes/fx:dc01/s:0.0  g"));
    const RunResult json =
        runCli({"--data-dir", data, "worldplan", "show", file, "--placements", "--json"}, environment);
    REQUIRE(json.code == ExitCode::Ok);
    const nlohmann::json summary = nlohmann::json::parse(json.out);
    CHECK(summary["placements"] == 50);
    CHECK(summary["placementList"].size() == 50);
    CHECK(summary["sections"]["band"]["dressed"] == 6);

    // No plan: exit 1; a hand-edited one or one of a newer schema: exit 3; a file and --library together: exit 2.
    const RunResult missing = runCli({"--data-dir", data, "worldplan", "show", "--library", "other"}, environment);
    CHECK(missing.code == ExitCode::ProblemsFound);
    CHECK(contains(missing.err, "no plan at"));
    REQUIRE(rv::core::writeTextFileAtomic(dir.file("edited.json"), planText + "\n").has_value());
    CHECK(runCli({"--data-dir", data, "worldplan", "show", rv::core::pathToUtf8(dir.file("edited.json"))}, environment)
              .code == ExitCode::RuntimeFailure);
    std::string newer = planText;
    newer.replace(newer.find("\"schemaVersion\":2"), 17, "\"schemaVersion\":3");
    REQUIRE(rv::core::writeTextFileAtomic(dir.file("newer.json"), newer).has_value());
    const RunResult refused =
        runCli({"--data-dir", data, "worldplan", "show", rv::core::pathToUtf8(dir.file("newer.json"))}, environment);
    CHECK(refused.code == ExitCode::RuntimeFailure);
    CHECK(contains(refused.err, "newer"));
    CHECK(runCli({"--data-dir", data, "worldplan", "show", file, "--library", "esde"}, environment).code ==
          ExitCode::UsageError);
}

TEST_CASE("cli/worldplan generate: the catalog's selection of an imported library", "[cli][worldplan]") {
    auto fixture = rv::test::Fixture::materialize("esde/standard");
    INFO((fixture ? "materialized" : fixture.error().toString()));
    REQUIRE(fixture.has_value());
    const rv::test::TempDir dir("rv-cli-worldplan-catalog");
    const LoggingReset loggingReset;
    const std::string data = rv::core::pathToUtf8(dir.file("data"));
    const rv::core::PathEnvironment environment = testEnvironment(dir.path());
    const auto run = [&](std::vector<std::string> arguments) {
        arguments.insert(arguments.begin(), {"--data-dir", data});
        return runCli(arguments, environment);
    };
    const RunResult created =
        run({"library", "create", "--home", rv::core::pathToUtf8(fixture->root().lexically_normal()), "--install",
             rv::core::pathToUtf8(fixture->path("Programs/ES-DE").lexically_normal())});
    INFO(created.err);
    REQUIRE(created.code == ExitCode::Ok);

    // No catalog yet: exit 3.
    CHECK(run({"worldplan", "generate", "--system", "snes"}).code == ExitCode::RuntimeFailure);
    REQUIRE(run({"import"}).code == ExitCode::Ok);

    // A system the catalog does not hold: exit 1; another library than library.json's: exit 2.
    CHECK(run({"worldplan", "generate", "--system", "nosuch"}).code == ExitCode::ProblemsFound);
    CHECK(run({"worldplan", "generate", "--system", "snes", "--library", "other"}).code == ExitCode::UsageError);

    // The media root out of reach: unknown media decide nothing, no plan.
    std::filesystem::rename(fixture->path("media"), fixture->path("media-away"));
    const RunResult unknown = run({"worldplan", "generate", "--system", "snes"});
    CHECK(unknown.code == ExitCode::ProblemsFound);
    CHECK(contains(unknown.err, "no plan while the media are unknown"));
    CHECK_FALSE(std::filesystem::exists(dir.file("data") / "worlds"));
    std::filesystem::rename(fixture->path("media-away"), fixture->path("media"));

    const RunResult generated = run({"worldplan", "generate", "--system", "snes", "--seed", "102938", "--json"});
    INFO(generated.err);
    REQUIRE(generated.code == ExitCode::Ok);
    const nlohmann::json summary = nlohmann::json::parse(generated.out);
    CHECK(summary["seed"] == 102938);
    CHECK(summary["validation"]["passed"] == true);
    CHECK(summary["fixtures"]["byRole"].value("side-module", 0) == 0);
    CHECK(summary["placements"].get<std::size_t>() > 0);
    // Every placed title stands face-out: the fixture's library is far below D + B.
    CHECK(summary["sections"]["spine"]["titles"] == 0);
    auto plan = rv::world::readPlan(readText(dir.file("data") / "worlds" / "esde" / "world-plan.json"), "plan");
    REQUIRE(plan.has_value());
    CHECK(plan->seed == 102938);
    CHECK(plan->placements.size() == summary["placements"].get<std::size_t>());

    // I4 step 1: --system restricts the building only with --plan or when no plan is stored.
    const RunResult restricted = run({"worldplan", "generate", "--system", "snes", "--replace"});
    CHECK(restricted.code == ExitCode::UsageError);
    CHECK(contains(restricted.err, "--system restricts the building only with --plan or when no plan is stored"));
    CHECK(run({"worldplan", "generate", "--system", "snes", "--plan", rv::core::pathToUtf8(dir.file("small.json"))})
              .code == ExitCode::Ok);
    // The whole building: every system of the catalog with a room and a placeable title. The fixture's snes holds
    // fewer than 30 distinct titles: a section of gen4's shared room (I4 step 4).
    const RunResult whole = run({"worldplan", "generate", "--replace", "--seed", "102938"});
    INFO(whole.err);
    REQUIRE(whole.code == ExitCode::Ok);
    CHECK(contains(whole.out, "Regenerated the plan of "));
    CHECK(contains(whole.out, "area:shared:gen4"));
    auto building = rv::world::readPlan(readText(dir.file("data") / "worlds" / "esde" / "world-plan.json"), "plan");
    REQUIRE(building.has_value());
    CHECK(std::any_of(building->rooms.begin(), building->rooms.end(), [](const rv::world::Room& room) {
        return room.id.toString() == "area:shared:gen4" &&
               std::find(room.sections.begin(), room.sections.end(), *rv::catalog::SystemName::parse("snes")) !=
                   room.sections.end();
    }));
    CHECK(std::any_of(building->floors.begin(), building->floors.end(),
                      [](const rv::world::Floor& floor) { return floor.id == "ground"; }));
    // The update of every room of the plan: nothing new.
    const RunResult updated = run({"worldplan", "update", "--seed", "102938"});
    INFO(updated.err);
    CHECK(updated.code == ExitCode::Ok);
    CHECK(contains(updated.out, "is up to date"));
    // A system without a room: refused, nothing written (new systems come with I4 step 6a).
    const RunResult newSystem = run({"worldplan", "update", "--system", "nosuch", "--seed", "102938"});
    CHECK(newSystem.code == ExitCode::ProblemsFound);
    CHECK(contains(newSystem.err, "the plan has no room for nosuch"));
}
