// The app's "Rigenera stanza / piano / mondo" previews on the real library (I4b step 6; ADR-0015 §5, §9): the jobs the
// app runs on the pool (rv/app/world_regenerate.hpp) on the building the real library plans. Hidden ([.real-library]):
// ctest never runs it; scripts/real-library.ps1 does, with ES-DE closed, read-only snapshots of the library before and
// after (0 differences), and the folders in the RV_REAL_* variables:
//   ./scripts/real-library.ps1 -Filter '[real-library][regenerate-app]'
// The whole library is imported into a new catalog in a data directory under the build tree
// (build/<preset>/real-library/regenerate-app/data, emptied first; library.json naming the real ES-DE home, install,
// ROM and media roots) and its building generated with retroverse-cli, as step 5's test does; then the app's path: the
// catalog's inputs (its building snapshot and every system's selection, takeRegenerationInputs(), what the directory's
// "Modifiche" tab reads ahead) and the previews of room:snes (else the largest room), the floor with the most
// placements and the world on them (previewRegeneration(): the world read without the lock, the dry run, the panel's
// content). Each writes nothing (the world's folder byte for byte) and, the catalog unchanged since the generation,
// changes nothing. The times are checked against ADR-0015 §9's acceptable limits (the preview of a room 2 s, of a floor
// or the world 15 s; the inputs counted apart and with the first) and printed ("Real app regeneration:" lines, for
// docs/i4b/step-6.md). Nothing derived from the user's titles is written outside that folder.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <tuple>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <windows.h>

#include <rv/app/room_snapshot.hpp>
#include <rv/app/world_build.hpp>
#include <rv/app/world_regenerate.hpp>
#include <rv/catalog_db/catalog_query.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/catalog_db/migrator.hpp>
#include <rv/cli/command.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/log.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/esde/discovery.hpp>
#include <rv/world/plan_json.hpp>
#include <rv/world/world_store.hpp>

#include "support/fixture.hpp"

namespace fs = std::filesystem;
using rv::cli::ExitCode;
using rv::cli::Output;

