// The file watcher in retroverse.exe (I7 step 7, ADR-0018 §10), on the building fixture materialized under the build
// tree (tests/fixtures/esde/building: every folder the runs watch is the case's own, never the user's). The runs are
// real time (no --fixed-dt: the watcher's windows are the wall clock's) unless they launch, hidden, with
// --watch-library; the test acts on the fixture's folders while the app runs (StartedApp) and closes its window once
// the log says what it waited for:
//  - a ROM copied into roms/snes appears in the quick catalog within 10 s of its arrival (NFR-05's row: local volume,
//    nothing else running) through a partial refresh of snes, and the room places it in a new slot with nothing moved
//    (SC-05, NFR-08) once its cover, copied next, is there (a cartridge waits unplaced without one); an emulator's
//    save copied beside it wakes nothing; roms/gb renamed away and back keeps every identity (SC-06: the room's
//    placements equal);
//  - a ROM copied while the fake emulator's game runs starts nothing until the return; the refresh comes after it.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/app/run_report.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>
#include <rv/world/world_store.hpp>

#include "building_support.hpp"
#include "launch_support.hpp"
#include "smoke_support.hpp"

using namespace rv::smoke;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

/// The log's occurrences of `text` (a log another process writes, shared for reading).
std::size_t countIn(const fs::path& file, std::string_view text) {
    std::ifstream stream(file, std::ios::binary);
    const std::string content((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    std::size_t count = 0;
    for (std::size_t at = content.find(text); at != std::string::npos; at = content.find(text, at + text.size())) {
        ++count;
    }
    return count;
}

/// Waits until the log holds `text` at least `times` times; false after `timeout`.
bool waitForLog(const fs::path& log, std::string_view text, std::size_t times = 1,
                std::chrono::milliseconds timeout = 120s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (countIn(log, text) >= times) {
            return true;
        }
        std::this_thread::sleep_for(20ms);
    }
    return false;
}

std::uint64_t unixMilliseconds() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
}

/// A snes title of additions/ that the committed fixture lacks: its ROM and its cover, below the fixture's root.
struct Addition {
    std::string rom;
    std::string cover;
};

Addition snesAddition(const rv::test::Fixture& fixture) {
    for (const fs::directory_entry& entry : fs::directory_iterator(fixture.path("additions/roms/snes"))) {
        const fs::path name = entry.path().filename();
        if (entry.path().extension() != ".sfc" || fs::exists(fixture.path("roms/snes") / name)) {
            continue;
        }
        for (const char* extension : {".png", ".jpg"}) {
            const fs::path cover = fs::path(name).replace_extension(extension);
            if (fs::exists(fixture.path("additions/media/snes/covers") / cover)) {
                return Addition{"roms/snes/" + rv::core::pathToUtf8(name),
                                "media/snes/covers/" + rv::core::pathToUtf8(cover)};
            }
        }
    }
    FAIL("additions/ holds no new snes title with a cover");
    return {};
}

void copyIn(const rv::test::Fixture& fixture, const std::string& relative) {
    std::error_code error;
    fs::copy_file(fixture.path("additions/" + relative), fixture.path(relative), error);
    INFO(relative << ": " << error.message());
    REQUIRE_FALSE(error);
}

/// Closes the run's window (as the user closes it), then waits for its report.
AppRun closeAndFinish(StartedApp& app) {
    bool closed = false;
    for (int attempt = 0; attempt < 100 && !closed; ++attempt) {
        closed = app.closeWindow();
        if (!closed) {
            std::this_thread::sleep_for(50ms);
        }
    }
    REQUIRE(closed);
    return app.finish();
}

const rv::app::RefreshRecord* watcherRefresh(const rv::app::RunReport& report, std::size_t index) {
    std::size_t seen = 0;
    for (const rv::app::RefreshRecord& refresh : report.catalog->refreshes) {
        if (refresh.trigger == "watcher" && seen++ == index) {
            return &refresh;
        }
    }
    return nullptr;
}

