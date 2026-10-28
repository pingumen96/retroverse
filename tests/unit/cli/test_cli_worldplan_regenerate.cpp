// retroverse-cli worldplan regenerate (I4b step 5, ADR-0015 §5, §8) on the building fixture's world: a dry run writing
// nothing (not even the lock file), its impact as JSON; a room regenerated behind a snapshot with its pin kept, the
// update then up to date, undone to the edited plan byte for byte; a floor; the world with --drop-overrides giving the
// fresh plan back; `generate --replace` honouring the pins as --world; the refusals and usage errors.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/cli/command.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/log.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/world/overrides.hpp>
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

/// Every file under `folder` with its bytes, by relative path.
std::map<std::string, std::string> folderContents(const std::filesystem::path& folder) {
    std::map<std::string, std::string> files;
    std::error_code error;
    for (auto it = std::filesystem::recursive_directory_iterator(folder, error);
         !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
        if (it->is_regular_file()) {
            auto bytes = rv::core::readBinaryFile(it->path());
            REQUIRE(bytes.has_value());
            files[rv::core::pathToUtf8(std::filesystem::relative(it->path(), folder))] =
                std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size());
        } else {
            files[rv::core::pathToUtf8(std::filesystem::relative(it->path(), folder)) + "/"] = "";
        }
    }
    REQUIRE_FALSE(error);
    return files;
}

struct Session {
    rv::test::TempDir dir{"rv-cli-worldplan-regenerate"};
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

/// The free spine-out slots of `room`.
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
                if (!taken.contains(slot)) {
                    slots.push_back(slot);
                }
            }
        }
    }
    return slots;
}

std::optional<rv::world::SlotId> slotOf(const std::string& planText, const std::string& game) {
    auto plan = rv::world::readPlan(planText, "plan");
    REQUIRE(plan.has_value());
    for (const rv::world::Placement& placement : plan->placements) {
        if (placement.game.toString() == game) {
            return placement.slot;
        }
    }
    return std::nullopt;
}

} // namespace

TEST_CASE("cli/worldplan regenerate: dry runs, a room, a floor and the world on the building fixture's world, undone",
          "[cli][worldplan]") {
    auto fixture = rv::test::Fixture::materialize("esde/building");
    INFO((fixture ? "materialized" : fixture.error().toString()));
    REQUIRE(fixture.has_value());
    const Session session;
    const LoggingReset loggingReset;
    session.importFixture(*fixture);

    // No plan yet.
    const RunResult early = session.run({"worldplan", "regenerate", "--world"});
    CHECK(early.code == ExitCode::ProblemsFound);
    CHECK(contains(early.err, "no plan to regenerate"));

    REQUIRE(session.run({"worldplan", "generate", "--seed", "102938"}).code == ExitCode::Ok);
    const std::string generated = *contentOf(session.files.plan);
    auto plan = rv::world::readPlan(generated, "plan");
    REQUIRE(plan.has_value());
    std::string game;
    for (const rv::world::Placement& placement : plan->placements) {
        if (game.empty() && placement.slot.fixture().room().toString() == "room:snes") {
            game = placement.game.toString();
        }
    }
    const std::vector<rv::world::SlotId> free = freeSpineSlots(*plan, "room:snes");
    REQUIRE_FALSE(free.empty());
    const std::string slot = free.back().toString();
    REQUIRE(session.run({"worldplan", "edit", "move", "--game", game, "--slot", slot}).code == ExitCode::Ok);
    const std::string edited = *contentOf(session.files.plan);
    const std::string overrides = *contentOf(session.files.overrides);
    REQUIRE(session.snapshots() == 1);

    // Usage errors and refusals: nothing written.
    CHECK(session.run({"worldplan", "regenerate"}).code == ExitCode::UsageError);
    CHECK(session.run({"worldplan", "regenerate", "--world", "--floor", "gen4"}).code == ExitCode::UsageError);
    CHECK(session.run({"worldplan", "regenerate", "--room", "nowhere"}).code == ExitCode::UsageError);
    const RunResult noRoom = session.run({"worldplan", "regenerate", "--room", "room:nes", "--seed", "102938"});
    CHECK(noRoom.code == ExitCode::ProblemsFound);
    CHECK(contains(noRoom.err, "the plan holds no room room:nes"));
    const RunResult noFloor = session.run({"worldplan", "regenerate", "--floor", "attic", "--seed", "102938"});
    CHECK(noFloor.code == ExitCode::ProblemsFound);
    CHECK(contains(noFloor.err, "the building has no floor attic"));
    // Another seed: only the world regenerates the plan.
    const RunResult otherSeed = session.run({"worldplan", "regenerate", "--room", "room:snes", "--seed", "7"});
    CHECK(otherSeed.code == ExitCode::ProblemsFound);
    CHECK(contains(otherSeed.err, "only the world's regeneration regenerates this plan"));
    CHECK(contentOf(session.files.plan) == edited);
    CHECK(session.snapshots() == 1);

    // A dry run: the impact, nothing written (the world's folder byte for byte, not even a lock file).
    const auto before = folderContents(session.files.folder);
    const RunResult dry = session.run(
        {"worldplan", "regenerate", "--room", "room:snes", "--seed", "102938", "--dry-run", "--json"});
    INFO(dry.out << dry.err);
    REQUIRE(dry.code == ExitCode::Ok);
    CHECK(folderContents(session.files.folder) == before);
    const nlohmann::json preview = nlohmann::json::parse(dry.out);
    CHECK(preview["scope"] == "room:snes");
    CHECK(preview["dryRun"] == true);
    CHECK(preview["written"] == false);
    CHECK(preview["snapshot"].is_null());
    CHECK(preview["impact"]["roomsRegenerated"] == nlohmann::json::array({"room:snes"}));
    CHECK(preview["impact"]["counts"]["pinsKept"] == 1);
    CHECK(preview["impact"]["counts"]["conflicts"] == 0);
    CHECK(preview["impact"]["pinsKept"][0] == "game " + game + " pinned to " + slot);
    CHECK(preview["timings"]["regenerationMs"].is_number());
    // The text summary of a dry run with --drop-overrides.
    const RunResult dryText = session.run(
        {"worldplan", "regenerate", "--world", "--seed", "102938", "--dry-run", "--drop-overrides"});
    INFO(dryText.out << dryText.err);
    REQUIRE(dryText.code == ExitCode::Ok);
    CHECK(contains(dryText.out, "Would regenerate world of the plan of "));
    CHECK(contains(dryText.out, "Overrides:       the scope's dropped\n"));
    CHECK(contains(dryText.out, "Pins:            0 kept, 0 conflicting, 1 dropped\n"));
    CHECK(contains(dryText.out, "Dropped:         game " + game + " pinned to " + slot + "\n"));
    CHECK(folderContents(session.files.folder) == before);

    // The room regenerated: behind a snapshot, its pin kept and honoured; the update then up to date.
    const RunResult room = session.run({"worldplan", "regenerate", "--room", "room:snes", "--seed", "102938"});
    INFO(room.out << room.err);
    REQUIRE(room.code == ExitCode::Ok);
    CHECK(contains(room.out, "Pins:            1 kept, 0 conflicting, 0 dropped\n"));
    CHECK(session.snapshots() == 2);
    const std::string regenerated = *contentOf(session.files.plan);
    CHECK(slotOf(regenerated, game) == rv::world::SlotId::parse(slot).value());
    CHECK(contentOf(session.files.overrides) == overrides);
    const RunResult update = session.run({"worldplan", "update", "--seed", "102938"});
    INFO(update.out << update.err);
    CHECK(update.code == ExitCode::Ok);
    CHECK(contains(update.out, "is up to date"));
    // Regenerated again: nothing to change, no snapshot.
    const RunResult again = session.run({"worldplan", "regenerate", "--room", "room:snes", "--seed", "102938"});
    REQUIRE(again.code == ExitCode::Ok);
    CHECK(session.snapshots() == 2);
    CHECK(contentOf(session.files.plan) == regenerated);
    // Undone: the edited plan byte for byte.
    REQUIRE(session.run({"worldplan", "undo"}).code == ExitCode::Ok);
    CHECK(contentOf(session.files.plan) == edited);
    CHECK(contentOf(session.files.overrides) == overrides);

    // A floor, then the world with --drop-overrides: the pin dropped, the fresh plan back.
    const RunResult floor = session.run({"worldplan", "regenerate", "--floor", "gen4", "--seed", "102938", "--json"});
    INFO(floor.out << floor.err);
    REQUIRE(floor.code == ExitCode::Ok);
    CHECK(nlohmann::json::parse(floor.out)["impact"]["counts"]["pinsKept"] == 1);
    const RunResult world =
        session.run({"worldplan", "regenerate", "--world", "--seed", "102938", "--drop-overrides"});
    INFO(world.out << world.err);
    REQUIRE(world.code == ExitCode::Ok);
    CHECK(contentOf(session.files.plan) == generated);
    auto dropped = rv::world::loadOverrides(session.files.overrides);
    REQUIRE(dropped.has_value());
    CHECK(dropped->empty());
}

