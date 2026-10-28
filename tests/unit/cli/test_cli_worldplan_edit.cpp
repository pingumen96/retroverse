// retroverse-cli worldplan edit (I4b step 2, ADR-0015 §8) on the building fixture's world: a game moved into a free
// slot behind a snapshot, pinned in overrides.json (origin cli), the update then finding the plan up to date; a refusal
// naming its rule (exit 1, nothing written); a dry run writing nothing; a script applied as one session, one snapshot,
// undone; a fixture moved off the grid refused; usage errors (exit 2); no plan (exit 1).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <optional>
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
    rv::test::TempDir dir{"rv-cli-worldplan-edit"};
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

    [[nodiscard]] std::size_t snapshots() const {
        const RunResult listed = run({"worldplan", "snapshots", "--json"});
        REQUIRE(listed.code == ExitCode::Ok);
        return nlohmann::json::parse(listed.out)["snapshots"].size();
    }
};

/// The spine-out slots of `room` that hold no game, the undressed ones first.
std::vector<rv::world::SlotId> freeSpineSlots(const rv::world::WorldPlan& plan, std::string_view room) {
    std::set<rv::world::SlotId> taken;
    for (const rv::world::Placement& placement : plan.placements) {
        taken.insert(placement.slot);
    }
    std::vector<rv::world::SlotId> slots;
    for (const rv::world::Fixture& fixture : plan.fixtures) {
        if (fixture.id.room().toString() != room) {
            continue;
        }
        for (const rv::world::FixtureLevel& level : fixture.levels) {
            if (level.section != rv::world::Section::Spine) {
                continue;
            }
            for (std::uint32_t index = 0; index < level.slots; ++index) {
                const rv::world::SlotId slot = *rv::world::SlotId::make(fixture.id, level.level, index);
                const bool dressed = std::any_of(plan.dressing.begin(), plan.dressing.end(), [&](const auto& span) {
                    return span.level == slot.levelId() && index >= span.firstSlot &&
                           index < span.firstSlot + span.count;
                });
                if (!taken.contains(slot)) {
                    slots.insert(dressed ? slots.end() : slots.begin(), slot);
                }
            }
        }
    }
    return slots;
}

} // namespace