/// The placements of `room` in the stored plan, as text: equal before and after means the same GameIds in the same
/// slots.
std::vector<std::string> placementsOf(const PlannedBuilding& building, std::string_view room) {
    const rv::world::StoredPlan stored = building.plan();
    std::vector<std::string> placements;
    const std::string prefix = std::string(room) + "/";
    for (const rv::world::Placement& placement : stored.plan.placements) {
        const std::string slot = placement.slot.toString();
        if (slot.starts_with(prefix)) {
            placements.push_back(placement.game.toString() + "@" + slot);
        }
    }
    std::sort(placements.begin(), placements.end());
    return placements;
}

} // namespace

TEST_CASE("watcher: a ROM copied in shows within 10 s and takes a new slot; its cover shows; a folder away and back "
          "keeps its ids",
          "[gpu][watcher]") {
    PlannedBuilding building("watcher/added");
    const Addition addition = snesAddition(building.fixture);
    const std::vector<std::string> gbBefore = placementsOf(building, "room:gb");
    REQUIRE_FALSE(gbBefore.empty());

    StartedApp app(building.dir, "watched",
                   {"--start-room", "room:snes", "--watch-library", "--exit-after-frames", "--frames", "1000000000",
                    "--log-level", "debug"});
    const fs::path log = building.dir / "watched.log";
    REQUIRE(waitForLog(log, "the room is shown"));
    REQUIRE(waitForLog(log, "root(s) armed:"));

    // The ROM, and a save an emulator would write beside it: only the ROM matters.
    const std::uint64_t arrival = unixMilliseconds();
    copyIn(building.fixture, addition.rom);
    REQUIRE(rv::core::writeTextFileAtomic(building.fixture.path(addition.rom + ".srm"), "save").has_value());
    REQUIRE(waitForLog(log, "the catalog's list after the refresh", 1, 60s));
    REQUIRE(waitForLog(log, "the room's live update (", 1, 60s));
    const std::uint64_t listed = unixMilliseconds();
    INFO("listed " << (listed - arrival) << " ms after the copy, as the test saw it");

    // Its cover next.
    copyIn(building.fixture, addition.cover);
    REQUIRE(waitForLog(log, "the catalog's list after the refresh", 2, 60s));
    REQUIRE(waitForLog(log, "the room's live update (", 2, 60s));

    // An emulator's save and state beside the ROM, alone: nothing starts.
    REQUIRE(rv::core::writeTextFileAtomic(building.fixture.path(addition.rom + ".srm"), "save 2").has_value());
    REQUIRE(rv::core::writeTextFileAtomic(building.fixture.path(addition.rom + ".state1"), "state").has_value());
    std::this_thread::sleep_for(4s);
    CHECK(countIn(log, "catalog refresh asked (watcher") == 2);

    // roms/gb renamed away (its games' source unavailable, nothing removed) and back (the same identities).
    fs::rename(building.fixture.path("roms/gb"), building.fixture.path("roms/gb.away"));
    REQUIRE(waitForLog(log, "the catalog's list after the refresh", 3, 60s));
    fs::rename(building.fixture.path("roms/gb.away"), building.fixture.path("roms/gb"));
    REQUIRE(waitForLog(log, "the catalog's list after the refresh", 4, 60s));
    REQUIRE(waitForLog(log, "the room's live update (", 4, 60s));
    std::this_thread::sleep_for(2s); // the covers' round settles

    const AppRun run = closeAndFinish(app);
    INFO(run.describe());
    REQUIRE(run.exitCode == 0);
    REQUIRE(run.report.has_value());
    const rv::app::RunReport& report = *run.report;
    REQUIRE(report.watcher.has_value());
    const rv::app::WatcherRecord& watcher = *report.watcher;
    CHECK(watcher.enabled);
    for (const rv::app::WatchRootRecord& root : watcher.roots) {
        INFO(root.name << ": " << root.mode << " " << root.reason);
        // custom_systems exists in the building fixture; every root is notified (NTFS under the build tree).
        CHECK(root.mode == "notify");
    }
    CHECK(watcher.refreshes == 4);
    CHECK(watcher.overflows == 0);
    const auto ignored = std::find_if(watcher.ignoredBy.begin(), watcher.ignoredBy.end(),
                                      [](const rv::app::WatchReasonCount& reason) { return reason.reason == "extension"; });
    REQUIRE(ignored != watcher.ignoredBy.end()); // the .srm
    CHECK(ignored->count >= 1);

    REQUIRE(report.catalog.has_value());
    const rv::app::RefreshRecord* added = watcherRefresh(report, 0);
    REQUIRE(added != nullptr);
    CHECK(added->outcome == "done");
    CHECK(added->systems == std::vector<std::string>{"snes"});
    CHECK(added->added == 1);
    CHECK(added->removed == 0);
    REQUIRE(added->firstChangeAt.has_value());
    REQUIRE(added->listedAt.has_value());
    CHECK(*added->firstChangeAt >= arrival);
    // NFR-05: in the quick catalog within 10 s of the file's arrival.
    INFO("from the copy to the list: " << (*added->listedAt - arrival) << " ms; first change seen after "
                                       << (*added->firstChangeAt - arrival) << " ms");
    CHECK(*added->listedAt - arrival <= 10000);
    REQUIRE(added->before.has_value());
    REQUIRE(added->after.has_value());

    const rv::app::RefreshRecord* cover = watcherRefresh(report, 1);
    REQUIRE(cover != nullptr);
    CHECK(cover->systems == std::vector<std::string>{"snes"});
    CHECK(cover->added == 0);
    for (std::size_t index : {2u, 3u}) {
        const rv::app::RefreshRecord* gb = watcherRefresh(report, index);
        REQUIRE(gb != nullptr);
        CHECK(gb->outcome == "done");
        CHECK(gb->systems == std::vector<std::string>{"gb"});
        CHECK(gb->added == 0);
        CHECK(gb->removed == 0);
    }

    // The room: a cartridge without its cover waits unplaced (noCover); its cover arrived, the title takes a free slot
    // with it, and nothing that stood moves (SC-05, NFR-08).
    const rv::app::WorldRecord& world = report.world;
    REQUIRE(world.liveUpdates.size() >= 2);
    CHECK(world.liveUpdates[0].trigger == "refresh");
    CHECK(world.liveUpdates[0].added == 0);
    const rv::app::WorldLiveUpdateRecord& placed = world.liveUpdates[1];
    CHECK(placed.trigger == "refresh");
    CHECK(placed.added == 1);
    CHECK(placed.moved == 0);
    CHECK(placed.removed == 0);
    for (const rv::app::WorldLiveUpdateRecord& update : world.liveUpdates) {
        CHECK(update.moved == 0);
        CHECK(update.removed == 0);
    }
    CHECK(world.items == 41);
    CHECK(world.noCover == 10); // mame's sets alone
    // SC-06: gb's folder away and back, its placements and GameIds as they were.
    CHECK(placementsOf(building, "room:gb") == gbBefore);
}

