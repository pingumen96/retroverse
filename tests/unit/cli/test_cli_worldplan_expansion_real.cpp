// The real library's building under generator 1.5 (I4b step 7, ADR-0015 §7): no room past its capacity, so no
// expansion room and no title unplaced for capacity, and a stored plan of generator 1.4 updated by 1.5 moving nothing.
// Hidden ([.real-library]): ctest never runs it; scripts/real-library.ps1 does, with ES-DE closed, read-only snapshots
// of the library before and after (0 differences), and the folders in the RV_REAL_* variables:
//   ./scripts/real-library.ps1 -Filter '[real-library][expansion]'
// The whole library is imported into a new catalog in a data directory under the build tree
// (build/<preset>/real-library/expansion/data, emptied first; library.json naming the real ES-DE home, install, ROM
// and media roots) and its building generated: no capacity entry, no expansion. The plan is then stored as generator
// 1.4 wrote it (its generator stamps 1.4, saved with world::savePlan() under the world lock: a building 1.4 and 1.5
// generate alike when no room is past its capacity) and `worldplan update` brings it to 1.5: written once (the stamps
// and the inputs' digest), nothing added, unplaced or moved, every room's elements byte for byte, still no capacity
// entry; the next update is up to date. The figures go to stdout ("Real expansion:" lines, for docs/i4b/step-7.md).
// Nothing derived from the user's titles is written outside that folder (docs/reference/safety.md: never under
// %TEMP%).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <system_error>
#include <tuple>
#include <vector>

#include <windows.h>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <rv/cli/command.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/log.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/world/generation.hpp>
#include <rv/world/plan_json.hpp>
#include <rv/world/world_store.hpp>

#include "support/fixture.hpp"

namespace fs = std::filesystem;
using rv::cli::ExitCode;
using rv::cli::Output;

namespace {

std::optional<fs::path> pathVariable(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size <= 1) {
        return std::nullopt;
    }
    std::wstring value(size, L'\0');
    value.resize(GetEnvironmentVariableW(name, value.data(), size));
    return fs::path(value);
}

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

std::size_t capacityEntries(const rv::world::WorldPlan& plan) {
    return static_cast<std::size_t>(std::count_if(plan.unplaced.begin(), plan.unplaced.end(), [](const auto& game) {
        return game.reason == rv::world::UnplacedReason::Capacity;
    }));
}

/// `plan` with every generator stamp `version` and the inputs' digest `digest` (what an update rewrites alone).
rv::world::WorldPlan stamped(rv::world::WorldPlan plan, rv::world::GeneratorVersion version,
                             const std::string& digest) {
    plan.generatorVersion = version;
    plan.inputDigest = digest;
    for (rv::world::Room& room : plan.rooms) {
        room.generatorVersion = version;
    }
    return plan;
}

} // namespace