namespace {

/// ADR-0015 §9: the impact preview of a room within 2 s, of a floor or the world within 15 s, on the reference library.
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

TEST_CASE("app/world_regenerate: the real library's previews of a room, a floor and the world, as the app runs them, "
          "write nothing",
          "[app][world_regenerate][regenerate-app][.real-library]") {
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
    const fs::path folder = rv::test::testProgramsDir().parent_path() / "real-library" / "regenerate-app";
    std::error_code removed;
    fs::remove_all(folder, removed);
    REQUIRE_FALSE(removed);
    fs::create_directories(folder, removed);
    REQUIRE_FALSE(removed);
    rv::core::PathEnvironment environment;
    environment.executableDir = folder;
    environment.sourceAssetDir = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);
    const fs::path data = folder / "data";
    const auto run = [&](std::vector<std::string> arguments) {
        arguments.insert(arguments.begin(), {"--data-dir", rv::core::pathToUtf8(data)});
        Output out = Output::memory();
        Output err = Output::memory();
        const ExitCode code = rv::cli::run(arguments, out, err, environment);
        return std::make_tuple(code, out.text(), err.text());
    };
    using Clock = std::chrono::steady_clock;
    const auto milliseconds = [](Clock::time_point start) {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
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
    std::cout << fmt::format("Real app regeneration: the whole library imported in {:.1f} s\n",
                             milliseconds(start) / 1000.0);
    start = Clock::now();
    const auto [generated, generatedOut, generatedErr] = run({"worldplan", "generate", "--json"});
    INFO(generatedErr);
    REQUIRE(generated == ExitCode::Ok);
    std::cout << fmt::format("Real app regeneration: the building generated in {:.1f} s\n",
                             milliseconds(start) / 1000.0);
    const nlohmann::json summary = nlohmann::json::parse(generatedOut);
    const fs::path planFile = rv::core::pathFromUtf8(summary["plan"].get<std::string>());
    const fs::path world = planFile.parent_path();
    auto text = rv::core::readTextFile(planFile);
    REQUIRE(text.has_value());
    auto plan = rv::world::readPlan(*text, "plan");
    REQUIRE(plan.has_value());

    // The app's world reader on the catalog, as the catalog worker opens it.
    rv::core::PathOptions options;
    options.userDataOverride = data;
    const rv::core::Paths paths = rv::core::Paths::resolve(options, environment).value();
    auto database = rv::catalog_db::openCatalogDatabase(rv::catalog_db::databaseFile(paths));
    REQUIRE(database.has_value());
    auto reader = std::make_shared<rv::app::WorldCatalogReader>();
    reader->query = rv::catalog_db::SqliteCatalogQuery::open(database->openReader().value()).value();
    auto profile = rv::esde::readLibraryFile(paths.configDir() / "library.json");
    REQUIRE(profile.has_value());
    reader->profile = *profile;
    const rv::app::WorldPaths worldPaths{paths.assetRoot(), paths.userDataRoot(), paths.configDir(), paths.cacheDir()};

    // The inputs, as the "Modifiche" tab reads them ahead (or the first preview takes them).
    const auto before = folderContents(world);
    start = Clock::now();
    const rv::app::RegenerationInputsResult inputs = rv::app::takeRegenerationInputs(worldPaths, *reader);
    const double inputsMs = milliseconds(start);
    INFO(inputs.error);
    REQUIRE(inputs.inputs);
    std::cout << fmt::format("Real app regeneration: the catalog's inputs in {:.0f} ms (the snapshot {:.0f} ms, {} "
                             "selections {:.0f} ms)\n",
                             inputsMs, inputs.inputs->snapshotMs, inputs.inputs->selections.size(),
                             inputs.inputs->selectMs);

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
        rv::world::RegenerationScope scope;
        std::string name;
        double limitMs = 0.0;
    };
    const std::vector<Scope> scopes{
        {rv::world::RegenerationScope::ofRoom(rv::world::RoomId::parse(room).value()),
         room + " (" + std::to_string(roomPlacements[room]) + " placed)", kRoomPreviewLimitMs},
        {rv::world::RegenerationScope::ofFloor(floor),
         "floor " + floor + " (" + std::to_string(floorPlacements[floor]) + " placed)", kWorldPreviewLimitMs},
        {rv::world::RegenerationScope::ofWorld(), "the world (" + std::to_string(plan->placements.size()) + " placed)",
         kWorldPreviewLimitMs},
        // The room again, its rooms' data read by the previews before it: a warm preview.
        {rv::world::RegenerationScope::ofRoom(rv::world::RoomId::parse(room).value()), room + " again (warm)",
         kRoomPreviewLimitMs}};
    bool first = true;
    for (const Scope& scope : scopes) {
        INFO(scope.name);
        start = Clock::now();
        const rv::app::RegenerationPreview preview =
            rv::app::previewRegeneration(worldPaths, scope.scope, *inputs.inputs);
        const double wall = milliseconds(start);
        INFO(preview.error);
        REQUIRE(preview.failure.empty());
        CHECK(folderContents(world) == before);
        // The catalog unchanged since the generation: nothing to change.
        const rv::ui::RegeneratePreviewContent& content = preview.content;
        CHECK_FALSE(content.changed);
        CHECK(content.gamesMoved == 0);
        CHECK(content.newlyPlaced == 0);
        CHECK(content.newlyUnplaced == 0);
        CHECK(content.fixturesAdded == 0);
        CHECK(content.fixturesRemoved == 0);
        CHECK(content.fixturesMoved == 0);
        CHECK(content.roomsMoved == 0);
        // The first preview of a run also waits for the inputs (unless the tab read them ahead).
        const double withInputs = wall + (first ? inputsMs : 0.0);
        std::cout << fmt::format("Real app regeneration: {} previewed in {:.0f} ms (read {:.0f} ms, regeneration "
                                 "{:.0f} ms){}, {} room(s) regenerated, acceptable {:.0f} ms\n",
                                 scope.name, wall, preview.readMs, preview.regenerateMs,
                                 first ? fmt::format("; {:.0f} ms with the inputs taken first", withInputs)
                                       : std::string(),
                                 content.roomsRegenerated, scope.limitMs);
        CHECK(wall <= scope.limitMs);
        first = false;
    }
}
