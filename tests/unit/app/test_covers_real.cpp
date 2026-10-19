// The covers of the real SNES room in retroverse.exe (I3 step 12). Hidden ([.real-library]): ctest never runs it;
// scripts/real-library.ps1 does, with ES-DE closed, read-only snapshots of the library before and after, and the
// folders in the RV_REAL_* variables. The system snes alone is imported into a new catalog under the build tree
// (build/<preset>/real-library/covers/data), then retroverse.exe of this build (hidden, frame-limited) opens its room:
// a first lockstep run makes the plan (its layers in RGBA8, so that the BC1 cache stays cold); a cold run and a warm
// one without a lockstep (1920 x 1080, --no-vsync: the frames never wait, the covers come in the loader's order within
// the upload budget), whose figures are printed (the lines starting "Real snes covers:") for docs/i3/step-12.md: every
// layer resident after the room showed (ADR-0011 §9: 10 s cold, 1.0 s warm for the 890 covers), the frame times over
// the coverLoad phase (NFR-04: p99 at most 33.3 ms), the uploads per frame, the cache's counters, the arrays' and the
// hero textures' memory; then close-ups for the review of BC1 on real covers (the hovered item shows its hero texture,
// the full-resolution cover, beside its BC1 neighbours; the same views in RGBA8), written under the build tree only.
// The checks hold whatever the collection holds (this machine's figures are not versioned): every run clean, every
// item's layers resident, the warm run served from the cache.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <windows.h>

#include <fmt/format.h>
#include <glm/glm.hpp>

#include <rv/app/run_report.hpp>
#include <rv/catalog/import_service.hpp>
#include <rv/catalog/reconciliation.hpp>
#include <rv/catalog_db/catalog_store.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/catalog_db/migrator.hpp>
#include <rv/core/math.hpp>
#include <rv/core/paths.hpp>
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

rv::app::TimeSummary sectionOf(const rv::app::RunReport& report, const std::string& name) {
    for (const rv::app::CpuSectionRecord& record : report.cpuSections) {
        if (record.name == name) {
            return record.times;
        }
    }
    return rv::app::TimeSummary{};
}

std::optional<rv::app::TimeSummary> phaseOf(const rv::app::RunReport& report, const std::string& name) {
    for (const rv::app::PhaseRecord& record : report.phases) {
        if (record.name == name) {
            return record.times;
        }
    }
    return std::nullopt;
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

} // namespace

