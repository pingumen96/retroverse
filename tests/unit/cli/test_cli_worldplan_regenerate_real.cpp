// The real library's regenerations as dry runs (I4b step 5, ADR-0015 §5, §9): a room, a floor and the world of the
// building the library plans, through retroverse-cli worldplan regenerate --dry-run. Hidden ([.real-library]): ctest
// never runs it; scripts/real-library.ps1 does, with ES-DE closed, read-only snapshots of the library before and after
// (0 differences), and the folders in the RV_REAL_* variables:
//   ./scripts/real-library.ps1 -Filter '[real-library][regenerate]'
// The whole library is imported into a new catalog in a data directory under the build tree
// (build/<preset>/real-library/regenerate/data, emptied first; library.json naming the real ES-DE home, install, ROM
// and media roots), its building generated, then each scope's dry run: the room room:snes (else the largest room), the
// floor with the most placements and the world. Each exits 0, writes nothing (the world's folder
// byte for byte, no lock file), and, the catalog unchanged since the generation, changes nothing (the regeneration of
// a world without overrides equals its fresh plan). The preview's time (the rooms' data and the regeneration, as the
// app's job will run it on the pool; the catalog's selections apart) is checked against ADR-0015 §9's acceptable
// limits (2 s a room, 15 s the world) and printed ("Real regeneration:" lines, for docs/i4b/step-5.md). Nothing derived
// from the user's titles is written outside that folder (docs/reference/safety.md: never under %TEMP%).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
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
#include <rv/world/plan_json.hpp>
#include <rv/world/world_store.hpp>

#include "support/fixture.hpp"

namespace fs = std::filesystem;
using rv::cli::ExitCode;
using rv::cli::Output;

namespace {

/// ADR-0015 §9: the impact preview of a room within 2 s, of the world within 15 s, on the reference library.
constexpr double kRoomPreviewLimitMs = 2000.0;
constexpr double kWorldPreviewLimitMs = 15000.0;

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

/// Every file under `folder` with its bytes, by relative path.
std::map<std::string, std::string> folderContents(const fs::path& folder) {
    std::map<std::string, std::string> files;
    std::error_code error;
    for (auto it = fs::recursive_directory_iterator(folder, error); !error && it != fs::recursive_directory_iterator();
         it.increment(error)) {
        const std::string name = rv::core::pathToUtf8(fs::relative(it->path(), folder));
        if (it->is_regular_file()) {
            auto bytes = rv::core::readBinaryFile(it->path());
            REQUIRE(bytes.has_value());
            files[name] = std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size());
        } else {
            files[name + "/"] = "";
        }
    }
    REQUIRE_FALSE(error);
    return files;
}

} // namespace

TEST_CASE("cli/worldplan regenerate: the real library's dry runs of a room, a floor and the world write nothing",
          "[cli][worldplan][regenerate][.real-library]") {
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
    const fs::path folder = rv::test::testProgramsDir().parent_path() / "real-library" / "regenerate";
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
    std::cout << fmt::format("Real regeneration: the whole library imported in {:.1f} s\n", seconds(start));
    start = Clock::now();
    const auto [generated, generatedOut, generatedErr] = run({"worldplan", "generate", "--json"});
    INFO(generatedErr);
    REQUIRE(generated == ExitCode::Ok);
    std::cout << fmt::format("Real regeneration: the building generated in {:.1f} s\n", seconds(start));
    const nlohmann::json summary = nlohmann::json::parse(generatedOut);
    const fs::path planFile = rv::core::pathFromUtf8(summary["plan"].get<std::string>());
    const fs::path world = planFile.parent_path();
    auto text = rv::core::readTextFile(planFile);
    REQUIRE(text.has_value());
    auto plan = rv::world::readPlan(*text, "plan");
    REQUIRE(plan.has_value());

    // The scopes: room:snes (else the room with the most placements), the floor with the most placements, the world.
    std::map<std::string, std::size_t> roomPlacements;
    std::map<std::string, std::size_t> floorPlacements;
    std::map<std::string, std::string> floorOf;
    for (const rv::world::Room& room : plan->rooms) {
        floorOf[room.id.toString()] = room.floor;
    }
    for (const rv::world::Placement& placement : plan->placements) {
        const std::string room = placement.slot.fixture().room().toString();
        ++roomPlacements[room];
        ++floorPlacements[floorOf[room]];
    }
    std::string room = "room:snes";
    if (!roomPlacements.contains(room)) {
        room = std::max_element(roomPlacements.begin(), roomPlacements.end(), [](const auto& a, const auto& b) {
                   return a.second < b.second;
               })->first;
    }
    const std::string floor =
        std::max_element(floorPlacements.begin(), floorPlacements.end(), [](const auto& a, const auto& b) {
            return a.second < b.second;
        })->first;

    struct Scope {
        std::vector<std::string> arguments;
        std::string name;
        double limitMs = 0.0;
    };
    const std::vector<Scope> scopes{{{"--room", room}, room + " (" + std::to_string(roomPlacements[room]) + " placed)",
                                     kRoomPreviewLimitMs},
                                    {{"--floor", floor},
                                     "floor " + floor + " (" + std::to_string(floorPlacements[floor]) + " placed)",
                                     kWorldPreviewLimitMs},
                                    {{"--world"}, "the world (" + std::to_string(plan->placements.size()) + " placed)",
                                     kWorldPreviewLimitMs}};
    for (const Scope& scope : scopes) {
        INFO(scope.name);
        const auto before = folderContents(world);
        std::vector<std::string> arguments{"worldplan", "regenerate"};
        arguments.insert(arguments.end(), scope.arguments.begin(), scope.arguments.end());
        arguments.insert(arguments.end(), {"--dry-run", "--json"});
        start = Clock::now();
        const auto [code, out, err] = run(arguments);
        const double wall = seconds(start);
        INFO(err);
        REQUIRE(code == ExitCode::Ok);
        CHECK(folderContents(world) == before);
        const nlohmann::json result = nlohmann::json::parse(out);
        CHECK(result["written"] == false);
        // The catalog unchanged since the generation: nothing to change.
        CHECK(result["changed"] == false);
        const nlohmann::json& counts = result["impact"]["counts"];
        CHECK(counts["gamesMoved"] == 0);
        CHECK(counts["newlyPlaced"] == 0);
        CHECK(counts["newlyUnplaced"] == 0);
        CHECK(counts["fixturesAdded"] == 0);
        CHECK(counts["fixturesRemoved"] == 0);
        CHECK(counts["fixturesMoved"] == 0);
        CHECK(counts["roomsMoved"] == 0);
        const nlohmann::json& timings = result["timings"];
        const double previewMs = timings["dataMs"].get<double>() + timings["regenerationMs"].get<double>();
        std::cout << fmt::format("Real regeneration: {} dry run in {:.1f} s: the preview {:.0f} ms (data {:.0f} ms, "
                                 "regeneration {:.0f} ms; selections {:.0f} ms apart), {} room(s) regenerated, "
                                 "acceptable {:.0f} ms\n",
                                 scope.name, wall, previewMs, timings["dataMs"].get<double>(),
                                 timings["regenerationMs"].get<double>(), timings["selectionsMs"].get<double>(),
                                 counts["roomsRegenerated"].get<std::size_t>(), scope.limitMs);
        CHECK(previewMs <= scope.limitMs);
    }
}