TEST_CASE("cli/worldplan update: the real library's building under generator 1.5 moves nothing",
          "[cli][worldplan][expansion][.real-library]") {
    const auto home = pathVariable(L"RV_REAL_ESDE_HOME");
    const auto install = pathVariable(L"RV_REAL_ESDE_INSTALL");
    const auto romRoot = pathVariable(L"RV_REAL_ROM_ROOT");
    const auto mediaRoot = pathVariable(L"RV_REAL_MEDIA_ROOT");
    if (!home || !install || !romRoot || !mediaRoot) {
        SKIP("set RV_REAL_ESDE_HOME, RV_REAL_ESDE_INSTALL, RV_REAL_ROM_ROOT and RV_REAL_MEDIA_ROOT "
             "(scripts/real-library.ps1 does)");
    }
    const LoggingReset loggingReset;
    // Under the build tree, never in the library nor in %TEMP%.
    const fs::path folder = rv::test::testProgramsDir().parent_path() / "real-library" / "expansion";
    std::error_code removed;
    fs::remove_all(folder, removed);
    REQUIRE_FALSE(removed);
    fs::create_directories(folder, removed);
    REQUIRE_FALSE(removed);
    rv::core::PathEnvironment environment;
    environment.executableDir = folder;
    environment.sourceAssetDir = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);
    const auto run = [&](std::vector<std::string> arguments) {
        arguments.insert(arguments.begin(), {"--data-dir", rv::core::pathToUtf8(folder / "data")});
        Output out = Output::memory();
        Output err = Output::memory();
        const ExitCode code = rv::cli::run(arguments, out, err, environment);
        return std::make_tuple(code, out.text(), err.text());
    };
    using Clock = std::chrono::steady_clock;
    const auto seconds = [](Clock::time_point start) {
        return std::chrono::duration<double>(Clock::now() - start).count();
    };

    const auto [created, createdOut, createdErr] =
        run({"library", "create", "--home", rv::core::pathToUtf8(home->parent_path()), "--install",
             rv::core::pathToUtf8(*install), "--rom-root", rv::core::pathToUtf8(*romRoot), "--media-root",
             rv::core::pathToUtf8(*mediaRoot)});
    INFO(createdErr);
    REQUIRE(created == ExitCode::Ok);
    Clock::time_point start = Clock::now();
    const auto [imported, importedOut, importedErr] = run({"import"});
    INFO(importedErr);
    REQUIRE(imported == ExitCode::Ok);
    std::cout << fmt::format("Real expansion: the whole library imported in {:.1f} s\n", seconds(start));
    start = Clock::now();
    const auto [generated, generatedOut, generatedErr] = run({"worldplan", "generate", "--json"});
    INFO(generatedErr);
    REQUIRE(generated == ExitCode::Ok);
    const nlohmann::json summary = nlohmann::json::parse(generatedOut);
    const fs::path planFile = rv::core::pathFromUtf8(summary["plan"].get<std::string>());
    const rv::world::WorldFiles files = rv::world::WorldFiles::in(planFile.parent_path());
    auto text = rv::core::readTextFile(planFile);
    REQUIRE(text.has_value());
    auto fresh = rv::world::readPlan(*text, "plan");
    REQUIRE(fresh.has_value());
    // No room past its capacity: no expansion, no title unplaced for capacity.
    const std::size_t expansions = static_cast<std::size_t>(std::count_if(
        fresh->rooms.begin(), fresh->rooms.end(), [](const rv::world::Room& room) { return room.id.isExpansion(); }));
    std::map<std::string, std::size_t> perRoom;
    for (const rv::world::Placement& placement : fresh->placements) {
        ++perRoom[placement.slot.fixture().room().toString()];
    }
    const auto busiest = std::max_element(perRoom.begin(), perRoom.end(),
                                          [](const auto& a, const auto& b) { return a.second < b.second; });
    std::cout << fmt::format("Real expansion: the building generated in {:.1f} s: {} rooms on {} floors, {} placed, "
                             "{} unplaced, {} for capacity, {} expansion rooms; the busiest room {} ({} placed)\n",
                             seconds(start), fresh->rooms.size(), fresh->floors.size(), fresh->placements.size(),
                             fresh->unplaced.size(), capacityEntries(*fresh), expansions,
                             busiest != perRoom.end() ? busiest->first : std::string("none"),
                             busiest != perRoom.end() ? busiest->second : 0);
    CHECK(capacityEntries(*fresh) == 0);
    CHECK(expansions == 0);
    CHECK(fresh->generatorVersion == rv::world::kGeneratorVersion);

    // The plan as generator 1.4 wrote it, saved as the world's plan.
    const rv::world::GeneratorVersion previous{1, 4};
    {
        auto lock = rv::world::WorldLock::acquire(files);
        REQUIRE(lock.has_value());
        rv::world::GenerationResult result;
        result.plan = stamped(*fresh, previous, fresh->inputDigest);
        auto saved = rv::world::savePlan(*lock, result, {"rv_tests real-library expansion", {}, {}});
        REQUIRE(saved.has_value());
        REQUIRE(saved->written);
    }
    auto legacyText = rv::core::readTextFile(files.plan);
    REQUIRE(legacyText.has_value());
    auto legacy = rv::world::readPlan(*legacyText, "legacy");
    REQUIRE(legacy.has_value());
    REQUIRE(legacy->generatorVersion == previous);

    start = Clock::now();
    const auto [updated, updatedOut, updatedErr] = run({"worldplan", "update", "--json"});
    INFO(updatedErr);
    REQUIRE(updated == ExitCode::Ok);
    const double updateSeconds = seconds(start);
    const nlohmann::json report = nlohmann::json::parse(updatedOut);
    const nlohmann::json& changes = report["changes"];
    INFO(changes.dump());
    CHECK(changes["added"].empty());
    CHECK(changes["unplaced"].empty());
    CHECK(changes["roomsAdded"].empty());
    CHECK(changes["fixturesAdded"].empty());
    CHECK(changes["fixturesRemoved"].empty());
    CHECK(changes["fixturesMoved"].empty());
    auto afterText = rv::core::readTextFile(files.plan);
    REQUIRE(afterText.has_value());
    auto after = rv::world::readPlan(*afterText, "updated");
    REQUIRE(after.has_value());
    CHECK(after->generatorVersion == rv::world::kGeneratorVersion);
    CHECK(capacityEntries(*after) == 0);
    // Every room's elements byte for byte: the plans equal but for the generator stamps and the inputs' digest.
    CHECK(stamped(*after, previous, legacy->inputDigest) == *legacy);
    std::size_t roomsEqual = 0;
    for (const rv::world::Room& room : legacy->rooms) {
        auto before = rv::world::roomContentDigest(stamped(*legacy, previous, legacy->inputDigest), room.id);
        auto now = rv::world::roomContentDigest(stamped(*after, previous, legacy->inputDigest), room.id);
        REQUIRE(before.has_value());
        REQUIRE(now.has_value());
        roomsEqual += *before == *now ? 1 : 0;
    }
    CHECK(roomsEqual == legacy->rooms.size());
    std::cout << fmt::format("Real expansion: generator 1.4's plan updated by {} in {:.1f} s: {} of {} rooms' elements "
                             "byte for byte, {} capacity entries before and {} after\n",
                             rv::world::kGeneratorVersion.toString(), updateSeconds, roomsEqual, legacy->rooms.size(),
                             capacityEntries(*legacy), capacityEntries(*after));

    // The next update is up to date.
    const auto [again, againOut, againErr] = run({"worldplan", "update"});
    INFO(againErr);
    CHECK(again == ExitCode::Ok);
    CHECK(againOut.find("is up to date") != std::string::npos);
    auto lastText = rv::core::readTextFile(files.plan);
    REQUIRE(lastText.has_value());
    CHECK(*lastText == *afterText);
}