TEST_CASE("cli/worldplan generate --replace: the world's regeneration, honouring the pins", "[cli][worldplan]") {
    auto fixture = rv::test::Fixture::materialize("esde/building");
    REQUIRE(fixture.has_value());
    const Session session;
    const LoggingReset loggingReset;
    session.importFixture(*fixture);
    REQUIRE(session.run({"worldplan", "generate", "--seed", "102938"}).code == ExitCode::Ok);
    auto plan = rv::world::readPlan(*contentOf(session.files.plan), "plan");
    REQUIRE(plan.has_value());
    std::string game;
    for (const rv::world::Placement& placement : plan->placements) {
        if (game.empty() && placement.slot.fixture().room().toString() == "room:snes") {
            game = placement.game.toString();
        }
    }
    const std::string slot = freeSpineSlots(*plan, "room:snes").back().toString();
    REQUIRE(session.run({"worldplan", "edit", "move", "--game", game, "--slot", slot}).code == ExitCode::Ok);
    const std::string edited = *contentOf(session.files.plan);
    const std::string overrides = *contentOf(session.files.overrides);
    // The pin of the move honoured: the plan written behind a snapshot, the pin kept.
    const RunResult replaced = session.run({"worldplan", "generate", "--seed", "102938", "--replace"});
    INFO(replaced.out << replaced.err);
    REQUIRE(replaced.code == ExitCode::Ok);
    CHECK_FALSE(contains(replaced.err, "pin-broken"));
    CHECK(contains(replaced.out, "-regenerate ('worldplan undo' restores it)\n"));
    CHECK(slotOf(*contentOf(session.files.plan), game) == rv::world::SlotId::parse(slot).value());
    CHECK(contentOf(session.files.overrides) == overrides);
    CHECK(session.snapshots() == 2);
    // The same as regenerate --world: nothing to change.
    const RunResult world = session.run({"worldplan", "regenerate", "--world", "--seed", "102938", "--json"});
    INFO(world.out << world.err);
    REQUIRE(world.code == ExitCode::Ok);
    CHECK(nlohmann::json::parse(world.out)["changed"] == false);
    CHECK(session.snapshots() == 2);
}
