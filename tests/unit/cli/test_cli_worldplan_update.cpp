// retroverse-cli worldplan update, worldplan generate --replace and where-is (I3 step 9): a plan updated from selection
// files in the world folder (the previous plan backed up, the meta sidecar written, every placement kept, --diff and
// --json), an update with nothing to change writing nothing, the plans refused (edited by hand, newer, another seed:
// exit 1, the plan untouched), --previous and --plan, the explicit regeneration choosing the face-out set again after a
// backup, the world lock held by this process refusing retroverse-cli as a second process, the catalog's selection of
// an imported fixture (unknown media change nothing), and where-is answering placed and unplaced games.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/cli/command.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/backups.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/log.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/sha256.hpp>
#include <rv/core/text.hpp>
#include <rv/world/generation.hpp>
#include <rv/world/incremental.hpp>
#include <rv/world/plan_json.hpp>
#include <rv/world/room_selection.hpp>
#include <rv/world/where_is.hpp>
#include <rv/world/world_store.hpp>

#include "../world/world_generation_support.hpp"
#include "../world/world_update_support.hpp"
#include "support/child_process.hpp"
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

void writeSelection(const std::filesystem::path& path, const rv::world::RoomSelection& selection) {
    REQUIRE(rv::core::writeTextFileAtomic(path, *rv::world::writeRoomSelection(selection)).has_value());
}

/// A session in a fresh data directory with a selection of 890 titles and the same 890 plus 5 new ones.
struct Session {
    rv::test::TempDir dir{"rv-cli-worldplan-update"};
    std::string data = rv::core::pathToUtf8(dir.file("data"));
    rv::core::PathEnvironment environment = testEnvironment(dir.path());
    rv::world::WorldFiles files = rv::world::WorldFiles::in(dir.file("data") / "worlds" / "esde");
    rv::world::RoomSelection base = rv::test::world::makeSelection(890, 4242);
    rv::world::RoomSelection more = base;
    std::string baseFile = rv::core::pathToUtf8(dir.file("base.json"));
    std::string moreFile = rv::core::pathToUtf8(dir.file("more.json"));

    Session() {
        std::set<std::uint64_t> taken = rv::test::world::gameValuesOf(base);
        rv::core::Random random(895);
        for (int i = 0; i < 5; ++i) {
            rv::test::world::addTitle(more, random, taken);
        }
        writeSelection(dir.file("base.json"), base);
        writeSelection(dir.file("more.json"), more);
    }

    RunResult run(std::vector<std::string> arguments) const {
        arguments.insert(arguments.begin(), {"--data-dir", data});
        return runCli(arguments, environment);
    }
    RunResult generate(const std::string& selection, std::vector<std::string> extra = {}) const {
        std::vector<std::string> arguments{"worldplan", "generate", "--system",  "snes", "--selection",
                                           selection,   "--library", "esde",     "--seed", "102938"};
        arguments.insert(arguments.end(), extra.begin(), extra.end());
        return run(arguments);
    }
    RunResult update(const std::string& selection, std::vector<std::string> extra = {}) const {
        std::vector<std::string> arguments{"worldplan", "update", "--system",  "snes", "--selection",
                                           selection,   "--library", "esde",   "--seed", "102938"};
        arguments.insert(arguments.end(), extra.begin(), extra.end());
        return run(arguments);
    }
};

} // namespace

