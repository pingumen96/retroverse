// retroverse-cli worldplan snapshots and worldplan undo (I4b step 1, ADR-0015 §6) on a fixture world: the building
// fixture's plan generated, a hand-written schema-1 overrides.json beside it, the explicit regeneration (generate
// --replace) saved behind a snapshot (the overrides rewritten in schema 2), listed, undone with --dry-run (nothing
// written) and then for real (the plan, its sidecar and the schema-1 overrides back byte for byte, an update finding
// nothing to do), the undo itself never undone. And the overflow fixture's building (I4b step 0): more titles than the
// big-box room and the arcade room hold, unplaced for capacity until generator 1.5's expansion rooms (step 7).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/cli/command.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/log.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/world/plan_json.hpp>
#include <rv/world/world_store.hpp>

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

std::optional<std::string> contentOf(const std::filesystem::path& path) {
    auto text = rv::core::readTextFile(path);
    if (!text) {
        REQUIRE(text.error().code == rv::ErrorCode::NotFound);
        return std::nullopt;
    }
    return *text;
}

struct Session {
    rv::test::TempDir dir{"rv-cli-worldplan-undo"};
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

    /// The fixture's library recorded and imported.
    void importFixture(const rv::test::Fixture& fixture) const {
        const RunResult created =
            run({"library", "create", "--home", rv::core::pathToUtf8(fixture.root().lexically_normal()), "--install",
                 rv::core::pathToUtf8(fixture.path("Programs/ES-DE").lexically_normal())});
        INFO(created.err);
        REQUIRE(created.code == ExitCode::Ok);
        const RunResult imported = run({"import"});
        INFO(imported.out << imported.err);
        REQUIRE(imported.code == ExitCode::Ok);
    }
};

} // namespace