TEST_CASE("watcher: a ROM copied while the game runs starts nothing until the return", "[gpu][watcher]") {
    ensureLaunchAllowList();
    PlannedBuilding building("watcher/held");
    const Addition addition = snesAddition(building.fixture);
    // Every system launches the build's fake emulator for 6 s, which says when it started.
    const fs::path started = building.dir / "fake-started.json";
    nlohmann::json systems = nlohmann::json::object();
    for (const char* system : {"amiga", "gb", "mame", "pocketlab", "pokemini", "psx", "snes"}) {
        systems[system] = nlohmann::json::object();
    }
    const nlohmann::json profile{
        {"id", "fake-hold"},
        {"executable", arg(fakeEmulatorExe())},
        {"arguments",
         {"--sleep-ms", "6000", "--print-args", arg(started), "--exit-timeout-ms", "30000", "--", "{rom}"}},
        {"windowTimeoutSeconds", 0},
        {"hideConsoleWindow", true},
        {"systems", systems}};
    const nlohmann::json document{{"schemaVersion", 1}, {"profiles", nlohmann::json::array({profile})}};
    REQUIRE(rv::core::writeTextFileAtomic(building.data / "config" / "emulators.json", document.dump(2) + "\n")
                .has_value());
    for (const auto& [system, unused] : systems.items()) {
        static_cast<void>(unused);
        building.cli("assign_" + system, {"assign", system, "--profile", "fake-hold"});
    }

    // Lockstep, so that the script's frames find the quick catalog's list: Avvia on frame 8 launches its first game.
    StartedApp app(building.dir, "held",
                   {"--start-room", "room:snes", "--watch-library", "--fixed-dt", "0.016667", "--input-script",
                    arg(inputScript("quick_catalog_launch.json")), "--exit-after-frames", "--frames", "1000000000"});
    const fs::path log = building.dir / "held.log";
    REQUIRE(waitForLog(log, "root(s) armed:"));
    REQUIRE(waitForFile(started, 120s)); // the game runs
    copyIn(building.fixture, addition.rom);
    // During the game nothing starts, however long the changes have been quiet.
    std::this_thread::sleep_for(3s);
    CHECK(countIn(log, "catalog refresh asked (watcher") == 0);
    REQUIRE(waitForLog(log, "catalog refresh asked (watcher", 1, 60s));
    REQUIRE(waitForLog(log, "the catalog's list after the refresh", 1, 60s));

    const AppRun run = closeAndFinish(app);
    INFO(run.describe());
    REQUIRE(run.exitCode == 0);
    REQUIRE(run.report.has_value());
    const rv::app::RunReport& report = *run.report;
    REQUIRE(report.launches.size() == 1);
    const rv::app::LaunchRecord& launch = report.launches.front();
    CHECK(launch.state == "Returned");
    REQUIRE(launch.endFrame.has_value());
    REQUIRE(report.catalog.has_value());
    const rv::app::RefreshRecord* refresh = watcherRefresh(report, 0);
    REQUIRE(refresh != nullptr);
    CHECK(refresh->held);
    CHECK(refresh->requestFrame >= *launch.endFrame);
    CHECK(refresh->added == 1);
    REQUIRE(report.watcher.has_value());
    CHECK(report.watcher->held >= 1);
    CHECK(report.watcher->refreshes == 1);
}