TEST_CASE("cli/worldplan update: the world's plan updated after a backup, every placement kept", "[cli][worldplan]") {
    const LoggingReset loggingReset;
    const Session session;
    const RunResult generated = session.generate(session.baseFile);
    INFO(generated.err);
    REQUIRE(generated.code == ExitCode::Ok);
    const std::string before = readText(session.files.plan);
    const rv::world::WorldPlan previous = *rv::world::readPlan(before, "plan");

    const RunResult updated = session.update(session.moreFile, {"--diff"});
    INFO(updated.err);
    REQUIRE(updated.code == ExitCode::Ok);
    CHECK(contains(updated.out, "Updated the plan of room:snes: "));
    CHECK(contains(updated.out, "Outcome:         updated, rebuild local-reflow"));
    CHECK(contains(updated.out, "Changes:         5 added, 890 kept, 0 newly unplaced"));
    CHECK(contains(updated.out, "Digest before:   " + rv::core::sha256Hex(before)));
    CHECK(contains(updated.out, "Added:\n  g"));
    // The plan is world::update's, every earlier placement byte-identical; the previous one is in backups/.
    rv::world::UpdateInput input;
    input.library = rv::test::world::library();
    input.selection = session.more;
    input.config.seed = 102938;
    input.previous = previous;
    const std::string after = readText(session.files.plan);
    CHECK(after == *rv::world::writePlan(rv::world::update(input, rv::test::world::snesData())->plan));
    const rv::world::WorldPlan plan = *rv::world::readPlan(after, "plan");
    CHECK(std::includes(plan.placements.begin(), plan.placements.end(), previous.placements.begin(),
                        previous.placements.end()));
    const std::vector<std::filesystem::path> backups =
        rv::core::listBackupsOf(session.files.plan, session.files.backups);
    REQUIRE(backups.size() == 1);
    CHECK(readText(backups.front()) == before);
    auto meta = rv::world::readPlanMeta(readText(session.files.meta), "meta");
    REQUIRE(meta.has_value());
    CHECK(meta->outcome == rv::world::GenerationOutcome::Updated);
    CHECK(meta->added == 5);
    CHECK(meta->kept == 890);
    CHECK(meta->digestBefore == rv::core::sha256Hex(before));
    CHECK(meta->planDigest == rv::core::sha256Hex(after));
    CHECK(contains(meta->writer, "retroverse-cli worldplan update"));

    // Nothing to change: nothing written, no backup more.
    const RunResult again = session.update(session.moreFile, {"--json"});
    REQUIRE(again.code == ExitCode::Ok);
    const nlohmann::json json = nlohmann::json::parse(again.out);
    CHECK(json["written"] == false);
    CHECK(json["changes"]["outcome"] == "updated");
    CHECK(json["changes"]["rebuild"] == "no-rebuild");
    CHECK(json["changes"]["added"].empty());
    CHECK(json["changes"]["kept"] == 895);
    CHECK(json["summary"]["placements"] == 895);
    CHECK(readText(session.files.plan) == after);
    CHECK(rv::core::listBackupsOf(session.files.plan, session.files.backups).size() == 1);
}