TEST_CASE("cli/worldplan edit: games moved, swapped and pinned on the building fixture's world, undone",
          "[cli][worldplan]") {
    auto fixture = rv::test::Fixture::materialize("esde/building");
    INFO((fixture ? "materialized" : fixture.error().toString()));
    REQUIRE(fixture.has_value());
    const Session session;
    const LoggingReset loggingReset;
    session.importFixture(*fixture);

    // No plan yet: nothing to edit.
    const RunResult early = session.run({"worldplan", "edit", "pin", "--game", "g0000000000000042"});
    CHECK(early.code == ExitCode::ProblemsFound);
    CHECK(contains(early.err, "no plan to edit"));

    REQUIRE(session.run({"worldplan", "generate", "--seed", "102938"}).code == ExitCode::Ok);
    const std::string generated = *contentOf(session.files.plan);
    auto plan = rv::world::readPlan(generated, "plan");
    REQUIRE(plan.has_value());
    std::vector<rv::world::Placement> snes;
    for (const rv::world::Placement& placement : plan->placements) {
        if (placement.slot.fixture().room().toString() == "room:snes") {
            snes.push_back(placement);
        }
    }
    REQUIRE(snes.size() >= 3);
    const std::vector<rv::world::SlotId> free = freeSpineSlots(*plan, "room:snes");
    REQUIRE_FALSE(free.empty());
    const std::string game = snes[0].game.toString();
    const std::string slot = free.front().toString();

    // Moved: behind a snapshot, pinned with the CLI's provenance.
    const RunResult moved = session.run({"worldplan", "edit", "move", "--game", game, "--slot", slot});
    INFO(moved.out << moved.err);
    REQUIRE(moved.code == ExitCode::Ok);
    CHECK(contains(moved.out, "Edited the plan of esde: 1 operation\n"));
    CHECK(contains(moved.out, "Done:            moved " + game + " from " + snes[0].slot.toString() + " to " + slot +
                                  ", pinned there\n"));
    CHECK(contains(moved.out, "Pins:            1 game pin, 0 fixture pins\n"));
    CHECK(contains(moved.out, "-edit ('worldplan undo' restores it)\n"));
    const std::string afterMove = *contentOf(session.files.plan);
    CHECK(afterMove != generated);
    const std::string overrides = *contentOf(session.files.overrides);
    CHECK(contains(overrides, "\"origin\": \"cli\""));
    CHECK(contains(overrides, "\"slot\": \"" + slot + "\""));
    CHECK(session.snapshots() == 1);
    // The catalog unchanged: the update finds the edited plan up to date and writes nothing.
    const RunResult update = session.run({"worldplan", "update", "--seed", "102938"});
    INFO(update.out << update.err);
    CHECK(update.code == ExitCode::Ok);
    CHECK(contains(update.out, "is up to date"));
    CHECK(contentOf(session.files.plan) == afterMove);
    // `generate --replace` honours the move's pin since I4b step 5 (test_cli_worldplan_regenerate.cpp).

    // Onto another game: refused by its rule, nothing written.
    const RunResult taken = session.run({"worldplan", "edit", "move", "--game", snes[1].game.toString(), "--slot", slot});
    CHECK(taken.code == ExitCode::ProblemsFound);
    CHECK(contains(taken.err, "operation 1 (move " + snes[1].game.toString() + " to " + slot + ") refused by slot-taken: "));
    CHECK(contains(taken.err, "nothing written"));
    const RunResult takenJson = session.run(
        {"worldplan", "edit", "move", "--game", snes[1].game.toString(), "--slot", slot, "--json"});
    CHECK(takenJson.code == ExitCode::ProblemsFound);
    CHECK(nlohmann::json::parse(takenJson.out)["refused"]["rule"] == "slot-taken");
    CHECK(contentOf(session.files.plan) == afterMove);
    CHECK(contentOf(session.files.overrides) == overrides);
    CHECK(session.snapshots() == 1);

    // A dry run writes nothing.
    const RunResult dry = session.run({"worldplan", "edit", "swap", "--game", snes[1].game.toString(), "--with",
                                       snes[2].game.toString(), "--dry-run"});
    INFO(dry.err);
    REQUIRE(dry.code == ExitCode::Ok);
    CHECK(contains(dry.out, "Would edit the plan of esde: 1 operation\n"));
    CHECK(contentOf(session.files.plan) == afterMove);
    CHECK(session.snapshots() == 1);

    // A script: a pin and a swap, one session, one snapshot.
    const std::filesystem::path script = session.dir.file("edits.json");
    REQUIRE(rv::core::writeTextFileAtomic(
                script, "// two edits\n{\"schemaVersion\": 1, \"operations\": [\n"
                        "  {\"op\": \"pin\", \"game\": \"" + snes[1].game.toString() + "\"},\n"
                        "  {\"op\": \"swap\", \"game\": \"" + snes[1].game.toString() + "\", \"with\": \"" +
                            snes[2].game.toString() + "\"}]}\n")
                .has_value());
    const RunResult scripted =
        session.run({"worldplan", "edit", "--script", rv::core::pathToUtf8(script), "--json"});
    INFO(scripted.out << scripted.err);
    REQUIRE(scripted.code == ExitCode::Ok);
    const nlohmann::json result = nlohmann::json::parse(scripted.out);
    CHECK(result["written"] == true);
    CHECK(result["changed"] == true);
    CHECK(result["operations"].size() == 2);
    CHECK(result["operations"][1]["op"] == "swap");
    CHECK(result["gamePins"] == 3);
    CHECK(result["snapshot"].is_string());
    CHECK(result["digestBefore"] == rv::world::planDigest(*rv::world::readPlan(afterMove, "plan")).value());
    CHECK(session.snapshots() == 2);
    auto swapped = rv::world::readPlan(*contentOf(session.files.plan), "plan");
    REQUIRE(swapped.has_value());
    for (const rv::world::Placement& placement : swapped->placements) {
        if (placement.game == snes[1].game) {
            CHECK(placement.slot == snes[2].slot);
        }
        if (placement.game == snes[2].game) {
            CHECK(placement.slot == snes[1].slot);
        }
    }

    // Undone: the plan and the overrides as the move left them.
    const RunResult undone = session.run({"worldplan", "undo"});
    INFO(undone.err);
    REQUIRE(undone.code == ExitCode::Ok);
    CHECK(contains(undone.out, "Undid edit of room:snes"));
    CHECK(contentOf(session.files.plan) == afterMove);
    CHECK(contentOf(session.files.overrides) == overrides);

    // A fixture moved off the room's grid: refused.
    const RunResult offGrid = session.run({"worldplan", "edit", "move-fixture", "--fixture", "room:snes/fx:dc01",
                                           "--x-mm", "-950", "--z-mm", "1000"});
    CHECK(offGrid.code == ExitCode::ProblemsFound);
    CHECK(contains(offGrid.err, "refused by off-grid: room:snes/fx:dc01: "));
    // Pinned where it stands, then unpinned.
    const RunResult pinned = session.run({"worldplan", "edit", "pin-fixture", "--fixture", "room:snes/fx:dc01"});
    INFO(pinned.err);
    REQUIRE(pinned.code == ExitCode::Ok);
    CHECK(contains(pinned.out, "Done:            pinned room:snes/fx:dc01 in its socket\n"));
    CHECK(contains(*contentOf(session.files.overrides), "\"fixture\": \"room:snes/fx:dc01\""));
    REQUIRE(session.run({"worldplan", "edit", "unpin-fixture", "--fixture", "room:snes/fx:dc01"}).code ==
            ExitCode::Ok);
    const RunResult nothing = session.run({"worldplan", "edit", "unpin-fixture", "--fixture", "room:snes/fx:dc01"});
    REQUIRE(nothing.code == ExitCode::Ok);
    CHECK(contains(nothing.out, "Nothing to change in the plan of esde"));

    // Usage errors.
    CHECK(session.run({"worldplan", "edit"}).code == ExitCode::UsageError);
    CHECK(session.run({"worldplan", "edit", "hide", "--game", game}).code == ExitCode::UsageError);
    CHECK(session.run({"worldplan", "edit", "move", "--game", game}).code == ExitCode::UsageError);
    CHECK(session.run({"worldplan", "edit", "pin", "--game", game, "--slot", slot}).code == ExitCode::UsageError);
    CHECK(session.run({"worldplan", "edit", "pin", "--game", "G42"}).code == ExitCode::UsageError);
    CHECK(session.run({"worldplan", "edit", "move-fixture", "--fixture", "room:snes/fx:dc01", "--x-mm", "0.5",
                       "--z-mm", "0"})
              .code == ExitCode::UsageError);
    CHECK(session.run({"worldplan", "edit", "pin", "--game", game, "--script", rv::core::pathToUtf8(script)}).code ==
          ExitCode::UsageError);
    const RunResult help = session.run({"worldplan", "edit", "--help"});
    CHECK(help.code == ExitCode::Ok);
    CHECK(contains(help.out, "move-fixture --fixture <id> --x-mm <n> --z-mm <n> [--yaw-cdeg <n>]"));
}
