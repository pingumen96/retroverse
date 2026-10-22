// The real SNES room checked end to end (I3 step 17b). Hidden ([.real-library]): ctest never runs it; scripts/real-
// library.ps1 does, with ES-DE closed, read-only snapshots of the library before and after, and the folders in the
// RV_REAL_* variables. The system snes alone is imported into a new catalog under the build tree
// (build/<preset>/real-library/room/data, library.json naming the real ES-DE home, install, ROM and media roots; a
// fresh plan of the template's current version), then retroverse.exe of this build (hidden, frame-limited) runs on that
// data directory:
//   cold       no plan, an empty derived-texture cache: the plan generated and written, every cover made (1920 x 1080,
//              --no-vsync, no lockstep: the residency as a first start sees it)
//   again      the plan deleted, the cache kept: the plan generated a second time, byte for byte the first (its
//              SHA-256 and the file's), every layer from the cache (warm)
//   restart    after a second import of the same library: the stored plan shown and reconciled, the update leaving it
//              as it was (no rebuild, 0 moves), warm
//   benchmark  tests/input_scripts/bench_snes_room.json at 1920 x 1080, --no-vsync, --fixed-dt (the "benchmark"
//              phase's frame times and GPU passes)
//   shots      screenshots for the review, lockstep: the doorway, a side wall's spine-out shelf at 1 m, the hero band
//              at 1.2 m, a band item inspected (tests/input_scripts/world_inspect.json up to the item held, its card
//              beside it), all under the build tree only (the user's covers: never committed)
//   refresh    (I3 step 18) --refresh-library with the quick catalog opened on frame 2 (catalog_open.json): the whole
//              library imported on the catalog worker while the room shows (the "refresh" phase's frame times, NFR-04),
//              the quick catalog's first page timed from the process's creation (NFR-01 with the room), the room
//              following the refreshed catalog without moving anything
// The figures are printed for docs/i3/step-17b.md (lines starting "Real snes room 17b:"); scripts/gate.ps1 -Gate B
// -RealLibrary reads the run reports (<name>.json) against the budgets. The checks hold whatever the collection holds
// (this machine's figures are not versioned): every run clean, every item's layers resident, the plan reproduced, the
// restart and the refresh moving nothing.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

#include <fmt/format.h>
#include <glm/glm.hpp>

#include <rv/app/run_report.hpp>
#include <rv/app/world_focus.hpp>
#include <rv/catalog/import_service.hpp>
#include <rv/catalog/reconciliation.hpp>
#include <rv/catalog_db/catalog_store.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/catalog_db/migrator.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/math.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/sha256.hpp>
#include <rv/core/text.hpp>
#include <rv/esde/discovery.hpp>
#include <rv/esde/library_source.hpp>
#include <rv/esde/media_resolver.hpp>
#include <rv/esde/settings.hpp>
#include <rv/world/collection_layout.hpp>
#include <rv/world/plan_json.hpp>
#include <rv/world/scene_builder.hpp>
#include <rv/world/world_store.hpp>

#include "support/child_process.hpp"
#include "support/fixture.hpp"

namespace fs = std::filesystem;

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