TEST_CASE("cli/worldplan update: plans it refuses and where it reads and writes", "[cli][worldplan]") {
    const LoggingReset loggingReset;
    const Session session;
    // No plan to update: exit 1.
    const RunResult none = session.update(session.moreFile);
    CHECK(none.code == ExitCode::ProblemsFound);
    CHECK(contains(none.err, "no plan to update at"));
    REQUIRE(session.generate(session.baseFile).code == ExitCode::Ok);
    const std::string before = readText(session.files.plan);

    // Another seed: an explicit regeneration is needed; exit 1, the plan untouched.
    const RunResult reseeded = session.run({"worldplan", "update", "--system", "snes", "--selection",
                                            session.moreFile, "--library", "esde", "--seed", "7"});
    CHECK(reseeded.code == ExitCode::ProblemsFound);
    CHECK(contains(reseeded.err, "needs an explicit regeneration (the world seed is 7"));
    CHECK(contains(reseeded.err, "worldplan generate --replace"));
    CHECK(contains(reseeded.out, "Outcome:         requires-explicit-regeneration"));
    CHECK(readText(session.files.plan) == before);

    // Edited by hand, or written by a newer Retroverse: refused, exit 1, nothing written.
    const std::filesystem::path edited = session.dir.file("edited.json");
    REQUIRE(rv::core::writeTextFileAtomic(edited, before + "\n").has_value());
    const RunResult handEdited = session.update(session.moreFile, {"--previous", rv::core::pathToUtf8(edited)});
    CHECK(handEdited.code == ExitCode::ProblemsFound);
    CHECK(contains(handEdited.err, "the plan is refused"));
    std::string newerText = before;
    // Every generator version of it (the plan's is the highest of its rooms').
    const std::string current = "\"generatorVersion\":\"" + rv::world::kGeneratorVersion.toString() + "\"";
    for (std::size_t at = newerText.find(current); at != std::string::npos; at = newerText.find(current, at)) {
        newerText.replace(at, current.size(), "\"generatorVersion\":\"1.9\"");
    }
    const std::filesystem::path newer = session.dir.file("newer.json");
    REQUIRE(rv::core::writeTextFileAtomic(newer, newerText).has_value());
    const RunResult newerRun = session.update(session.moreFile, {"--previous", rv::core::pathToUtf8(newer)});
    CHECK(newerRun.code == ExitCode::ProblemsFound);
    CHECK(contains(newerRun.err, "newer Retroverse"));
    CHECK(readText(session.files.plan) == before);

    // --previous and --plan: another plan read, the result written elsewhere, the world's plan untouched.
    const std::filesystem::path copy = session.dir.file("copy.json");
    REQUIRE(rv::core::writeTextFileAtomic(copy, before).has_value());
    const std::filesystem::path out = session.dir.file("out.json");
    const RunResult elsewhere = session.update(
        session.moreFile, {"--previous", rv::core::pathToUtf8(copy), "--plan", rv::core::pathToUtf8(out), "--json"});
    INFO(elsewhere.err);
    REQUIRE(elsewhere.code == ExitCode::Ok);
    CHECK(rv::world::readPlan(readText(out), "out")->placements.size() == 895);
    CHECK(readText(session.files.plan) == before);
    CHECK(readText(copy) == before);
    CHECK(rv::core::listBackupsOf(session.files.plan, session.files.backups).empty());

    // Usage errors: exit 2.
    CHECK(session.run({"worldplan", "update", "--selection", session.moreFile}).code == ExitCode::UsageError);
    CHECK(session.update(session.moreFile, {"--seed", "x"}).code == ExitCode::UsageError);
    const RunResult help = session.run({"worldplan", "update", "--help"});
    CHECK(help.code == ExitCode::Ok);
    CHECK(contains(help.out, "Usage: retroverse-cli [global options] worldplan update [--system <system>]..."));
}

TEST_CASE("cli/worldplan generate --replace: the face-out set chosen again after a backup", "[cli][worldplan]") {
    const LoggingReset loggingReset;
    const Session session;
    REQUIRE(session.generate(session.baseFile).code == ExitCode::Ok);
    const std::string before = readText(session.files.plan);
    // The last title by priority becomes the first: an update moves nothing, the regeneration features it.
    rv::world::RoomSelection favoured = session.base;
    rv::world::RoomTitle title = favoured.titles.back();
    favoured.titles.pop_back();
    title.favorite = true;
    title.playCount = 1000000;
    favoured.titles.insert(favoured.titles.begin(), title);
    const std::filesystem::path favouredFile = session.dir.file("favoured.json");
    writeSelection(favouredFile, favoured);
    REQUIRE(session.update(rv::core::pathToUtf8(favouredFile)).code == ExitCode::Ok);
    const auto standsFaceOut = [&] {
        const rv::world::WorldPlan plan = *rv::world::readPlan(readText(session.files.plan), "plan");
        const auto location = rv::world::PlanIndex(plan).find(title.game);
        return location && location->place && rv::world::isFaceOut(location->place->section);
    };
    CHECK_FALSE(standsFaceOut());
    const std::string updated = readText(session.files.plan);

    // Without --replace: never over the plan.
    const RunResult refused = session.generate(rv::core::pathToUtf8(favouredFile));
    CHECK(refused.code == ExitCode::ProblemsFound);
    CHECK(contains(refused.err, "a plan exists already"));
    CHECK(session.generate(rv::core::pathToUtf8(favouredFile), {"--replace", "--plan", "x.json"}).code ==
          ExitCode::UsageError);

    const RunResult replaced = session.generate(rv::core::pathToUtf8(favouredFile), {"--replace"});
    INFO(replaced.err);
    REQUIRE(replaced.code == ExitCode::Ok);
    CHECK(contains(replaced.out, "Regenerated the plan of room:snes: "));
    CHECK(contains(replaced.out, "Backup:          the previous plan kept as world-plan-"));
    CHECK(standsFaceOut());
    const std::vector<std::filesystem::path> backups =
        rv::core::listBackupsOf(session.files.plan, session.files.backups);
    REQUIRE(backups.size() == 2);
    CHECK(readText(backups[0]) == updated);
    CHECK(readText(backups[1]) == before);
    CHECK(rv::world::readPlanMeta(readText(session.files.meta), "meta")->outcome ==
          rv::world::GenerationOutcome::Generated);
}