TEST_CASE("app/covers: the real snes room's covers, cold and warm", "[.real-library][real-library][covers-room]") {
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
    const fs::path folder = rv::test::testProgramsDir().parent_path() / "real-library" / "covers";
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

    // The plan, its layers in RGBA8 (the BC1 entries stay cold).
    const rv::app::RunReport plan =
        runRoom(folder, data, "plan",
                {"--exit-after-frames", "--frames", "2", "--fixed-dt", "0.016667", "--no-texture-compression"});
    REQUIRE(plan.world.state == "room");
    const std::vector<std::string> unpaced{"--no-vsync",          "--width",  "1920", "--height", "1080",
                                           "--exit-after-frames", "--frames", "4000"};
    // The cold run makes the plan again (no preview of a stored one, I3 step 10c); the warm run starts as a restart
    // does (the stored room shown with its covers from the cache, then reconciled).
    fs::remove_all(data / "worlds", removed);
    REQUIRE_FALSE(removed);
    const rv::app::RunReport cold = runRoom(folder, data, "cold", unpaced);
    const rv::app::RunReport warm = runRoom(folder, data, "warm", unpaced);

    for (const auto& [name, report] :
         {std::pair<const char*, const rv::app::RunReport*>{"cold", &cold}, {"warm", &warm}}) {
        INFO(name);
        REQUIRE(report->world.state == "room");
        CHECK(report->world.planDigest == plan.world.planDigest);
        const rv::app::ResidencyRecord& residency = report->residency;
        REQUIRE(residency.bound);
        CHECK(residency.format == "bc1-srgb");
        CHECK(residency.frontsResident == residency.items);
        CHECK(residency.spinesResident == residency.items);
        CHECK(residency.failed == 0);
        CHECK(residency.maxLayersPerRound <= 64);
        const rv::app::TimeSummary uploads = sectionOf(*report, "layerUploads");
        const std::optional<rv::app::TimeSummary> load = phaseOf(*report, "coverLoad");
        const rv::app::TimeSummary frames = report->frameTimes.times.value_or(rv::app::TimeSummary{});
        std::cout << fmt::format(
            "Real snes covers: {}: {} items, every layer resident {} after the room showed (the {} in view after {}; "
            "{} of the first in view; the rest's distance quarters {:.2f}, {:.2f}, {:.2f}, {:.2f} m); fronts {} "
            "covers, {} labels ({} failures); cache {} hits, {} misses, {} stored; coverLoad phase {} frames p50 "
            "{:.2f} ms p99 {:.2f} ms max {:.2f} ms; run frames p50 {:.2f} ms p99 {:.2f} ms; uploads {} frames p50 "
            "{:.3f} ms p99 {:.3f} ms max {:.3f} ms, {} rounds over 1 ms (longest {:.2f} ms), at most {} layers; arrays "
            "{:.1f} MiB, GPU memory peak {:.1f} MiB\n",
            name, residency.items, ms(residency.residentAfterShownMs), residency.viewItems,
            ms(residency.viewResidentAfterShownMs), residency.inViewAmongFirst, residency.arrivalDistanceQuarters[0],
            residency.arrivalDistanceQuarters[1], residency.arrivalDistanceQuarters[2],
            residency.arrivalDistanceQuarters[3], residency.covers, residency.labels, residency.coverFailures,
            residency.cacheHits, residency.cacheMisses, residency.cacheStored, load ? load->count : 0,
            load ? load->p50Ms : 0.0, load ? load->p99Ms : 0.0, load ? load->maxMs : 0.0, frames.p50Ms, frames.p99Ms,
            uploads.count, uploads.p50Ms, uploads.p99Ms, uploads.maxMs, residency.overBudgetRounds,
            residency.maxRoundMs, residency.maxLayersPerRound,
            static_cast<double>(residency.arrayBytes) / (1024.0 * 1024.0),
            static_cast<double>(report->gpuMemory.peakTotalBytes) / (1024.0 * 1024.0));
    }
    CHECK(warm.residency.cacheStored == 0);
    CHECK(warm.residency.cacheMisses == warm.residency.coverFailures);

    // Close-ups for the review of BC1 on real covers: a face-out item of the hero band at 0.6 m and 1.5 m (the hovered
    // one shows its hero texture), a side wall's spine-out level at 1 m; in BC1 and in RGBA8.
    auto stored = rv::world::readStoredPlan(rv::world::worldFiles(data, profile.id).plan);
    REQUIRE(stored.has_value());
    const rv::world::RoomId room = rv::world::RoomId::parse(plan.world.room).value();
    auto sceneData = rv::world::loadSceneData(plan.assetRoot, stored->plan, room);
    REQUIRE(sceneData.has_value());
    auto layout = rv::world::collectionLayout(stored->plan, room, *sceneData, {});
    REQUIRE(layout.has_value());
    const auto first = [&](rv::world::Section section) {
        return std::find_if(layout->items.begin(), layout->items.end(),
                            [&](const rv::world::CollectionItem& item) { return item.section == section; });
    };
    const auto band = first(rv::world::Section::Band);
    const auto spine = first(rv::world::Section::Spine);
    REQUIRE(band != layout->items.end());
    REQUIRE(spine != layout->items.end());
    const std::vector<std::pair<std::string, std::string>> views{{"band_060", spawnBefore(*band, 0.6f)},
                                                                 {"band_150", spawnBefore(*band, 1.5f)},
                                                                 {"spines_100", spawnBefore(*spine, 1.0f)}};
    for (const auto& [view, spawn] : views) {
        for (const bool rgba8 : {false, true}) {
            const std::string name = view + (rgba8 ? "_rgba8" : "_bc1");
            std::vector<std::string> arguments{"--exit-after-frames",
                                               "--frames",
                                               "8",
                                               "--fixed-dt",
                                               "0.016667",
                                               "--spawn",
                                               spawn,
                                               "--screenshot",
                                               rv::core::pathToUtf8(folder / (name + ".png"))};
            if (rgba8) {
                arguments.push_back("--no-texture-compression");
            }
            const rv::app::RunReport shot = runRoom(folder, data, name, arguments);
            CHECK(shot.world.state == "room");
            CHECK(shot.residency.frontsResident == shot.residency.items);
            if (view != "spines_100") {
                std::cout << fmt::format("Real snes covers: {}: hero textures {} loaded, shown {}, {:.1f} MiB\n", name,
                                         shot.residency.hero.loaded,
                                         shot.residency.hero.shownGame.empty() ? "none" : "one",
                                         static_cast<double>(shot.residency.hero.bytes) / (1024.0 * 1024.0));
            }
        }
    }
}