/// retroverse.exe with `arguments` on `data`, hidden, its report in <folder>/<name>.json; the report read back.
rv::app::RunReport runRoom(const fs::path& folder, const fs::path& data, const std::string& name,
                           std::vector<std::string> arguments) {
    const fs::path report = folder / (name + ".json");
    // The snes room (I4 step 5a: the building starts in its lobby otherwise), its frame the --spawn's.
    arguments.insert(arguments.end(), {"--start-room", "room:snes", "--hidden", "--data-dir",
                                       rv::core::pathToUtf8(data), "--run-report", rv::core::pathToUtf8(report)});
    rv::test::ChildRequest request;
    request.executable = rv::test::testProgramsDir() / "retroverse.exe";
    request.arguments = std::move(arguments);
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

std::string ms(const std::optional<double>& value) {
    return value ? fmt::format("{:.0f} ms", *value) : std::string("-");
}

/// The snes system imported into the catalog of `data` (created or updated); the import's result.
rv::catalog::ImportResult importSnes(const rv::catalog::LibraryProfile& profile, const fs::path& data) {
    rv::core::PathOptions options;
    options.userDataOverride = data;
    rv::core::PathEnvironment environment;
    environment.executableDir = rv::test::testProgramsDir();
    environment.sourceAssetDir = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);
    const rv::core::Paths paths = rv::core::Paths::resolve(options, environment).value();
    auto database = rv::catalog_db::openCatalogDatabase(rv::catalog_db::databaseFile(paths));
    REQUIRE(database.has_value());
    auto store = rv::catalog_db::SqliteCatalogStore::open(*database);
    REQUIRE(store.has_value());
    const auto settings = rv::esde::readSettings(profile.esdeHome);
    REQUIRE(settings.has_value());
    rv::esde::EsdeLibrarySourceOptions sourceOptions;
    sourceOptions.systems = {"snes"};
    rv::esde::EsdeLibrarySource source(sourceOptions);
    rv::esde::EsdeMediaResolver media(rv::esde::resolveMediaRoot(*settings, profile));
    rv::catalog::IdentityPairing pairing;
    rv::catalog::ImportService service(source, media, **store, &pairing);
    rv::catalog::ImportOptions importOptions;
    importOptions.systems = {rv::catalog::SystemName::parse("snes").value()};
    auto imported = service.run(profile, importOptions);
    INFO((imported ? std::string() : imported.error().toString()));
    REQUIRE(imported.has_value());
    REQUIRE(imported->statistics.mediaUnknown == 0);
    return std::move(*imported);
}

/// The SHA-256 of the plan file's bytes.
std::string fileSha256(const fs::path& file) {
    const auto bytes = rv::core::readBinaryFile(file);
    REQUIRE(bytes.has_value());
    return rv::core::sha256Hex(std::span<const std::byte>(*bytes));
}

/// "x,y,z,yaw,pitch" for --spawn: the walker `distance` in front of `item`'s face, looking at its middle.
std::string spawnBefore(const rv::world::CollectionItem& item, float distance) {
    const glm::vec3 local(item.slotBox.center().x, item.slotBox.center().y, item.slotBox.max.z);
    const glm::vec3 center = glm::vec3(item.slotFrame * glm::vec4(local, 1.0f));
    glm::vec3 out = glm::vec3(item.slotFrame * glm::vec4(0.0f, 0.0f, 1.0f, 0.0f));
    out.y = 0.0f;
    out = glm::normalize(out);
    const glm::vec3 eye(center.x + distance * out.x, 1.65f, center.z + distance * out.z);
    const glm::vec3 look = center - eye;
    const double yaw = rv::core::radiansToDegrees(std::atan2(look.x, -look.z));
    const double pitch = rv::core::radiansToDegrees(std::atan2(look.y, std::sqrt(look.x * look.x + look.z * look.z)));
    return fmt::format("{:.4f},0,{:.4f},{:.3f},{:.3f}", eye.x, eye.z, yaw, pitch);
}

/// --spawn for `item`'s Focus pose (the reticle on it, as world_inspect.json expects), for the default window.
std::string focusSpawn(const rv::world::CollectionItem& item, const rv::scene::CameraSetup& lens) {
    rv::app::FocusViewer viewer;
    viewer.lens = lens.lens;
    viewer.aspect = 1280.0f / 720.0f;
    const rv::scene::WalkerPose pose = rv::app::focusPoseFor(item, viewer);
    return fmt::format("{:.5f},{:.5f},{:.5f},{:.4f},{:.4f}", pose.feet.x, pose.feet.y, pose.feet.z,
                       rv::core::radiansToDegrees(pose.yaw), rv::core::radiansToDegrees(pose.pitch));
}

const rv::app::PhaseRecord* phaseOf(const rv::app::RunReport& report, std::string_view name) {
    for (const rv::app::PhaseRecord& phase : report.phases) {
        if (phase.name == name) {
            return &phase;
        }
    }
    return nullptr;
}

rv::app::TimeSummary sectionOf(const rv::app::RunReport& report, std::string_view name) {
    for (const rv::app::CpuSectionRecord& record : report.cpuSections) {
        if (record.name == name) {
            return record.times;
        }
    }
    return rv::app::TimeSummary{};
}

