// retroverse-cli worldplan check (I3 step 4, ADR-0010 §11): a valid plan passes (exit 0), a plan that breaks a hard
// rule fails (exit 1) with its findings, a selection's games are checked, a text that is not a plan is a problem found
// (exit 1), a missing plan file a runtime failure (exit 3) and bad arguments usage errors (exit 2); text and JSON, to
// stdout or --output; nothing written under the user data but the log.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

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
#include <rv/world/plan_json.hpp>
#include <rv/world/room_selection.hpp>

#include "../world/world_test_plans.hpp"
#include "support/temp_dir.hpp"

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::StartsWith;
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

/// The valid plan with its collection fixtures' prefabs renamed to manifests no asset root holds, so that their slots
/// are never read (the rule prefab-unresolved, soft, for each of the 11) whatever prefabs the asset root ships.
rv::world::WorldPlan cliPlan() {
    rv::world::WorldPlan plan = rv::test::validRoomPlan();
    for (rv::world::Fixture& fixture : plan.fixtures) {
        if (!fixture.levels.empty()) {
            fixture.prefab.manifest = "test.unshipped." + fixture.prefab.kind;
        }
    }
    return plan;
}

class CheckRun {
public:
    CheckRun() : m_dir("rv-cli-worldplan-check"), m_data(rv::core::pathToUtf8(m_dir.file("data"))) {}

    std::string writePlan(const rv::world::WorldPlan& plan, std::string_view name = "world-plan.json") {
        auto text = rv::world::writePlan(plan);
        REQUIRE(text.has_value());
        return writeFile(name, *text);
    }

    std::string writeFile(std::string_view name, std::string_view text) {
        const std::filesystem::path path = m_dir.file(std::string(name));
        REQUIRE(rv::core::writeTextFileAtomic(path, text).has_value());
        return rv::core::pathToUtf8(path);
    }

    RunResult run(std::vector<std::string> arguments) {
        arguments.insert(arguments.begin(), {"--data-dir", m_data, "worldplan", "check"});
        Output out = Output::memory();
        Output err = Output::memory();
        const ExitCode code = rv::cli::run(arguments, out, err, testEnvironment(m_dir.path()));
        return RunResult{code, out.text(), err.text()};
    }

    [[nodiscard]] const rv::test::TempDir& dir() const noexcept { return m_dir; }

private:
    rv::test::TempDir m_dir;
    std::string m_data;
    LoggingReset m_loggingReset;
};

} // namespace

TEST_CASE("cli/worldplan check: a valid plan passes, its soft findings and LayoutScore listed", "[cli][worldplan]") {
    CheckRun check;
    const std::string plan = check.writePlan(cliPlan());

    const RunResult text = check.run({plan});
    INFO(text.out << text.err);
    CHECK(text.code == ExitCode::Ok);
    CHECK_THAT(text.out, StartsWith("Plan: " + plan + "\nDigest: "));
    CHECK_THAT(text.out, ContainsSubstring("Room room:snes: template console_room_9x10 version 1, 9000 x 10000 mm, 0 "
                                           "growth steps, 28 fixtures\n"));
    CHECK_THAT(text.out, ContainsSubstring("Placements: 30, dressed spans: 2, unplaced: 2\n"));
    CHECK_THAT(text.out, ContainsSubstring("Selection: none (games missing from the plan are not checked)\n"));
    CHECK_THAT(text.out, ContainsSubstring("Findings: 0 hard, 11 soft\n"));
    CHECK_THAT(text.out, ContainsSubstring("  soft prefab-unresolved room:snes/fx:dc01: the slots of the prefab "
                                           "test.unshipped.display_case (display_case) cannot be read"));
    CHECK_THAT(text.out, ContainsSubstring("\nResult: passed\n"));

    const RunResult json = check.run({plan, "--json"});
    CHECK(json.code == ExitCode::Ok);
    const nlohmann::json result = nlohmann::json::parse(json.out);
    CHECK(result["plan"] == plan);
    CHECK(result["readable"] == true);
    CHECK(result["passed"] == true);
    CHECK(result["hardCount"] == 0);
    CHECK(result["softCount"] == 11);
    CHECK(result["selection"].is_null());
    auto planText = rv::core::readTextFile(rv::core::pathFromUtf8(plan));
    REQUIRE(planText.has_value());
    CHECK(result["digest"] == rv::core::sha256Hex(std::string_view(*planText)));
    CHECK(result["rooms"].size() == 1);
    CHECK(result["rooms"][0]["fixtures"] == 28);
    CHECK(result["placements"] == 30);
    REQUIRE(result["findings"].size() == 11);
    CHECK(result["findings"][0]["rule"] == "prefab-unresolved");
    CHECK(result["findings"][0]["severity"] == "soft");
    CHECK(result["layoutScorePermille"].is_number_integer());

    // --output writes the results to the file instead.
    const std::string output = rv::core::pathToUtf8(check.dir().file("out/check.txt"));
    const RunResult toFile = check.run({plan, "--output", output});
    CHECK(toFile.code == ExitCode::Ok);
    CHECK(toFile.out.empty());
    auto written = rv::core::readTextFile(rv::core::pathFromUtf8(output));
    REQUIRE(written.has_value());
    CHECK(*written == text.out);

    // Nothing but the log under the user data.
    CHECK_FALSE(std::filesystem::exists(check.dir().file("data") / "worlds"));
}