TEST_CASE("cli/worldplan: the world lock held by another process refuses the write", "[cli][worldplan]") {
    const LoggingReset loggingReset;
    const Session session;
    const std::filesystem::path cli = rv::test::testProgramsDir() / "retroverse-cli.exe";
    if (!std::filesystem::exists(cli)) {
        WARN("retroverse-cli.exe is not built beside rv_tests.exe: the second process is not run");
        return;
    }
    const auto child = [&](std::vector<std::string> arguments) {
        rv::test::ChildRequest request;
        request.executable = cli;
        request.arguments = {"--data-dir", session.data, "worldplan"};
        request.arguments.insert(request.arguments.end(), arguments.begin(), arguments.end());
        request.stdoutFile = session.dir.file("child-stdout.txt");
        request.stderrFile = session.dir.file("child-stderr.txt");
        request.timeout = std::chrono::minutes(2);
        auto result = rv::test::runChild(request);
        REQUIRE(result.has_value());
        CHECK_FALSE(result->timedOut);
        return *result;
    };
    const std::vector<std::string> generate{"generate", "--system", "snes", "--selection", session.baseFile,
                                            "--library", "esde", "--seed", "102938"};
    {
        auto lock = rv::world::WorldLock::acquire(session.files);
        REQUIRE(lock.has_value());
        const rv::test::ChildResult held = child(generate);
        CHECK(held.exitCode == 3);
        CHECK(contains(held.standardError, "another program writes this world"));
        CHECK(held.elapsed >= rv::world::kWorldLockWait);
        CHECK_FALSE(std::filesystem::exists(session.files.plan));
    }
    // Released: the other process writes, and its update after it too.
    const rv::test::ChildResult free = child(generate);
    INFO(free.standardError);
    CHECK(free.exitCode == 0);
    CHECK(std::filesystem::exists(session.files.plan));
    std::vector<std::string> update = generate;
    update[0] = "update";
    update[4] = session.moreFile;
    const rv::test::ChildResult updated = child(update);
    INFO(updated.standardError);
    CHECK(updated.exitCode == 0);
    CHECK(rv::world::readStoredPlan(session.files.plan)->plan.placements.size() == 895);
}