/// What every run of the room must show: the room, every item drawn and resident, no hard finding.
void requireRoom(const rv::app::RunReport& report, std::string_view name) {
    INFO(name);
    const rv::app::WorldRecord& world = report.world;
    REQUIRE(world.bound);
    CHECK(world.state == "room");
    CHECK(world.hardFindings == 0);
    // Every placement of the floor shown drawn (I4: the plan may hold other floors, as after the refresh, whose building
    // update enters every other system of the library).
    std::uint64_t floorPlacements = world.placements;
    for (const rv::app::WorldFloorRecord& floor : world.floors) {
        if (floor.id == world.floor) {
            floorPlacements = floor.placed;
        }
    }
    CHECK(world.items == floorPlacements);
    CHECK(world.skipped == 0);
    const rv::app::ResidencyRecord& residency = report.residency;
    REQUIRE(residency.bound);
    CHECK(residency.frontsResident == residency.items);
    CHECK(residency.spinesResident == residency.items);
    CHECK(residency.failed == 0);
}

void printResidency(std::string_view name, const rv::app::RunReport& report) {
    const rv::app::WorldRecord& world = report.world;
    const rv::app::ResidencyRecord& residency = report.residency;
    const rv::app::TimeSummary frames = report.frameTimes.times.value_or(rv::app::TimeSummary{});
    const rv::app::PhaseRecord* load = phaseOf(report, "coverLoad");
    const rv::app::TimeSummary uploads = sectionOf(report, "layerUploads");
    std::cout << fmt::format(
        "Real snes room 17b: {}: outcome {} (change {}, rebuild {}), plan written {}; snapshot {} (done {} after the "
        "process started), room job {}, commit {}, snapshot to commit {}, room shown {} after the process started; "
        "every layer resident {} after the room showed ({} items, the {} in view after {}); cache {} hits, {} misses, "
        "{} stored, {} cover failures; coverLoad {} frames p50 {:.2f} ms p99 {:.2f} ms; run frames p50 {:.2f} ms p99 "
        "{:.2f} ms max {:.2f} ms; uploads p99 {:.3f} ms max {:.3f} ms\n",
        name, world.outcome, world.changeOutcome.empty() ? "-" : world.changeOutcome,
        world.rebuild.empty() ? "-" : world.rebuild, world.planWritten, ms(world.snapshotMs), ms(world.snapshotAtMs),
        ms(world.buildMs), ms(world.commitMs), ms(world.snapshotToCommitMs), ms(world.committedAtMs),
        ms(residency.residentAfterShownMs), residency.items, residency.viewItems,
        ms(residency.viewResidentAfterShownMs), residency.cacheHits, residency.cacheMisses, residency.cacheStored,
        residency.coverFailures, load ? load->frames : 0, load && load->times ? load->times->p50Ms : 0.0,
        load && load->times ? load->times->p99Ms : 0.0, frames.p50Ms, frames.p99Ms, frames.maxMs, uploads.p99Ms,
        uploads.maxMs);
}

} // namespace