TEST_CASE("cli/worldplan check: a hard rule broken exits 1 with the findings; the selection's games are checked",
          "[cli][worldplan]") {
    CheckRun check;
    rv::world::WorldPlan broken = cliPlan();
    broken.placements[1].slot = broken.placements[0].slot;
    const std::string plan = check.writePlan(broken);
    const RunResult text = check.run({plan});
    INFO(text.out << text.err);
    CHECK(text.code == ExitCode::ProblemsFound);
    CHECK_THAT(text.out, ContainsSubstring("Findings: 1 hard, 11 soft\n"));
    CHECK_THAT(text.out, ContainsSubstring("  hard slot-shared room:snes/fx:hw-w/s:0.0: the slot holds 2 games: "
                                           "g0000000000000001, g0000000000000002\n"));
    CHECK_THAT(text.out, ContainsSubstring("\nResult: FAILED\n"));
    const nlohmann::json json = nlohmann::json::parse(check.run({plan, "--json"}).out);
    CHECK(json["passed"] == false);
    CHECK(json["hardCount"] == 1);

    // A selection naming a game the plan neither places nor lists.
    const rv::world::WorldPlan valid = cliPlan();
    rv::world::RoomSelection selection = rv::test::selectionOf(valid);
    selection.unplaced.push_back(
        {rv::test::testGameId(200), rv::test::snesRoomId(), rv::world::UnplacedReason::NoCover, {}});
    auto selectionText = rv::world::writeRoomSelection(selection);
    REQUIRE(selectionText.has_value());
    const std::string selectionFile = check.writeFile("selection.json", *selectionText);
    const std::string validPlan = check.writePlan(valid, "valid.json");
    const RunResult missing = check.run({validPlan, "--selection", selectionFile});
    INFO(missing.out << missing.err);
    CHECK(missing.code == ExitCode::ProblemsFound);
    CHECK_THAT(missing.out, ContainsSubstring("Selection: " + selectionFile + " (every game of it checked)\n"));
    CHECK_THAT(missing.out, ContainsSubstring("  hard game-missing g00000000000000c8: "));
    // The plan's own selection: every game accounted for.
    auto ownText = rv::world::writeRoomSelection(rv::test::selectionOf(valid));
    REQUIRE(ownText.has_value());
    const RunResult own = check.run({validPlan, "--selection", check.writeFile("own.json", *ownText)});
    CHECK(own.code == ExitCode::Ok);
}

TEST_CASE("cli/worldplan check: a text that is not a plan exits 1, a missing file 3, bad arguments 2",
          "[cli][worldplan]") {
    CheckRun check;
    auto canonical = rv::world::writePlan(cliPlan());
    REQUIRE(canonical.has_value());

    // Edited by hand (a space after the first brace): not canonical.
    const std::string edited = check.writeFile("edited.json", "{ " + canonical->substr(1));
    const RunResult text = check.run({edited});
    CHECK(text.code == ExitCode::ProblemsFound);
    CHECK_THAT(text.out, StartsWith("Plan: " + edited + "\nNot a plan this program reads: "));
    CHECK_THAT(text.out, ContainsSubstring("\nResult: FAILED\n"));
    const RunResult json = check.run({edited, "--json"});
    CHECK(json.code == ExitCode::ProblemsFound);
    const nlohmann::json result = nlohmann::json::parse(json.out);
    CHECK(result["readable"] == false);
    CHECK(result["passed"] == false);
    CHECK(result["error"].is_string());

    // A newer schema.
    std::string newer = *canonical;
    const std::size_t at = newer.find("\"schemaVersion\":1");
    REQUIRE(at != std::string::npos);
    newer.replace(at, 17, "\"schemaVersion\":2");
    const RunResult schema = check.run({check.writeFile("newer.json", newer)});
    CHECK(schema.code == ExitCode::ProblemsFound);
    CHECK_THAT(schema.out, ContainsSubstring("Not a plan this program reads: "));

    // A plan file that is not there: a runtime failure, on stderr.
    const RunResult absent = check.run({rv::core::pathToUtf8(check.dir().file("absent.json"))});
    CHECK(absent.code == ExitCode::RuntimeFailure);
    CHECK(absent.out.empty());
    CHECK_THAT(absent.err, ContainsSubstring("retroverse-cli worldplan check: cannot read the plan: "));

    // Usage errors: no plan, an unknown option, a selection that cannot be read or is not one.
    const RunResult none = check.run({});
    CHECK(none.code == ExitCode::UsageError);
    CHECK_THAT(none.err, ContainsSubstring("retroverse-cli worldplan check: "));
    const RunResult option = check.run({edited, "--no-such-option"});
    CHECK(option.code == ExitCode::UsageError);
    const RunResult noSelection =
        check.run({edited, "--selection", rv::core::pathToUtf8(check.dir().file("absent-selection.json"))});
    CHECK(noSelection.code == ExitCode::UsageError);
    CHECK_THAT(noSelection.err, ContainsSubstring("the selection cannot be read"));
    const RunResult notSelection = check.run({edited, "--selection", edited});
    CHECK(notSelection.code == ExitCode::UsageError);
    CHECK_THAT(notSelection.err, ContainsSubstring("not a room selection"));

    // --help: the usage on stdout.
    const RunResult help = check.run({"--help"});
    CHECK(help.code == ExitCode::Ok);
    CHECK_THAT(help.out, StartsWith("Usage: retroverse-cli [global options] worldplan check <plan.json>"));
    CHECK(help.err.empty());
}