// By hand, for a look: rv_smoke_tests "[watcher_shots]" with RV_SHOTS_DIR naming a folder writes the snes room before a
// ROM and its cover are copied in and after the watcher placed them (a fresh lockstep start each, 1920 x 1080). Not a
// check: visual verification (docs/reference/visual-verification.md).
TEST_CASE("watcher: the pictures", "[.watcher_shots]") {
    const char* folder = std::getenv("RV_SHOTS_DIR");
    if (folder == nullptr) {
        SKIP("RV_SHOTS_DIR names no folder");
    }
    const fs::path out = rv::core::pathFromUtf8(folder);
    fs::create_directories(out);
    PlannedBuilding building("watcher/shots");
    const Addition addition = snesAddition(building.fixture);
    const auto shoot = [&](std::string_view name) {
        const fs::path shot = building.dir / (std::string(name) + ".png");
        const AppRun run = runApp(retroverseExe(), building.dir, name,
                                  {"--start-room", "room:snes", "--exit-after-frames", "--frames", "240", "--fixed-dt",
                                   "0.016667", "--width", "1920", "--height", "1080", "--screenshot", arg(shot)});
        requireCleanRun(run);
        fs::copy_file(shot, out / ("watcher_" + std::string(name) + ".png"), fs::copy_options::overwrite_existing);
        return run;
    };
    static_cast<void>(shoot("before"));
    StartedApp app(building.dir, "watched",
                   {"--start-room", "room:snes", "--watch-library", "--exit-after-frames", "--frames", "1000000000"});
    const fs::path log = building.dir / "watched.log";
    REQUIRE(waitForLog(log, "root(s) armed:"));
    copyIn(building.fixture, addition.rom);
    copyIn(building.fixture, addition.cover);
    REQUIRE(waitForLog(log, "the room's live update (", 1, 60s));
    const AppRun watched = closeAndFinish(app);
    REQUIRE(watched.exitCode == 0);
    const AppRun after = shoot("after");
    CHECK(after.report->world.items == 41);
}