TEST_CASE("cli/worldplan undo: a regeneration undone on the building fixture's world", "[cli][worldplan]") {
    auto fixture = rv::test::Fixture::materialize("esde/building");
    INFO((fixture ? "materialized" : fixture.error().toString()));
    REQUIRE(fixture.has_value());
    const Session session;
    const LoggingReset loggingReset;
    session.importFixture(*fixture);

    // No world yet: nothing to undo, and no folder made for it.
    const RunResult early = session.run({"worldplan", "undo"});
    CHECK(early.code == ExitCode::ProblemsFound);
    CHECK(contains(early.err, "nothing to undo: the library esde has no world yet"));
    CHECK_FALSE(std::filesystem::exists(session.files.folder));

    const RunResult generated = session.run({"worldplan", "generate", "--seed", "102938"});
    INFO(generated.out << generated.err);
    REQUIRE(generated.code == ExitCode::Ok);
    CHECK_FALSE(contains(generated.out, "Snapshot:")); // a first plan: no explicit action over a world
    const std::string firstPlan = *contentOf(session.files.plan);
    const std::string firstMeta = *contentOf(session.files.meta);
    // A pin written by hand in I3's schema: the hero CRT of the snes room, which every seed's plan holds.
    auto plan = rv::world::readPlan(firstPlan, "plan");
    REQUIRE(plan.has_value());
    const bool crt = std::any_of(plan->fixtures.begin(), plan->fixtures.end(), [](const rv::world::Fixture& f) {
        return f.id.toString() == "room:snes/fx:hero.crt";
    });
    REQUIRE(crt);
    const std::string handOverrides =
        "{\"schemaVersion\": 1, \"fixturePins\": [{\"fixture\": \"room:snes/fx:hero.crt\"}]}\n";
    REQUIRE(rv::core::writeTextFileAtomic(session.files.overrides, handOverrides).has_value());
    const RunResult none = session.run({"worldplan", "snapshots"});
    REQUIRE(none.code == ExitCode::Ok);
    CHECK(contains(none.out, "  none\n"));
    CHECK(contains(none.out, "Next undo:       nothing to undo\n"));

    // The explicit regeneration with another seed: behind a snapshot, the overrides written again in schema 2.
    const RunResult replaced = session.run({"worldplan", "generate", "--replace", "--seed", "4242"});
    INFO(replaced.out << replaced.err);
    REQUIRE(replaced.code == ExitCode::Ok);
    CHECK(contains(replaced.out, "Snapshot:        "));
    CHECK(contains(replaced.out, "-regenerate ('worldplan undo' restores it)\n"));
    const std::string secondPlan = *contentOf(session.files.plan);
    REQUIRE(secondPlan != firstPlan);
    const std::string liftedOverrides = *contentOf(session.files.overrides);
    CHECK(contains(liftedOverrides, "\"schemaVersion\": 2"));
    CHECK(contains(liftedOverrides, "\"origin\": \"file\""));

    const RunResult listed = session.run({"worldplan", "snapshots", "--json"});
    INFO(listed.err);
    REQUIRE(listed.code == ExitCode::Ok);
    const nlohmann::json list = nlohmann::json::parse(listed.out);
    REQUIRE(list["snapshots"].size() == 1);
    const nlohmann::json& snapshot = list["snapshots"][0];
    CHECK(snapshot["kind"] == "action");
    CHECK(snapshot["action"] == "regenerate");
    CHECK(snapshot["scope"] == "world");
    CHECK(snapshot["undone"] == false);
    CHECK(snapshot["undoes"].is_null());
    CHECK(contains(snapshot["writer"].get<std::string>(), "retroverse-cli worldplan generate --replace"));
    CHECK(list["nextUndo"] == snapshot["name"]);
    CHECK(list["kept"] == 20);
    CHECK(list["interruptedAction"] == false);
    const std::string name = snapshot["name"].get<std::string>();
    const RunResult listedText = session.run({"worldplan", "snapshots"});
    CHECK(contains(listedText.out, "  " + name + "  regenerate of world, "));
    CHECK(contains(listedText.out, "Next undo:       " + name + " (regenerate of world)\n"));

    // A snapshot missing a file it kept is damaged (exit 3), not "nothing to undo".
    const std::filesystem::path keptPlan = session.files.snapshots / name / "world-plan.json";
    const std::filesystem::path aside = session.dir.file("world-plan.aside");
    std::filesystem::rename(keptPlan, aside);
    const RunResult missing = session.run({"worldplan", "undo"});
    CHECK(missing.code == ExitCode::RuntimeFailure);
    CHECK(contains(missing.err, "the snapshot " + name + " is damaged: world-plan.json is missing"));
    std::filesystem::rename(aside, keptPlan);
    // A folder whose snapshot.json cannot be read: listed as damaged, undo refused (exit 3) until it is moved out.
    const std::filesystem::path torn = session.files.snapshots / "20200101T000000.000Z-edit";
    std::filesystem::create_directories(torn);
    REQUIRE(rv::core::writeTextFileAtomic(torn / "snapshot.json", "{ torn").has_value());
    const RunResult damagedText = session.run({"worldplan", "snapshots"});
    REQUIRE(damagedText.code == ExitCode::Ok);
    CHECK(contains(damagedText.out, "  20200101T000000.000Z-edit  damaged: "));
    CHECK(contains(damagedText.out, "Next undo:       refused while a snapshot is damaged"));
    const nlohmann::json damagedList =
        nlohmann::json::parse(session.run({"worldplan", "snapshots", "--json"}).out);
    REQUIRE(damagedList["damaged"].size() == 1);
    CHECK(damagedList["damaged"][0]["name"] == "20200101T000000.000Z-edit");
    CHECK(damagedList["nextUndo"].is_null());
    const RunResult refused = session.run({"worldplan", "undo", "--dry-run"});
    CHECK(refused.code == ExitCode::RuntimeFailure);
    CHECK(contains(refused.err, "undo is refused while a snapshot is damaged: 20200101T000000.000Z-edit"));
    std::filesystem::remove_all(torn);
    CHECK(contentOf(session.files.plan) == secondPlan);

    // A dry run names the snapshot and writes nothing.
    const RunResult dry = session.run({"worldplan", "undo", "--dry-run"});
    INFO(dry.err);
    REQUIRE(dry.code == ExitCode::Ok);
    CHECK(contains(dry.out, "Would undo regenerate of world: the snapshot " + name + " taken "));
    CHECK(contentOf(session.files.plan) == secondPlan);
    CHECK(contentOf(session.files.overrides) == liftedOverrides);

    // Undone: the plan, its sidecar and the hand-written overrides back, byte for byte.
    const RunResult undone = session.run({"worldplan", "undo", "--json"});
    INFO(undone.err);
    REQUIRE(undone.code == ExitCode::Ok);
    const nlohmann::json result = nlohmann::json::parse(undone.out);
    CHECK(result["dryRun"] == false);
    CHECK(result["restored"]["name"] == name);
    CHECK(result["restored"]["undone"] == true);
    CHECK(result["kept"]["kind"] == "undo");
    CHECK(result["kept"]["undoes"] == name);
    CHECK(result["rolledBack"].is_null());
    CHECK(contentOf(session.files.plan) == firstPlan);
    CHECK(contentOf(session.files.meta) == firstMeta);
    CHECK(contentOf(session.files.overrides) == handOverrides);
    // The catalog has not changed since: the update finds the restored plan up to date.
    const RunResult update = session.run({"worldplan", "update", "--seed", "102938"});
    INFO(update.out << update.err);
    CHECK(update.code == ExitCode::Ok);
    CHECK(contains(update.out, "is up to date"));
    CHECK(contentOf(session.files.plan) == firstPlan);

    // Undo of undo refused: the undo's own snapshot is never offered, and no action is left.
    const RunResult again = session.run({"worldplan", "undo"});
    CHECK(again.code == ExitCode::ProblemsFound);
    CHECK(contains(again.err, "nothing to undo: the last change was an undo, which is never undone (no redo)"));
    CHECK(contentOf(session.files.plan) == firstPlan);
    const nlohmann::json after = nlohmann::json::parse(session.run({"worldplan", "snapshots", "--json"}).out);
    REQUIRE(after["snapshots"].size() == 2);
    CHECK(after["snapshots"][0]["kind"] == "undo");
    CHECK(after["snapshots"][1]["undone"] == true);
    CHECK(after["nextUndo"].is_null());
}

