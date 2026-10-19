// The real SNES room in retroverse.exe (I3 step 10b). Hidden ([.real-library]): ctest never runs it; scripts/real-
// library.ps1 does, with ES-DE closed, read-only snapshots of the library before and after, and the folders in the
// RV_REAL_* variables. The system snes alone is imported into a new catalog under the build tree
// (build/<preset>/real-library/world/data, library.json naming the real ES-DE home, install, ROM and media roots), then
// retroverse.exe of this build (hidden, frame-limited) opens its room three times on that data directory: a cold start
// (no plan, no cached texture: the plan generated and written, every cover made), a warm one (the plan reused
// unchanged, every layer from the cache), and a measurement at the doorway (--no-vsync, 600 frames, after the room and
// its layers are in: lockstep). The figures are printed for docs/i3/step-10b.md (the lines starting "Real snes room:"):
// the placements and unplaced games, the snapshot on the catalog worker, the room's job, the commit, from the snapshot
// to the commit, the layers resident after the room showed, cold and warm, the cache's counters, the frame times at the
// doorway and the arrays' memory. The checks hold whatever the collection holds (this machine's figures are not
// versioned): every run clean, the room shown with every item's layers, the warm start's plan unchanged.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <windows.h>

#include <fmt/format.h>

#include <rv/app/run_report.hpp>
#include <rv/catalog/import_service.hpp>
#include <rv/catalog/reconciliation.hpp>
#include <rv/catalog_db/catalog_store.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/catalog_db/migrator.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/esde/discovery.hpp>
#include <rv/esde/library_source.hpp>
#include <rv/esde/media_resolver.hpp>
#include <rv/esde/settings.hpp>

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
    arguments.insert(arguments.end(), {"--hidden", "--data-dir", rv::core::pathToUtf8(data), "--run-report",
                                       rv::core::pathToUtf8(report)});
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

} // namespace

TEST_CASE("app/world: the real snes room in retroverse.exe", "[.real-library][real-library][world]") {
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
    const rv::catalog::SystemName snes = rv::catalog::SystemName::parse("snes").value();

    // A new data directory under the build tree, never in the library: library.json and the snes system imported.
    const fs::path folder = rv::test::testProgramsDir().parent_path() / "real-library" / "world";
    std::error_code removed;
    fs::remove_all(folder, removed);
    REQUIRE_FALSE(removed);
    const fs::path data = folder / "data";
    REQUIRE(rv::esde::writeLibraryFile(data / "config" / std::string(rv::esde::kLibraryFileName), profile,
                                       rv::esde::ExistingFile::Replace)
                .has_value());
    {
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
        importOptions.systems = {snes};
        const auto imported = service.run(profile, importOptions);
        INFO((imported ? std::string() : imported.error().toString()));
        REQUIRE(imported.has_value());
        REQUIRE(imported->statistics.mediaUnknown == 0);
    }

    const std::vector<std::string> lockstep{"--exit-after-frames", "--frames", "60", "--fixed-dt", "0.016667"};
    const rv::app::RunReport cold = runRoom(folder, data, "cold", lockstep);
    const rv::app::RunReport warm = runRoom(folder, data, "warm", lockstep);
    const rv::app::RunReport doorway =
        runRoom(folder, data, "doorway",
                {"--no-vsync", "--width", "1920", "--height", "1080", "--exit-after-frames", "--frames", "600",
                 "--fixed-dt", "0.016667", "--screenshot", rv::core::pathToUtf8(folder / "doorway.png")});

    for (const rv::app::RunReport* report : {&cold, &warm, &doorway}) {
        const rv::app::WorldRecord& world = report->world;
        REQUIRE(world.bound);
        CHECK(world.state == "room");
        CHECK(world.hardFindings == 0);
        CHECK(world.items == world.placements);
        CHECK(world.skipped == 0);
        const rv::app::ResidencyRecord& residency = report->residency;
        REQUIRE(residency.bound);
        CHECK(residency.frontsResident == residency.items);
        CHECK(residency.spinesResident == residency.items);
        CHECK(residency.failed == 0);
    }
    CHECK(cold.world.outcome == "generated");
    CHECK(cold.world.planWritten);
    CHECK(warm.world.outcome == "unchanged");
    CHECK_FALSE(warm.world.planWritten);
    CHECK(warm.world.planDigest == cold.world.planDigest);
    CHECK(doorway.world.planDigest == cold.world.planDigest);
    CHECK(warm.residency.cacheStored == 0);

    const rv::app::WorldRecord& world = cold.world;
    std::cout << fmt::format(
        "Real snes room: {} placements ({} featured, {} band, {} spine), {} unplaced ({} duplicate "
        "titles, {} without a cover, {} unavailable, {} capacity); {} scene objects, {} lights, {} "
        "clusters\n",
        world.placements, world.featured, world.band, world.spine, world.unplaced, world.duplicateTitle, world.noCover,
        world.unavailable, world.capacity, world.objects, world.lights, world.clusters);
    for (const auto& [name, report] :
         {std::pair<const char*, const rv::app::RunReport*>{"cold", &cold}, {"warm", &warm}}) {
        const rv::app::WorldRecord& run = report->world;
        const rv::app::ResidencyRecord& residency = report->residency;
        std::cout << fmt::format(
            "Real snes room: {} start: snapshot {} on the catalog worker (done {} after the process started), room job "
            "{}, commit {}, snapshot to commit {}, room shown {} after the process started; every layer resident {} "
            "after the room showed ({} covers, {} labels, {} cover failures); cache {} hits, {} misses, {} stored\n",
            name, ms(run.snapshotMs), ms(run.snapshotAtMs), ms(run.buildMs), ms(run.commitMs),
            ms(run.snapshotToCommitMs), ms(run.committedAtMs), ms(residency.residentAfterShownMs), residency.covers,
            residency.labels, residency.coverFailures, residency.cacheHits, residency.cacheMisses,
            residency.cacheStored);
    }
    const rv::app::TimeSummary times = doorway.frameTimes.times.value_or(rv::app::TimeSummary{});
    std::cout << fmt::format(
        "Real snes room: at the doorway (1920 x 1080, --no-vsync, 600 frames): frame p50 {:.2f} ms, p95 {:.2f} ms, "
        "p99 {:.2f} ms, max {:.2f} ms; collection arrays {:.1f} MiB ({}), GPU memory at its peak {:.1f} MiB\n",
        times.p50Ms, times.p95Ms, times.p99Ms, times.maxMs,
        static_cast<double>(doorway.residency.arrayBytes) / (1024.0 * 1024.0), doorway.residency.format,
        static_cast<double>(doorway.gpuMemory.peakTotalBytes) / (1024.0 * 1024.0));
    if (doorway.lastView) {
        std::cout << fmt::format("Real snes room: the doorway's view: {} draw calls ({} of the static groups), {} "
                                 "instances, {} clusters, {} culled\n",
                                 doorway.lastView->drawCalls, doorway.lastView->staticDrawCalls,
                                 doorway.lastView->instances, doorway.lastView->clusters,
                                 doorway.lastView->culledClusters);
    }
}
