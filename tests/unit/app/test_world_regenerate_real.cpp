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
// docs/i4b/step-6.md), with the memory the inputs hold (the process's private bytes before and after).
// Then (I4b step 6's review) the app's own commits in that world: a game of room:snes and one of the computer floor's
// busiest room that has a free slot moved by the CLI (pinned there: Rigenera then changes the room), the building shown
// once by retroverse.exe of this build (hidden, lockstep: the covers into the cache, the building's job run), then in
// the app "Rigenera stanza" committed and "Annulla ultima modifica" committed in each room, and "Rigenera piano" of
// room:snes's floor: the commit's job and the room shown again (from the choice to the frame that shows it: ADR-0015
// §9's 3 s warm, checked for the rooms) printed with the path that showed it. Nothing derived from the user's titles is
// written outside that folder.

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

#include <psapi.h>

#include <rv/app/room_snapshot.hpp>
#include <rv/app/run_report.hpp>
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
#include <rv/world/collection_layout.hpp>
#include <rv/world/plan_json.hpp>
#include <rv/world/scene_builder.hpp>
#include <rv/world/world_store.hpp>

#include "support/child_process.hpp"
#include "support/fixture.hpp"

namespace fs = std::filesystem;
using rv::cli::ExitCode;
using rv::cli::Output;

namespace {

/// ADR-0015 §9: the impact preview of a room within 2 s, of a floor or the world within 15 s, on the reference library;
/// a regeneration's commit or an undo: the room shown again within 3 s warm.
constexpr double kRoomPreviewLimitMs = 2000.0;
constexpr double kWorldPreviewLimitMs = 15000.0;
constexpr double kCommitShownLimitMs = 3000.0;

/// The process's private bytes now (its commit charge).
std::size_t privateBytes() {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters)) == 0) {
        return 0;
    }
    return counters.PrivateUsage;
}

/// retroverse.exe of this build on `data`, hidden, lockstep, at `room`'s doorway, with the input script `steps`
/// ({frame, actions}) for `frames` frames; its report (<folder>/<name>.json) read back.
rv::app::RunReport runApp(const fs::path& folder, const fs::path& data, const std::string& name,
                          const std::string& room, const std::vector<std::pair<int, std::string>>& steps, int frames) {
    const fs::path script = folder / (name + "-script.json");
    {
        std::string text = "[\n  {\"fromFrame\": 5, \"toFrame\": 24, \"actions\": {\"move_forward\": 1}}";
        for (const auto& [frame, actions] : steps) {
            text += fmt::format(",\n  {{\"fromFrame\": {0}, \"toFrame\": {0}, \"actions\": {{{1}}}}}", frame, actions);
        }
        text += "\n]\n";
        REQUIRE(rv::core::writeTextFileAtomic(script, text).has_value());
    }
    const fs::path report = folder / (name + ".json");
    rv::test::ChildRequest request;
    request.executable = rv::test::testProgramsDir() / "retroverse.exe";
    request.arguments = {"--start-room", room, "--hidden", "--data-dir", rv::core::pathToUtf8(data),
                         "--exit-after-frames", "--frames", std::to_string(frames), "--fixed-dt", "0.016667",
                         "--run-report", rv::core::pathToUtf8(report)};
    if (!steps.empty()) {
        request.arguments.insert(request.arguments.end(), {"--input-script", rv::core::pathToUtf8(script)});
    }
    request.stdoutFile = folder / (name + ".log");
    request.stderrFile = request.stdoutFile;
    request.environment.push_back({"RV_ASSET_ROOT", std::nullopt});
    request.timeout = std::chrono::minutes(10);
    const auto run = rv::test::runChild(request);
    REQUIRE(run.has_value());
    INFO(name << ": exit code " << run->exitCode << (run->timedOut ? " (timed out)" : "") << "; "
              << rv::core::pathToUtf8(request.stdoutFile));
    REQUIRE_FALSE(run->timedOut);
    CHECK(run->exitCode == 0);
    auto read = rv::app::readRunReport(report);
    REQUIRE(read.has_value());
    return std::move(*read);
}