TEST_CASE("cli/where-is: placed and unplaced games of the world's plan", "[cli][worldplan]") {
    const LoggingReset loggingReset;
    const Session session;
    const RunResult noPlan = session.run({"where-is", "g0123456789abcdef", "--library", "esde"});
    CHECK(noPlan.code == ExitCode::ProblemsFound);
    CHECK(contains(noPlan.err, "no plan at"));
    REQUIRE(session.generate(session.baseFile).code == ExitCode::Ok);
    const rv::world::WorldPlan plan = *rv::world::readPlan(readText(session.files.plan), "plan");

    const rv::world::Placement& placed = plan.placements.back();
    const RunResult found = session.run({"where-is", placed.game.toString(), "--library", "esde"});
    INFO(found.err);
    REQUIRE(found.code == ExitCode::Ok);
    CHECK(contains(found.out, placed.game.toString() + " stands in room:snes"));
    CHECK(contains(found.out, "Slot:            " + placed.slot.toString()));
    const RunResult json = session.run({"where-is", placed.game.toString(), "--library", "esde", "--json"});
    REQUIRE(json.code == ExitCode::Ok);
    const nlohmann::json answer = nlohmann::json::parse(json.out);
    CHECK(answer["placed"] == true);
    CHECK(answer["place"]["slot"] == placed.slot.toString());
    CHECK(answer["digest"] == rv::core::sha256Hex(readText(session.files.plan)));

    const auto duplicate = std::find_if(plan.unplaced.begin(), plan.unplaced.end(), [](const auto& game) {
        return game.reason == rv::world::UnplacedReason::DuplicateTitle;
    });
    REQUIRE(duplicate != plan.unplaced.end());
    const RunResult unplaced =
        session.run({"where-is", duplicate->game.toString(), "--plan", rv::core::pathToUtf8(session.files.plan)});
    REQUIRE(unplaced.code == ExitCode::Ok);
    CHECK(contains(unplaced.out, "is not placed in room:snes: duplicate-title"));
    CHECK(contains(unplaced.out, "Representative:  " + duplicate->representative.toString()));
    CHECK(contains(unplaced.out, "It stands at:    room:snes/fx:"));

    // A game the plan does not hold: exit 1; a malformed GameId or no GameId: exit 2; a refused plan: exit 3.
    CHECK(session.run({"where-is", "g0000000000000001", "--library", "esde"}).code == ExitCode::ProblemsFound);
    CHECK(session.run({"where-is", "nope", "--library", "esde"}).code == ExitCode::UsageError);
    CHECK(session.run({"where-is", "--library", "esde"}).code == ExitCode::UsageError);
    REQUIRE(rv::core::writeTextFileAtomic(session.dir.file("edited.json"), readText(session.files.plan) + " ")
                .has_value());
    CHECK(session.run({"where-is", placed.game.toString(), "--plan",
                       rv::core::pathToUtf8(session.dir.file("edited.json"))})
              .code == ExitCode::RuntimeFailure);
    CHECK(session.run({"where-is", "--help"}).code == ExitCode::Ok);
}

TEST_CASE("cli/worldplan update: the catalog's selection; unknown media change nothing", "[cli][worldplan]") {
    auto fixture = rv::test::Fixture::materialize("esde/standard");
    INFO((fixture ? "materialized" : fixture.error().toString()));
    REQUIRE(fixture.has_value());
    const rv::test::TempDir dir("rv-cli-worldplan-update-catalog");
    const LoggingReset loggingReset;
    const std::string data = rv::core::pathToUtf8(dir.file("data"));
    const rv::core::PathEnvironment environment = testEnvironment(dir.path());
    const auto run = [&](std::vector<std::string> arguments) {
        arguments.insert(arguments.begin(), {"--data-dir", data});
        return runCli(arguments, environment);
    };
    REQUIRE(run({"library", "create", "--home", rv::core::pathToUtf8(fixture->root().lexically_normal()), "--install",
                 rv::core::pathToUtf8(fixture->path("Programs/ES-DE").lexically_normal())})
                .code == ExitCode::Ok);
    REQUIRE(run({"import"}).code == ExitCode::Ok);
    REQUIRE(run({"worldplan", "generate", "--system", "snes"}).code == ExitCode::Ok);
    const std::filesystem::path planPath = dir.file("data") / "worlds" / "esde" / "world-plan.json";
    const std::string before = readText(planPath);

    // The catalog unchanged: nothing to update.
    const RunResult unchanged = run({"worldplan", "update", "--system", "snes"});
    INFO(unchanged.err);
    REQUIRE(unchanged.code == ExitCode::Ok);
    CHECK(contains(unchanged.out, "is up to date"));
    CHECK(readText(planPath) == before);

    // The media root out of reach: no update, the plan untouched.
    std::filesystem::rename(fixture->path("media"), fixture->path("media-away"));
    const RunResult unknown = run({"worldplan", "update", "--system", "snes"});
    CHECK(unknown.code == ExitCode::ProblemsFound);
    CHECK(contains(unknown.err, "no update while the media are unknown"));
    CHECK(readText(planPath) == before);
    std::filesystem::rename(fixture->path("media-away"), fixture->path("media"));

    // where-is answers every game of the catalog's SNES system that the plan holds.
    const rv::world::WorldPlan plan = *rv::world::readPlan(before, "plan");
    for (const rv::world::Placement& placement : plan.placements) {
        CHECK(run({"where-is", placement.game.toString()}).code == ExitCode::Ok);
    }
}