TEST_CASE("app/room: the real snes room, its plan reproduced, its covers cold and warm, the benchmark path",
          "[.real-library][real-library][room-real]") {
    const auto home = pathVariable(L"RV_REAL_ESDE_HOME");
    const auto install = pathVariable(L"RV_REAL_ESDE_INSTALL");
    const auto romRoot = pathVariable(L"RV_REAL_ROM_ROOT");
    const auto mediaRoot = pathVariable(L"RV_REAL_MEDIA_ROOT");
    if (!home || !install || !romRoot || !mediaRoot) {
        SKIP("set RV_REAL_ESDE_HOME, RV_REAL_ESDE_INSTALL, RV_REAL_ROM_ROOT and RV_REAL_MEDIA_ROOT "
             "(scripts/real-library.ps1 does)");
    }
    rv::catalog::LibraryProfile profile;
    profile.id = rv::catalog::LibraryId::parse("real").value();
    profile.esdeHome = *home;
    profile.installFolder = *install;
    profile.romRoot = *romRoot;
    profile.mediaRoot = *mediaRoot;

    // A new data directory under the build tree, never in the library: library.json and the snes system imported.
    const fs::path folder = rv::test::testProgramsDir().parent_path() / "real-library" / "room";
    std::error_code removed;
    fs::remove_all(folder, removed);
    REQUIRE_FALSE(removed);
    const fs::path data = folder / "data";
    REQUIRE(rv::esde::writeLibraryFile(data / "config" / std::string(rv::esde::kLibraryFileName), profile,
                                       rv::esde::ExistingFile::Replace)
                .has_value());
    const auto importStart = std::chrono::steady_clock::now();
    const rv::catalog::ImportResult first = importSnes(profile, data);
    const double firstImportMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - importStart).count();
    const fs::path planFile = rv::world::worldFiles(data, profile.id).plan;

    const std::vector<std::string> unpaced{"--no-vsync",          "--width", "1920", "--height", "1080",
                                           "--exit-after-frames", "--frames"};
    const auto unpacedFor = [&](std::string_view frames) {
        std::vector<std::string> arguments = unpaced;
        arguments.emplace_back(frames);
        return arguments;
    };

    // Cold: the plan made and every cover derived anew (7 s on a quiet machine, step 12; the frame limits leave room for a
    // loaded one, about 60 s at 3 ms a frame).
    const rv::app::RunReport cold = runRoom(folder, data, "cold", unpacedFor("20000"));
    requireRoom(cold, "cold");
    CHECK(cold.world.outcome == "generated");
    CHECK(cold.world.planWritten);
    const std::string firstFileHash = fileSha256(planFile);

    // Again: the same catalog, the plan generated a second time from nothing, warm.
    fs::remove_all(data / "worlds", removed);
    REQUIRE_FALSE(removed);
    const rv::app::RunReport again = runRoom(folder, data, "again", unpacedFor("8000"));
    requireRoom(again, "again");
    CHECK(again.world.outcome == "generated");
    CHECK(again.world.planWritten);
    CHECK(again.world.planDigest == cold.world.planDigest);
    const std::string secondFileHash = fileSha256(planFile);
    CHECK(secondFileHash == firstFileHash);
    CHECK(again.residency.cacheStored == 0);

    // A second import of the same library, then a restart: the stored plan reconciled, nothing moved.
    const auto secondStart = std::chrono::steady_clock::now();
    const rv::catalog::ImportResult second = importSnes(profile, data);
    const double secondImportMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - secondStart).count();
    const rv::app::RunReport restart = runRoom(folder, data, "restart", unpacedFor("8000"));
    requireRoom(restart, "restart");
    CHECK((restart.world.outcome == "unchanged" || restart.world.outcome == "updated"));
    if (!restart.world.rebuild.empty()) {
        CHECK(restart.world.rebuild == "no-rebuild");
    }
    CHECK(restart.world.planDigest == cold.world.planDigest);
    CHECK(fileSha256(planFile) == firstFileHash);
    REQUIRE(restart.world.preview.has_value());
    if (restart.world.reconcile) {
        CHECK(restart.world.reconcile->moved == 0);
        CHECK(restart.world.reconcile->added == 0);
        CHECK(restart.world.reconcile->removed == 0);
    }
    CHECK(restart.residency.cacheStored == 0);

    // The benchmark path.
    const rv::app::RunReport bench =
        runRoom(folder, data, "benchmark",
                {"--width", "1920", "--height", "1080", "--no-vsync", "--exit-after-frames", "--frames", "1270",
                 "--fixed-dt", "0.016667", "--input-script",
                 rv::core::pathToUtf8(rv::core::pathFromUtf8(RV_TEST_SOURCE_DIR) / "tests" / "input_scripts" /
                                      "bench_snes_room.json")});
    requireRoom(bench, "benchmark");
    const rv::app::PhaseRecord* benchmark = phaseOf(bench, "benchmark");
    REQUIRE(benchmark != nullptr);
    CHECK(benchmark->frames == 1230);
    REQUIRE(benchmark->times.has_value());

    // Screenshots for the review, under the build tree only.
    auto stored = rv::world::readStoredPlan(planFile);
    REQUIRE(stored.has_value());
    const rv::world::RoomId room = rv::world::RoomId::parse(cold.world.room).value();
    auto sceneData = rv::world::loadSceneData(cold.assetRoot, stored->plan, room);
    REQUIRE(sceneData.has_value());
    auto scene = rv::world::toSceneDescription(stored->plan, room, *sceneData);
    REQUIRE(scene.has_value());
    auto layout = rv::world::collectionLayout(stored->plan, room, *sceneData, {});
    REQUIRE(layout.has_value());
    const auto firstIn = [&](rv::world::Section section) {
        return std::find_if(layout->items.begin(), layout->items.end(),
                            [&](const rv::world::CollectionItem& item) { return item.section == section; });
    };
    const auto band = firstIn(rv::world::Section::Band);
    REQUIRE(band != layout->items.end());
    // A spine-out level of a side wall: the item in the middle of the spine section.
    std::vector<const rv::world::CollectionItem*> spines;
    for (const rv::world::CollectionItem& item : layout->items) {
        if (item.section == rv::world::Section::Spine) {
            spines.push_back(&item);
        }
    }
    REQUIRE_FALSE(spines.empty());
    const rv::world::CollectionItem& spine = *spines[spines.size() / 2];
    const std::vector<std::string> lockstep{"--fixed-dt", "0.016667", "--exit-after-frames", "--frames"};
    struct Shot {
        std::string name;
        std::string frames;
        std::vector<std::string> extra;
    };
    const std::vector<Shot> shots{
        {"doorway", "8", {}},
        {"shelf_spines_100", "8", {"--spawn", spawnBefore(spine, 1.0f)}},
        {"shelf_band_120", "8", {"--spawn", spawnBefore(*band, 1.2f)}},
        {"inspect",
         "62",
         {"--spawn", focusSpawn(*band, scene->camera), "--input-script",
          rv::core::pathToUtf8(rv::core::pathFromUtf8(RV_TEST_SOURCE_DIR) / "tests" / "input_scripts" /
                               "world_inspect.json")}},
    };
    for (const Shot& shot : shots) {
        std::vector<std::string> arguments = lockstep;
        arguments.push_back(shot.frames);
        arguments.insert(arguments.end(), shot.extra.begin(), shot.extra.end());
        const fs::path png = folder / (shot.name + ".png");
        arguments.insert(arguments.end(), {"--screenshot", rv::core::pathToUtf8(png)});
        const rv::app::RunReport report = runRoom(folder, data, shot.name, arguments);
        requireRoom(report, shot.name);
        CHECK(report.screenshot.has_value());
        if (shot.name == "inspect") {
            CHECK(report.world.interaction.state == "inspect");
        }
        std::cout << fmt::format("Real snes room 17b: screenshot {}: {}\n", shot.name, rv::core::pathToUtf8(png));
    }

    // The whole library refreshed while the room shows, the quick catalog open (I3 step 18: NFR-01 with the room, the
    // refresh's frame times). The refresh imports every system of the library into this catalog (the snes rows as
    // they are): the room's live update keeps every item where it stands. Since I4 step 6b the building's update after
    // it enters every other system (new rooms and floors), so the plan file changes; the SNES room's content (its
    // record, fixtures, placements, dressing, lights and unplaced games: ADR-0013 §5's content digest) must not.
    const auto snesContent = [&] {
        auto stored = rv::world::readStoredPlan(planFile);
        REQUIRE(stored.has_value());
        auto digest = rv::world::roomContentDigest(stored->plan, rv::world::RoomId::parse("room:snes").value());
        REQUIRE(digest.has_value());
        return *digest;
    };
    const std::string snesBefore = snesContent();
    std::vector<std::string> refreshArguments = unpacedFor("20000");
    refreshArguments.insert(refreshArguments.end(),
                            {"--refresh-library", "--input-script",
                             rv::core::pathToUtf8(rv::core::pathFromUtf8(RV_TEST_SOURCE_DIR) / "tests" /
                                                  "input_scripts" / "catalog_open.json")});
    const rv::app::RunReport refresh = runRoom(folder, data, "refresh", refreshArguments);
    requireRoom(refresh, "refresh");
    REQUIRE(refresh.catalog.has_value());
    const rv::app::CatalogRecord& catalog = *refresh.catalog;
    CHECK(catalog.firstPageMs.has_value());
    REQUIRE(catalog.refreshes.size() == 1);
    CHECK(catalog.refreshes.front().outcome == "done");
    const rv::app::PhaseRecord* refreshing = phaseOf(refresh, "refresh");
    REQUIRE(refreshing != nullptr);
    CHECK_FALSE(refreshing->open);
    CHECK_FALSE(refresh.world.liveUpdates.empty());
    for (const rv::app::WorldLiveUpdateRecord& update : refresh.world.liveUpdates) {
        CHECK(update.moved == 0);
        CHECK(update.removed == 0);
    }
    CHECK(snesContent() == snesBefore);
    // The room's own reconciliation after the refresh (the stored room previewed first) moved nothing either.
    if (refresh.world.reconcile) {
        CHECK(refresh.world.reconcile->moved == 0);
    }

    // The figures.
    const rv::app::WorldRecord& world = cold.world;
    std::cout << fmt::format(
        "Real snes room 17b: {} placements ({} featured, {} band, {} spine), {} unplaced ({} duplicate titles, {} "
        "without a cover, {} unavailable, {} capacity); {} scene objects, {} lights, {} clusters; plan {} (file "
        "SHA-256 {} both times)\n",
        world.placements, world.featured, world.band, world.spine, world.unplaced, world.duplicateTitle, world.noCover,
        world.unavailable, world.capacity, world.objects, world.lights, world.clusters, world.planDigest,
        firstFileHash);
    std::cout << fmt::format("Real snes room 17b: imports: the first {:.0f} ms ({} entries, {} identities written), "
                             "the second {:.0f} ms ({} identities, {} entries written, {} removed)\n",
                             firstImportMs, first.statistics.entries, first.statistics.identitiesWritten,
                             secondImportMs, second.statistics.identitiesWritten, second.statistics.entriesWritten,
                             second.statistics.entriesRemoved);
    printResidency("cold", cold);
    printResidency("again", again);
    printResidency("restart", restart);
    if (restart.world.reconcile) {
        const rv::app::WorldReconcileRecord& reconcile = *restart.world.reconcile;
        std::cout << fmt::format("Real snes room 17b: restart's reconciliation: {} at frame {}: {} kept, {} moved, {} "
                                 "flags changed, {} added, {} removed, covers reloaded {}\n",
                                 reconcile.kind, reconcile.frame, reconcile.kept, reconcile.moved,
                                 reconcile.flagsChanged, reconcile.added, reconcile.removed, reconcile.coversReloaded);
    }
    const rv::app::TimeSummary& times = *benchmark->times;
    std::string passes;
    for (const rv::app::GpuPassRecord& pass : benchmark->gpuPasses) {
        passes += fmt::format("{}{} p50 {:.3f} p99 {:.3f} max {:.3f} ms", passes.empty() ? "" : "; ", pass.name,
                              pass.times.p50Ms, pass.times.p99Ms, pass.times.maxMs);
    }
    std::cout << fmt::format(
        "Real snes room 17b: benchmark path: {} frames, frame p50 {:.2f} ms, p95 {:.2f} ms, p99 {:.2f} ms, max {:.2f} "
        "ms; GPU {}; arrays {:.1f} MiB ({}), GPU memory peak {:.1f} MiB; the walker ended at {:.2f},{:.2f},{:.2f}\n",
        times.count, times.p50Ms, times.p95Ms, times.p99Ms, times.maxMs, passes,
        static_cast<double>(bench.residency.arrayBytes) / (1024.0 * 1024.0), bench.residency.format,
        static_cast<double>(bench.gpuMemory.peakTotalBytes) / (1024.0 * 1024.0),
        bench.finalCamera ? bench.finalCamera->feet.x : 0.0f, bench.finalCamera ? bench.finalCamera->feet.y : 0.0f,
        bench.finalCamera ? bench.finalCamera->feet.z : 0.0f);
    const rv::app::RefreshRecord& refreshed = catalog.refreshes.front();
    const rv::app::TimeSummary refreshTimes = refreshing->times.value_or(rv::app::TimeSummary{});
    std::cout << fmt::format(
        "Real snes room 17b: refresh: the quick catalog's first page {} after the process started ({} games); the "
        "refresh {} in {} ({} entries, {} added, {} removed, {} changed), the refresh phase {} frames p50 {:.2f} ms p99 "
        "{:.2f} ms max {:.2f} ms; {} live update(s) of the room, the last {} ({} kept, {} moved)\n",
        ms(catalog.firstPageMs), catalog.games, refreshed.outcome, ms(refreshed.ms), refreshed.entries,
        refreshed.added, refreshed.removed, refreshed.changed, refreshing->frames, refreshTimes.p50Ms,
        refreshTimes.p99Ms, refreshTimes.maxMs, refresh.world.liveUpdates.size(),
        refresh.world.liveUpdates.empty() ? std::string("-") : refresh.world.liveUpdates.back().kind,
        refresh.world.liveUpdates.empty() ? 0 : refresh.world.liveUpdates.back().kept,
        refresh.world.liveUpdates.empty() ? 0 : refresh.world.liveUpdates.back().moved);
}