/// The input script's steps to the directory's "Modifiche" tab (M, the right shoulder twice) and its last row (End).
std::vector<std::pair<int, std::string>> toChanges() {
    return {{30, R"("map": 1)"},
            {36, R"("ui_filter_next": 1)"},
            {42, R"("ui_filter_next": 1)"},
            {48, R"("ui_end": 1)"}};
}

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
    const std::size_t bytesBefore = privateBytes();
    start = Clock::now();
    const rv::app::RegenerationInputsResult inputs = rv::app::takeRegenerationInputs(worldPaths, *reader);
    const double inputsMs = milliseconds(start);
    INFO(inputs.error);
    REQUIRE(inputs.inputs);
    const std::size_t bytesAfter = privateBytes();
    std::cout << fmt::format("Real app regeneration: the catalog's inputs in {:.0f} ms (the snapshot {:.0f} ms, {} "
                             "selections {:.0f} ms), {:.1f} MiB of private bytes held ({:.1f} MiB before)\n",
                             inputsMs, inputs.inputs->snapshotMs, inputs.inputs->selections.size(),
                             inputs.inputs->selectMs,
                             (static_cast<double>(bytesAfter) - static_cast<double>(bytesBefore)) / (1024.0 * 1024.0),
                             static_cast<double>(bytesBefore) / (1024.0 * 1024.0));

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

    // I4b step 6's review: the app's commits in this world (the file comment). A game of each room moved into a free
    // slot of another fixture by the CLI, pinned there.
    const auto customize = [&](const std::string& roomId) {
        auto stored = rv::world::readStoredPlan(planFile);
        REQUIRE(stored.has_value());
        const rv::world::RoomId id = rv::world::RoomId::parse(roomId).value();
        auto data = rv::world::loadSceneData(paths.assetRoot(), stored->plan, id);
        if (!data) {
            return false;
        }
        auto layout = rv::world::collectionLayout(stored->plan, id, *data, {});
        if (!layout) {
            return false;
        }
        for (const rv::world::FreeSlot& free : layout->freeSlots) {
            for (const rv::world::CollectionItem& item : layout->items) {
                if (item.slot.fixture() == free.slot.fixture() || item.stance != free.stance) {
                    continue;
                }
                const auto [moved, movedOut, movedErr] = run({"worldplan", "edit", "move", "--game",
                                                              item.game.toString(), "--slot", free.slot.toString()});
                return moved == ExitCode::Ok;
            }
        }
        return false;
    };
    REQUIRE(customize(room));
    // The computer floor's busiest room that takes a move (else none: its row is left out).
    std::vector<std::pair<std::size_t, std::string>> computer;
    for (const auto& [id, placed] : roomPlacements) {
        if (floorOf[id] == "computer") {
            computer.emplace_back(placed, id);
        }
    }
    std::sort(computer.rbegin(), computer.rend());
    std::string computerRoom;
    for (const auto& [placed, id] : computer) {
        if (customize(id)) {
            computerRoom = id;
            break;
        }
    }
    const fs::path appFolder = folder / "app";
    fs::create_directories(appFolder, removed);
    REQUIRE_FALSE(removed);
    struct Commit {
        std::string name;
        std::string scope;
        std::optional<double> commitMs;
        std::optional<double> backMs;
        std::string path;
    };
    std::vector<Commit> commits;
    const auto commitOf = [&](const rv::app::RunReport& report, const std::string& name, const std::string& choice) {
        REQUIRE(report.world.regenerations.size() == 1);
        const rv::app::WorldRegenerationRecord& record = report.world.regenerations.front();
        INFO(name << ": " << record.kind << " " << record.scope << ", failure " << record.failure << ", choice "
                  << record.choice);
        CHECK(record.failure.empty());
        CHECK(record.choice == choice);
        CHECK(record.end == "committed");
        CHECK(record.written);
        commits.push_back(Commit{name, record.scope, record.commitMs, record.backMs, record.path});
        std::cout << fmt::format("Real app regeneration: {} of {}: the commit's job {:.0f} ms, the room shown again "
                                 "{:.0f} ms after the choice (path {}), acceptable {:.0f} ms\n",
                                 name, record.scope, record.commitMs.value_or(-1.0), record.backMs.value_or(-1.0),
                                 record.path, kCommitShownLimitMs);
        return record;
    };
    for (const std::string& shownRoom : {room, computerRoom}) {
        if (shownRoom.empty()) {
            continue;
        }
        const std::string tag = shownRoom.substr(shownRoom.rfind(':') + 1);
        // The building shown once (the covers into the cache, the building's job run and its stamp written).
        static_cast<void>(runApp(appFolder, data, "warm-" + tag, shownRoom, {}, 300));
        // "Rigenera stanza" (End, then up past "Rigenera mondo" and "Rigenera piano"), Rigenera once its preview shows.
        std::vector<std::pair<int, std::string>> regenerate = toChanges();
        regenerate.insert(regenerate.end(), {{52, R"("ui_up": 1)"},
                                             {56, R"("ui_up": 1)"},
                                             {60, R"("ui_up": 1)"},
                                             {66, R"("ui_activate": 1)"},
                                             {110, R"("ui_activate": 1)"}});
        const rv::app::RunReport regenerated = runApp(appFolder, data, "regenerate-" + tag, shownRoom, regenerate, 160);
        const rv::app::WorldRegenerationRecord done = commitOf(regenerated, "Rigenera stanza", "regenerate");
        CHECK(done.backMs.value_or(1e9) <= kCommitShownLimitMs);
        // "Annulla ultima modifica" (the last row; its preview focused on "Indietro": Up to "Annulla la modifica").
        std::vector<std::pair<int, std::string>> undo = toChanges();
        undo.insert(undo.end(), {{54, R"("ui_activate": 1)"}, {92, R"("ui_up": 1)"}, {96, R"("ui_activate": 1)"}});
        const rv::app::RunReport undone = runApp(appFolder, data, "undo-" + tag, shownRoom, undo, 150);
        const rv::app::WorldRegenerationRecord back = commitOf(undone, "Annulla ultima modifica", "undo");
        CHECK(back.backMs.value_or(1e9) <= kCommitShownLimitMs);
    }
    // "Rigenera piano" of room:snes's floor (its pinned game: the floor changes), shown again.
    std::vector<std::pair<int, std::string>> floorSteps = toChanges();
    floorSteps.insert(floorSteps.end(), {{52, R"("ui_up": 1)"},
                                         {56, R"("ui_up": 1)"},
                                         {62, R"("ui_activate": 1)"},
                                         {110, R"("ui_activate": 1)"}});
    const rv::app::RunReport floorRun = runApp(appFolder, data, "regenerate-floor", room, floorSteps, 160);
    REQUIRE(floorRun.world.regenerations.size() == 1);
    if (floorRun.world.regenerations.front().changed) {
        static_cast<void>(commitOf(floorRun, "Rigenera piano", "regenerate"));
    } else {
        WARN("Rigenera piano of " << floorOf[room] << " found nothing to change: no floor commit measured");
    }
}