TEST_CASE("cli/worldplan: the overflow fixture's building holds more titles than its rooms", "[cli][worldplan]") {
    // I4b step 0 (tests/fixtures/esde/overflow/README.md): until generator 1.5 (step 7) the big-box room and the
    // arcade room place what their templates' growth allows, the rest unplaced for capacity, naming the room.
    auto fixture = rv::test::Fixture::materialize("esde/overflow");
    INFO((fixture ? "materialized" : fixture.error().toString()));
    REQUIRE(fixture.has_value());
    const Session session;
    const LoggingReset loggingReset;
    session.importFixture(*fixture);
    const RunResult generated = session.run({"worldplan", "generate", "--seed", "102938"});
    INFO(generated.out << generated.err);
    REQUIRE(generated.code == ExitCode::Ok);
    auto plan = rv::world::readPlan(*contentOf(session.files.plan), "plan");
    REQUIRE(plan.has_value());
    std::map<std::string, std::size_t> placed;
    for (const rv::world::Placement& placement : plan->placements) {
        ++placed[placement.slot.fixture().room().toString()];
    }
    std::map<std::string, std::size_t> capacity;
    std::size_t otherwise = 0;
    for (const rv::world::UnplacedGame& unplaced : plan->unplaced) {
        if (unplaced.reason == rv::world::UnplacedReason::Capacity) {
            ++capacity[unplaced.room.toString()];
        } else {
            ++otherwise;
        }
    }
    INFO(generated.out);
    CHECK(otherwise == 0);
    REQUIRE(placed.size() == 2);
    CHECK(placed.at("area:arcade") + capacity["area:arcade"] == 60);
    CHECK(placed.at("room:amiga") + capacity["room:amiga"] == 1700);
    // Both past their room (the arcade room holds 53 sets, the big-box room 1,561 titles at its growth limit): the
    // titles the expansion rooms of step 7 will place.
    CHECK(capacity["area:arcade"] == 7);
    CHECK(capacity["room:amiga"] == 139);
}
